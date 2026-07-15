#define _GNU_SOURCE
#include "interp.h"
#include "parser.h"
#include "pcre_wrap.h"
#include "json.h"
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

/* ================================================================== */
/* small helpers                                                      */
/* ================================================================== */

#define ARG(name, idx) vrl_arg(a, name, idx)

static int arg_bytes(vrl_call_args *a, const char *name, int idx,
		     const char **data, size_t *len, char **err)
{
	vrl_value *v = vrl_arg(a, name, idx);
	if (!v || v->type != VRL_BYTES) {
		*err = vrl_errf("expected string argument '%s'", name ? name : "value");
		return 0;
	}
	*data = v->u.bytes.data;
	*len = v->u.bytes.len;
	return 1;
}

/* ================================================================== */
/* type predicates                                                    */
/* ================================================================== */

#define IS_FN(fname, TYPE) \
	static vrl_status fname(vrl_call_args *a, vrl_value **out, char **err) { \
		(void)err; vrl_value *v = ARG("value", 0); \
		*out = vrl_boolean(v && v->type == TYPE); return VRL_OK; }

IS_FN(fn_is_null, VRL_NULL)
IS_FN(fn_is_string, VRL_BYTES)
IS_FN(fn_is_integer, VRL_INTEGER)
IS_FN(fn_is_float, VRL_FLOAT)
IS_FN(fn_is_boolean, VRL_BOOLEAN)
IS_FN(fn_is_array, VRL_ARRAY)
IS_FN(fn_is_object, VRL_OBJECT)
IS_FN(fn_is_timestamp, VRL_TIMESTAMP)
IS_FN(fn_is_regex, VRL_REGEX)

/* ================================================================== */
/* type conversion                                                    */
/* ================================================================== */

static vrl_status fn_to_string(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	vrl_value *v = ARG("value", 0);
	size_t len;
	char *s = vrl_value_to_string(v, &len);
	*out = vrl_bytes_take(s, len);
	return VRL_OK;
}

static vrl_status fn_to_int(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v) { *err = vrl_errf("to_int: missing argument"); return VRL_ERR; }
	switch (v->type) {
	case VRL_INTEGER: *out = vrl_integer(v->u.integer); return VRL_OK;
	case VRL_FLOAT: *out = vrl_integer((int64_t)v->u.flt); return VRL_OK;
	case VRL_BOOLEAN: *out = vrl_integer(v->u.boolean ? 1 : 0); return VRL_OK;
	case VRL_TIMESTAMP: *out = vrl_integer((int64_t)v->u.timestamp); return VRL_OK;
	case VRL_BYTES: {
		char *end = NULL;
		long long r = strtoll(v->u.bytes.data, &end, 10);
		if (end == v->u.bytes.data) { *err = vrl_errf("to_int: '%s' is not an integer", v->u.bytes.data); return VRL_ERR; }
		*out = vrl_integer(r); return VRL_OK;
	}
	default: *err = vrl_errf("to_int: cannot convert %s", vrl_type_name(v->type)); return VRL_ERR;
	}
}

static vrl_status fn_to_float(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v) { *err = vrl_errf("to_float: missing argument"); return VRL_ERR; }
	switch (v->type) {
	case VRL_INTEGER: *out = vrl_float((double)v->u.integer); return VRL_OK;
	case VRL_FLOAT: *out = vrl_float(v->u.flt); return VRL_OK;
	case VRL_BOOLEAN: *out = vrl_float(v->u.boolean ? 1.0 : 0.0); return VRL_OK;
	case VRL_TIMESTAMP: *out = vrl_float(v->u.timestamp); return VRL_OK;
	case VRL_BYTES: {
		char *end = NULL;
		double r = strtod(v->u.bytes.data, &end);
		if (end == v->u.bytes.data) { *err = vrl_errf("to_float: '%s' is not a float", v->u.bytes.data); return VRL_ERR; }
		*out = vrl_float(r); return VRL_OK;
	}
	default: *err = vrl_errf("to_float: cannot convert %s", vrl_type_name(v->type)); return VRL_ERR;
	}
}

static vrl_status fn_to_bool(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v) { *err = vrl_errf("to_bool: missing argument"); return VRL_ERR; }
	switch (v->type) {
	case VRL_BOOLEAN: *out = vrl_boolean(v->u.boolean); return VRL_OK;
	case VRL_INTEGER: *out = vrl_boolean(v->u.integer != 0); return VRL_OK;
	case VRL_FLOAT: *out = vrl_boolean(v->u.flt != 0.0); return VRL_OK;
	case VRL_NULL: *out = vrl_boolean(0); return VRL_OK;
	case VRL_BYTES: {
		const char *s = v->u.bytes.data;
		if (!strcasecmp(s, "true") || !strcasecmp(s, "t") || !strcmp(s, "1") || !strcasecmp(s, "yes")) { *out = vrl_boolean(1); return VRL_OK; }
		if (!strcasecmp(s, "false") || !strcasecmp(s, "f") || !strcmp(s, "0") || !strcasecmp(s, "no")) { *out = vrl_boolean(0); return VRL_OK; }
		*err = vrl_errf("to_bool: '%s' is not a boolean", s); return VRL_ERR;
	}
	default: *err = vrl_errf("to_bool: cannot convert %s", vrl_type_name(v->type)); return VRL_ERR;
	}
}

static vrl_status fn_to_timestamp(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v) { *err = vrl_errf("to_timestamp: missing argument"); return VRL_ERR; }
	if (v->type == VRL_TIMESTAMP) { *out = vrl_timestamp(v->u.timestamp); return VRL_OK; }
	if (v->type == VRL_INTEGER) { *out = vrl_timestamp((double)v->u.integer); return VRL_OK; }
	if (v->type == VRL_FLOAT) { *out = vrl_timestamp(v->u.flt); return VRL_OK; }
	if (v->type == VRL_BYTES) {
		double ts;
		if (vrl_parse_timestamp_str(v->u.bytes.data, v->u.bytes.len, &ts) != 0) {
			*err = vrl_errf("to_timestamp: cannot parse '%s'", v->u.bytes.data);
			return VRL_ERR;
		}
		*out = vrl_timestamp(ts); return VRL_OK;
	}
	*err = vrl_errf("to_timestamp: cannot convert %s", vrl_type_name(v->type));
	return VRL_ERR;
}

/* ================================================================== */
/* json                                                               */
/* ================================================================== */

static vrl_status fn_parse_json(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err))
		return VRL_ERR;
	char *jerr = NULL;
	vrl_value *v = vrl_json_decode(s, len, &jerr);
	if (!v) {
		*err = vrl_errf("parse_json: %s", jerr ? jerr : "invalid json");
		free(jerr);
		return VRL_ERR;
	}
	*out = v;
	return VRL_OK;
}

static vrl_status fn_encode_json(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	vrl_value *v = ARG("value", 0);
	char *s = vrl_json_encode(v);
	if (!s) { *out = vrl_bytes_cstr(""); return VRL_OK; }
	*out = vrl_bytes_take(s, strlen(s));
	return VRL_OK;
}

/* ================================================================== */
/* regex                                                              */
/* ================================================================== */

static regex_match *arg_regex(vrl_call_args *a, const char *name, int idx,
			      int *owned, char **err)
{
	vrl_value *v = vrl_arg(a, name, idx);
	*owned = 0;
	if (!v) { *err = vrl_errf("expected regex/pattern argument"); return NULL; }
	if (v->type == VRL_REGEX)
		return v->u.regex;
	if (v->type == VRL_BYTES) {
		regex_match *re = avrl_regex_compile(v->u.bytes.data);
		if (!re) { *err = vrl_errf("invalid regex pattern"); return NULL; }
		*owned = 1;
		return re;
	}
	*err = vrl_errf("expected regex or string pattern");
	return NULL;
}

static vrl_status fn_match(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err))
		return VRL_ERR;
	int owned;
	regex_match *re = arg_regex(a, "pattern", 1, &owned, err);
	if (!re)
		return VRL_ERR;
	int ov[AVRL_OVECCOUNT];
	int cnt = avrl_regex_exec(re, s, len, ov, AVRL_OVECCOUNT, a->ctx->ll);
	if (owned) avrl_regex_free(re);
	*out = vrl_boolean(cnt > 0);
	return VRL_OK;
}

static vrl_status fn_parse_regex(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err))
		return VRL_ERR;
	int owned;
	regex_match *re = arg_regex(a, "pattern", 1, &owned, err);
	if (!re)
		return VRL_ERR;
	int ov[AVRL_OVECCOUNT];
	int cnt = avrl_regex_exec(re, s, len, ov, AVRL_OVECCOUNT, a->ctx->ll);
	if (cnt <= 0) {
		if (owned) avrl_regex_free(re);
		*err = vrl_errf("parse_regex: no match");
		return VRL_ERR;
	}
	vrl_value *obj = vrl_object_new();
	/* numbered groups */
	for (int i = 0; i < cnt; i++) {
		int start = ov[2 * i], end = ov[2 * i + 1];
		char key[16];
		snprintf(key, sizeof(key), "%d", i);
		if (start < 0)
			vrl_object_set_cstr(obj, key, vrl_null());
		else
			vrl_object_set_cstr(obj, key, vrl_bytes(s + start, end - start));
	}
	/* named groups */
	if (re->pcre_name_count > 0 && re->pcre_name_table) {
		const unsigned char *tab = re->pcre_name_table;
		for (int i = 0; i < re->pcre_name_count; i++) {
			const unsigned char *entry = tab + i * re->pcre_name_entry_size;
			int gidx = (entry[0] << 8) | entry[1];
			const char *gname = (const char *)(entry + 2);
			if (gidx < cnt && ov[2 * gidx] >= 0)
				vrl_object_set_cstr(obj, gname, vrl_bytes(s + ov[2 * gidx], ov[2 * gidx + 1] - ov[2 * gidx]));
			else
				vrl_object_set_cstr(obj, gname, vrl_null());
		}
	}
	if (owned) avrl_regex_free(re);
	*out = obj;
	return VRL_OK;
}

/* ================================================================== */
/* timestamps                                                         */
/* ================================================================== */

static vrl_status fn_now(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)a; (void)err;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	*out = vrl_timestamp((double)tv.tv_sec + tv.tv_usec / 1e6);
	return VRL_OK;
}

static vrl_status fn_parse_timestamp(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err))
		return VRL_ERR;
	/* optional 'format' (strptime) */
	vrl_value *fmt = vrl_arg(a, "format", 1);
	char buf[128];
	if (len >= sizeof(buf)) { *err = vrl_errf("parse_timestamp: input too long"); return VRL_ERR; }
	memcpy(buf, s, len); buf[len] = '\0';
	if (fmt && fmt->type == VRL_BYTES) {
		struct tm tmv; memset(&tmv, 0, sizeof(tmv));
		if (!strptime(buf, fmt->u.bytes.data, &tmv)) {
			*err = vrl_errf("parse_timestamp: '%s' does not match format", buf);
			return VRL_ERR;
		}
#if defined(__APPLE__) || defined(__linux__)
		time_t t = timegm(&tmv);
#else
		time_t t = mktime(&tmv);
#endif
		*out = vrl_timestamp((double)t);
		return VRL_OK;
	}
	double ts;
	if (vrl_parse_timestamp_str(s, len, &ts) != 0) {
		*err = vrl_errf("parse_timestamp: cannot parse '%s'", buf);
		return VRL_ERR;
	}
	*out = vrl_timestamp(ts);
	return VRL_OK;
}

static vrl_status fn_format_timestamp(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v || v->type != VRL_TIMESTAMP) {
		*err = vrl_errf("format_timestamp: expected timestamp");
		return VRL_ERR;
	}
	vrl_value *fmt = vrl_arg(a, "format", 1);
	const char *f = (fmt && fmt->type == VRL_BYTES) ? fmt->u.bytes.data : "%Y-%m-%dT%H:%M:%SZ";
	time_t secs = (time_t)v->u.timestamp;
	struct tm tmv;
	gmtime_r(&secs, &tmv);
	char buf[128];
	strftime(buf, sizeof(buf), f, &tmv);
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

/* ================================================================== */
/* string manipulation                                                */
/* ================================================================== */

static vrl_status fn_upcase(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	char *buf = malloc(len + 1);
	for (size_t i = 0; i < len; i++) buf[i] = toupper((unsigned char)s[i]);
	buf[len] = '\0';
	*out = vrl_bytes_take(buf, len);
	return VRL_OK;
}

static vrl_status fn_downcase(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	char *buf = malloc(len + 1);
	for (size_t i = 0; i < len; i++) buf[i] = tolower((unsigned char)s[i]);
	buf[len] = '\0';
	*out = vrl_bytes_take(buf, len);
	return VRL_OK;
}

static vrl_status fn_strip_whitespace(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	size_t b = 0, e = len;
	while (b < e && isspace((unsigned char)s[b])) b++;
	while (e > b && isspace((unsigned char)s[e - 1])) e--;
	*out = vrl_bytes(s + b, e - b);
	return VRL_OK;
}

static vrl_status fn_contains(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	const char *sub; size_t sublen;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	if (!arg_bytes(a, "substring", 1, &sub, &sublen, err)) return VRL_ERR;
	int found = 0;
	if (sublen == 0) found = 1;
	else if (sublen <= len)
		for (size_t i = 0; i + sublen <= len; i++)
			if (!memcmp(s + i, sub, sublen)) { found = 1; break; }
	*out = vrl_boolean(found);
	return VRL_OK;
}

static vrl_status fn_starts_with(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len; const char *sub; size_t sublen;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	if (!arg_bytes(a, "substring", 1, &sub, &sublen, err)) return VRL_ERR;
	*out = vrl_boolean(sublen <= len && !memcmp(s, sub, sublen));
	return VRL_OK;
}

static vrl_status fn_ends_with(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len; const char *sub; size_t sublen;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	if (!arg_bytes(a, "substring", 1, &sub, &sublen, err)) return VRL_ERR;
	*out = vrl_boolean(sublen <= len && !memcmp(s + len - sublen, sub, sublen));
	return VRL_OK;
}

static vrl_status fn_split(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	vrl_value *pat = vrl_arg(a, "pattern", 1);
	vrl_value *lim = vrl_arg(a, "limit", 2);
	int64_t limit = (lim && lim->type == VRL_INTEGER) ? lim->u.integer : -1;
	vrl_value *arr = vrl_array_new();

	if (pat && pat->type == VRL_REGEX) {
		int ov[AVRL_OVECCOUNT];
		size_t pos = 0;
		while (pos <= len) {
			if (limit > 0 && (int64_t)vrl_array_len(arr) == limit - 1)
				break;
			int cnt = pcre_exec(pat->u.regex->regex_compiled, pat->u.regex->pcreExtra,
					    s, (int)len, (int)pos, 0, ov, AVRL_OVECCOUNT);
			if (cnt <= 0)
				break;
			vrl_array_push(arr, vrl_bytes(s + pos, ov[0] - pos));
			size_t np = ov[1];
			if (np == pos) np++; /* avoid infinite loop on empty match */
			pos = np;
		}
		vrl_array_push(arr, vrl_bytes(s + pos, len - pos));
		*out = arr;
		return VRL_OK;
	}

	const char *sep; size_t seplen;
	if (!arg_bytes(a, "pattern", 1, &sep, &seplen, err)) {
		vrl_value_unref(arr);
		return VRL_ERR;
	}
	if (seplen == 0) {
		for (size_t i = 0; i < len; i++)
			vrl_array_push(arr, vrl_bytes(s + i, 1));
		*out = arr;
		return VRL_OK;
	}
	size_t start = 0;
	for (size_t i = 0; i + seplen <= len; ) {
		if (limit > 0 && (int64_t)vrl_array_len(arr) == limit - 1)
			break;
		if (!memcmp(s + i, sep, seplen)) {
			vrl_array_push(arr, vrl_bytes(s + start, i - start));
			i += seplen;
			start = i;
		} else {
			i++;
		}
	}
	vrl_array_push(arr, vrl_bytes(s + start, len - start));
	*out = arr;
	return VRL_OK;
}

static vrl_status fn_join(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *arr = ARG("value", 0);
	if (!arr || arr->type != VRL_ARRAY) { *err = vrl_errf("join: expected array"); return VRL_ERR; }
	vrl_value *sepv = vrl_arg(a, "separator", 1);
	const char *sep = (sepv && sepv->type == VRL_BYTES) ? sepv->u.bytes.data : "";
	size_t seplen = (sepv && sepv->type == VRL_BYTES) ? sepv->u.bytes.len : 0;
	size_t total = 0;
	for (size_t i = 0; i < arr->u.array.len; i++) {
		vrl_value *e = arr->u.array.items[i];
		if (e->type != VRL_BYTES) { *err = vrl_errf("join: array elements must be strings"); return VRL_ERR; }
		total += e->u.bytes.len + (i ? seplen : 0);
	}
	char *buf = malloc(total + 1);
	size_t off = 0;
	for (size_t i = 0; i < arr->u.array.len; i++) {
		if (i) { memcpy(buf + off, sep, seplen); off += seplen; }
		vrl_value *e = arr->u.array.items[i];
		memcpy(buf + off, e->u.bytes.data, e->u.bytes.len);
		off += e->u.bytes.len;
	}
	buf[off] = '\0';
	*out = vrl_bytes_take(buf, off);
	return VRL_OK;
}

static vrl_status fn_replace(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	vrl_value *pat = vrl_arg(a, "pattern", 1);
	const char *with; size_t withlen;
	if (!arg_bytes(a, "with", 2, &with, &withlen, err)) return VRL_ERR;

	char *buf = NULL; size_t cap = 0, off = 0;
	#define PUSH(p, n) do { size_t _n = (n); if (off + _n + 1 > cap) { cap = cap ? cap * 2 : 64; while (cap < off + _n + 1) cap *= 2; buf = realloc(buf, cap); } if (_n) memcpy(buf + off, (p), _n); off += _n; } while (0)

	if (pat && pat->type == VRL_REGEX) {
		int ov[AVRL_OVECCOUNT];
		size_t pos = 0;
		while (pos <= len) {
			int cnt = pcre_exec(pat->u.regex->regex_compiled, pat->u.regex->pcreExtra,
					    s, (int)len, (int)pos, 0, ov, AVRL_OVECCOUNT);
			if (cnt <= 0) break;
			PUSH(s + pos, (size_t)ov[0] - pos);
			PUSH(with, withlen);
			size_t np = ov[1];
			if (np == pos) { if (pos < len) PUSH(s + pos, 1); np = pos + 1; }
			pos = np;
		}
		if (pos < len) PUSH(s + pos, len - pos);
	} else {
		const char *sub; size_t sublen;
		if (!arg_bytes(a, "pattern", 1, &sub, &sublen, err)) { free(buf); return VRL_ERR; }
		if (sublen == 0) { *out = vrl_bytes(s, len); return VRL_OK; }
		size_t i = 0;
		while (i < len) {
			if (i + sublen <= len && !memcmp(s + i, sub, sublen)) {
				PUSH(with, withlen);
				i += sublen;
			} else {
				PUSH(s + i, 1);
				i++;
			}
		}
	}
	if (!buf) buf = malloc(1);
	buf[off] = '\0';
	*out = vrl_bytes_take(buf, off);
	#undef PUSH
	return VRL_OK;
}

static vrl_status fn_slice(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t len;
	if (!arg_bytes(a, "value", 0, &s, &len, err)) return VRL_ERR;
	vrl_value *startv = vrl_arg(a, "start", 1);
	vrl_value *endv = vrl_arg(a, "end", 2);
	int64_t start = (startv && startv->type == VRL_INTEGER) ? startv->u.integer : 0;
	int64_t end = (endv && endv->type == VRL_INTEGER) ? endv->u.integer : (int64_t)len;
	if (start < 0) start += (int64_t)len;
	if (end < 0) end += (int64_t)len;
	if (start < 0) start = 0;
	if (end > (int64_t)len) end = (int64_t)len;
	if (start > end) start = end;
	*out = vrl_bytes(s + start, (size_t)(end - start));
	return VRL_OK;
}

/* ================================================================== */
/* collections                                                        */
/* ================================================================== */

static vrl_status fn_length(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v) { *err = vrl_errf("length: missing argument"); return VRL_ERR; }
	switch (v->type) {
	case VRL_BYTES: *out = vrl_integer((int64_t)v->u.bytes.len); return VRL_OK;
	case VRL_ARRAY: *out = vrl_integer((int64_t)v->u.array.len); return VRL_OK;
	case VRL_OBJECT: *out = vrl_integer((int64_t)v->u.object.len); return VRL_OK;
	default: *err = vrl_errf("length: cannot measure %s", vrl_type_name(v->type)); return VRL_ERR;
	}
}

static vrl_status fn_push(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *arr = ARG("value", 0);
	if (!arr || arr->type != VRL_ARRAY) { *err = vrl_errf("push: expected array"); return VRL_ERR; }
	vrl_value *item = ARG("item", 1);
	vrl_value *copy = vrl_value_clone(arr);
	vrl_array_push(copy, item ? vrl_value_clone(item) : vrl_null());
	*out = copy;
	return VRL_OK;
}

static vrl_status fn_keys(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *obj = ARG("value", 0);
	if (!obj || obj->type != VRL_OBJECT) { *err = vrl_errf("keys: expected object"); return VRL_ERR; }
	vrl_value *arr = vrl_array_new();
	for (size_t i = 0; i < obj->u.object.len; i++)
		vrl_array_push(arr, vrl_bytes(obj->u.object.entries[i].key, obj->u.object.entries[i].key_len));
	*out = arr;
	return VRL_OK;
}

static vrl_status fn_values(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *obj = ARG("value", 0);
	if (!obj || obj->type != VRL_OBJECT) { *err = vrl_errf("values: expected object"); return VRL_ERR; }
	vrl_value *arr = vrl_array_new();
	for (size_t i = 0; i < obj->u.object.len; i++)
		vrl_array_push(arr, vrl_value_clone(obj->u.object.entries[i].val));
	*out = arr;
	return VRL_OK;
}

static vrl_status fn_merge(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *to = ARG("to", 0);
	vrl_value *from = ARG("from", 1);
	if (!to || to->type != VRL_OBJECT || !from || from->type != VRL_OBJECT) {
		*err = vrl_errf("merge: expected two objects");
		return VRL_ERR;
	}
	vrl_value *res = vrl_value_clone(to);
	for (size_t i = 0; i < from->u.object.len; i++) {
		vrl_object_entry *e = &from->u.object.entries[i];
		vrl_object_set(res, e->key, e->key_len, vrl_value_clone(e->val));
	}
	*out = res;
	return VRL_OK;
}

/* ================================================================== */
/* math                                                               */
/* ================================================================== */

#define MATH1(fname, expr) \
	static vrl_status fname(vrl_call_args *a, vrl_value **out, char **err) { \
		vrl_value *v = ARG("value", 0); double x; \
		if (!v || !(v->type == VRL_INTEGER || v->type == VRL_FLOAT)) { *err = vrl_errf("expected number"); return VRL_ERR; } \
		x = v->type == VRL_INTEGER ? (double)v->u.integer : v->u.flt; \
		*out = (expr); return VRL_OK; }

MATH1(fn_abs, v->type == VRL_INTEGER ? vrl_integer(llabs(v->u.integer)) : vrl_float(fabs(x)))
MATH1(fn_floor, vrl_float(floor(x)))
MATH1(fn_ceil, vrl_float(ceil(x)))
MATH1(fn_round, vrl_float(round(x)))

/* ================================================================== */
/* misc                                                               */
/* ================================================================== */

static vrl_status fn_log(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	vrl_value *v = ARG("value", 0);
	char *s = vrl_value_to_string(v, NULL);
	fprintf(stderr, "[vrl log] %s\n", s ? s : "");
	free(s);
	*out = vrl_null();
	return VRL_OK;
}

/* ================================================================== */
/* iteration (closures)                                               */
/* ================================================================== */

static vrl_status fn_map_values(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v || (v->type != VRL_OBJECT && v->type != VRL_ARRAY)) {
		*err = vrl_errf("map_values: expected object or array");
		return VRL_ERR;
	}
	if (!a->closure) { *err = vrl_errf("map_values: requires a closure"); return VRL_ERR; }
	vrl_value *res = v->type == VRL_OBJECT ? vrl_object_new() : vrl_array_new();
	if (v->type == VRL_OBJECT) {
		for (size_t i = 0; i < v->u.object.len; i++) {
			vrl_object_entry *e = &v->u.object.entries[i];
			vrl_value *params[1] = { e->val };
			vrl_value *mapped = NULL;
			vrl_status st = vrl_invoke_closure(a->ctx, a->closure, params, 1, &mapped, err);
			if (st != VRL_OK) { vrl_value_unref(res); return st; }
			vrl_object_set(res, e->key, e->key_len, mapped);
		}
	} else {
		for (size_t i = 0; i < v->u.array.len; i++) {
			vrl_value *params[1] = { v->u.array.items[i] };
			vrl_value *mapped = NULL;
			vrl_status st = vrl_invoke_closure(a->ctx, a->closure, params, 1, &mapped, err);
			if (st != VRL_OK) { vrl_value_unref(res); return st; }
			vrl_array_push(res, mapped);
		}
	}
	*out = res;
	return VRL_OK;
}

static vrl_status fn_filter(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = ARG("value", 0);
	if (!v || (v->type != VRL_OBJECT && v->type != VRL_ARRAY)) {
		*err = vrl_errf("filter: expected object or array");
		return VRL_ERR;
	}
	if (!a->closure) { *err = vrl_errf("filter: requires a closure"); return VRL_ERR; }
	vrl_value *res = v->type == VRL_OBJECT ? vrl_object_new() : vrl_array_new();
	if (v->type == VRL_OBJECT) {
		for (size_t i = 0; i < v->u.object.len; i++) {
			vrl_object_entry *e = &v->u.object.entries[i];
			vrl_value *kv = vrl_bytes(e->key, e->key_len);
			vrl_value *params[2] = { kv, e->val };
			vrl_value *keep = NULL;
			vrl_status st = vrl_invoke_closure(a->ctx, a->closure, params, 2, &keep, err);
			vrl_value_unref(kv);
			if (st != VRL_OK) { vrl_value_unref(res); return st; }
			int t = vrl_value_truthy(keep);
			vrl_value_unref(keep);
			if (t) vrl_object_set(res, e->key, e->key_len, vrl_value_clone(e->val));
		}
	} else {
		for (size_t i = 0; i < v->u.array.len; i++) {
			vrl_value *iv = vrl_integer((int64_t)i);
			vrl_value *params[2] = { iv, v->u.array.items[i] };
			vrl_value *keep = NULL;
			vrl_status st = vrl_invoke_closure(a->ctx, a->closure, params, 2, &keep, err);
			vrl_value_unref(iv);
			if (st != VRL_OK) { vrl_value_unref(res); return st; }
			int t = vrl_value_truthy(keep);
			vrl_value_unref(keep);
			if (t) vrl_array_push(res, vrl_value_clone(v->u.array.items[i]));
		}
	}
	*out = res;
	return VRL_OK;
}

/* ================================================================== */
/* registry                                                           */
/* ================================================================== */

typedef struct { const char *name; vrl_fn fn; } builtin;

static const builtin BUILTINS[] = {
	{"is_null", fn_is_null}, {"is_string", fn_is_string}, {"is_integer", fn_is_integer},
	{"is_float", fn_is_float}, {"is_boolean", fn_is_boolean}, {"is_array", fn_is_array},
	{"is_object", fn_is_object}, {"is_timestamp", fn_is_timestamp}, {"is_regex", fn_is_regex},
	{"to_string", fn_to_string}, {"to_int", fn_to_int}, {"to_float", fn_to_float},
	{"to_bool", fn_to_bool}, {"to_timestamp", fn_to_timestamp},
	{"string", fn_to_string}, /* lenient alias */
	{"parse_json", fn_parse_json}, {"encode_json", fn_encode_json},
	{"match", fn_match}, {"parse_regex", fn_parse_regex},
	{"now", fn_now}, {"parse_timestamp", fn_parse_timestamp}, {"format_timestamp", fn_format_timestamp},
	{"upcase", fn_upcase}, {"downcase", fn_downcase}, {"strip_whitespace", fn_strip_whitespace},
	{"contains", fn_contains}, {"starts_with", fn_starts_with}, {"ends_with", fn_ends_with},
	{"split", fn_split}, {"join", fn_join}, {"replace", fn_replace}, {"slice", fn_slice},
	{"length", fn_length}, {"push", fn_push}, {"keys", fn_keys}, {"values", fn_values},
	{"merge", fn_merge},
	{"abs", fn_abs}, {"floor", fn_floor}, {"ceil", fn_ceil}, {"round", fn_round},
	{"log", fn_log},
	{"map_values", fn_map_values}, {"filter", fn_filter},
	{NULL, NULL},
};

/* simple open-addressing hash for lookup */
#define REG_SIZE 1024
static builtin REG[REG_SIZE];
static int REG_built = 0;

static uint32_t reg_hash(const char *s, size_t len)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < len; i++) {
		h ^= (unsigned char)s[i];
		h *= 16777619u;
	}
	return h;
}

void vrl_register(const char *name, vrl_fn fn)
{
	uint32_t h = reg_hash(name, strlen(name)) % REG_SIZE;
	for (int probe = 0; probe < REG_SIZE; probe++) {
		if (!REG[h].name) {
			REG[h].name = name;
			REG[h].fn = fn;
			return;
		}
		if (!strcmp(REG[h].name, name)) { /* override */
			REG[h].fn = fn;
			return;
		}
		h = (h + 1) % REG_SIZE;
	}
	/* table full: should never happen with REG_SIZE headroom */
}

void vrl_stdlib_init(void)
{
	if (REG_built)
		return;
	memset(REG, 0, sizeof(REG));
	REG_built = 1; /* set first: vrl_register() is used below */
	for (int i = 0; BUILTINS[i].name; i++)
		vrl_register(BUILTINS[i].name, BUILTINS[i].fn);
	/* module registrations (each in its own translation unit) */
	vrl_reg_type();
	vrl_reg_string();
	vrl_reg_collection();
	vrl_reg_codec();
	vrl_reg_number();
	vrl_reg_random();
	vrl_reg_path();
	vrl_reg_parse();
}

vrl_fn vrl_stdlib_lookup(const char *name, size_t len)
{
	if (!REG_built)
		vrl_stdlib_init();
	uint32_t h = reg_hash(name, len) % REG_SIZE;
	for (int probe = 0; probe < REG_SIZE; probe++) {
		if (!REG[h].name)
			return NULL;
		if (strlen(REG[h].name) == len && !memcmp(REG[h].name, name, len))
			return REG[h].fn;
		h = (h + 1) % REG_SIZE;
	}
	return NULL;
}
