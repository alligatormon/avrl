#define _GNU_SOURCE
#include "stdlib_internal.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

/* ================================================================== */
/* shared helpers                                                     */
/* ================================================================== */

static int is_intlike(const char *s, size_t n)
{
	if (n == 0) return 0;
	size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
	if (i == n) return 0;
	for (; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
	return 1;
}

static vrl_value *typed_scalar(const char *s, size_t n)
{
	if (is_intlike(s, n)) {
		char *end; long long v = strtoll(s, &end, 10);
		if ((size_t)(end - s) == n) return vrl_integer(v);
	}
	/* float? */
	if (n > 0) {
		char *end; double d = strtod(s, &end);
		if ((size_t)(end - s) == n && (memchr(s, '.', n) || memchr(s, 'e', n) || memchr(s, 'E', n)))
			return vrl_float(d);
	}
	return vrl_bytes(s, n);
}

/* Percent-decode into a fresh bytes value. */
static vrl_value *pct_decode(const char *s, size_t n)
{
	avrl_buf b; avrl_buf_init(&b);
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '+') { avrl_buf_addc(&b, ' '); continue; }
		if (s[i] == '%' && i + 2 < n) {
			int hi = -1, lo = -1;
			char c1 = s[i + 1], c2 = s[i + 2];
			if (c1 >= '0' && c1 <= '9') hi = c1 - '0'; else if ((c1|32) >= 'a' && (c1|32) <= 'f') hi = (c1|32) - 'a' + 10;
			if (c2 >= '0' && c2 <= '9') lo = c2 - '0'; else if ((c2|32) >= 'a' && (c2|32) <= 'f') lo = (c2|32) - 'a' + 10;
			if (hi >= 0 && lo >= 0) { avrl_buf_addc(&b, (char)((hi << 4) | lo)); i += 2; continue; }
		}
		avrl_buf_addc(&b, s[i]);
	}
	vrl_value *v = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return v;
}

/* Compile a regex, exec against subject, populate obj with named captures.
 * Returns 1 on match, 0 on no match, -1 on compile error. */
static int regex_named_into(const char *pat, const char *s, size_t n,
			    vrl_value *obj, avrl_log_level ll)
{
	regex_match *re = avrl_regex_compile((char *)pat);
	if (!re) return -1;
	int ov[AVRL_OVECCOUNT];
	int cnt = avrl_regex_exec(re, s, n, ov, AVRL_OVECCOUNT, ll);
	if (cnt <= 0) { avrl_regex_free(re); return 0; }
	if (re->pcre_name_count > 0 && re->pcre_name_table) {
		const unsigned char *tab = re->pcre_name_table;
		for (int i = 0; i < re->pcre_name_count; i++) {
			const unsigned char *entry = tab + i * re->pcre_name_entry_size;
			int gidx = (entry[0] << 8) | entry[1];
			const char *gname = (const char *)(entry + 2);
			if (gidx < cnt && ov[2 * gidx] >= 0)
				vrl_object_set_cstr(obj, gname, vrl_bytes(s + ov[2 * gidx], (size_t)(ov[2 * gidx + 1] - ov[2 * gidx])));
		}
	}
	avrl_regex_free(re);
	return 1;
}

/* Convert a curated set of well-known numeric field names to integers. */
static void convert_numeric_fields(vrl_value *obj, const char **names)
{
	for (int i = 0; names[i]; i++) {
		vrl_value *v = vrl_object_get(obj, names[i], strlen(names[i]));
		if (v && v->type == VRL_BYTES && is_intlike(v->u.bytes.data, v->u.bytes.len)) {
			int64_t n = strtoll(v->u.bytes.data, NULL, 10);
			vrl_object_set_cstr(obj, names[i], vrl_integer(n));
		}
	}
}

/* ================================================================== */
/* parse_int / parse_bytes / parse_duration                           */
/* ================================================================== */

static vrl_status fn_parse_int(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *bv = vrl_arg(a, "base", 1);
	int base = (bv && bv->type == VRL_INTEGER) ? (int)bv->u.integer : 10;
	char buf[128];
	if (n >= sizeof(buf)) { *err = vrl_errf("parse_int: input too long"); return VRL_ERR; }
	memcpy(buf, s, n); buf[n] = '\0';
	char *end = NULL;
	long long v = strtoll(buf, &end, base == 10 ? 0 : base); /* base 10 => auto-detect 0x/0 */
	if (end == buf) { *err = vrl_errf("parse_int: '%s' is not an integer", buf); return VRL_ERR; }
	*out = vrl_integer(v);
	return VRL_OK;
}

static double byte_unit_factor(const char *u, int binary)
{
	double k = binary ? 1024.0 : 1000.0;
	if (!strcasecmp(u, "b") || !*u) return 1;
	if (!strcasecmp(u, "kb") || !strcasecmp(u, "kib")) return k;
	if (!strcasecmp(u, "mb") || !strcasecmp(u, "mib")) return k * k;
	if (!strcasecmp(u, "gb") || !strcasecmp(u, "gib")) return k * k * k;
	if (!strcasecmp(u, "tb") || !strcasecmp(u, "tib")) return k * k * k * k;
	if (!strcasecmp(u, "pb") || !strcasecmp(u, "pib")) return k * k * k * k * k;
	return -1;
}

static vrl_status fn_parse_bytes(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *unitv = vrl_arg(a, "unit", 1);
	vrl_value *basev = vrl_arg(a, "base", 2);
	const char *outunit = (unitv && unitv->type == VRL_BYTES) ? unitv->u.bytes.data : "B";
	int binary = 1;
	if (basev && basev->type == VRL_BYTES && !strcmp(basev->u.bytes.data, "10")) binary = 0;
	char buf[128];
	if (n >= sizeof(buf)) { *err = vrl_errf("parse_bytes: input too long"); return VRL_ERR; }
	memcpy(buf, s, n); buf[n] = '\0';
	char *end = NULL;
	double num = strtod(buf, &end);
	if (end == buf) { *err = vrl_errf("parse_bytes: invalid number in '%s'", buf); return VRL_ERR; }
	while (*end == ' ') end++;
	double in_factor = byte_unit_factor(end, binary);
	if (in_factor < 0) { *err = vrl_errf("parse_bytes: unknown unit '%s'", end); return VRL_ERR; }
	double out_factor = byte_unit_factor(outunit, binary);
	if (out_factor < 0) out_factor = 1;
	*out = vrl_float(num * in_factor / out_factor);
	return VRL_OK;
}

static double dur_unit_seconds(const char *u, size_t ul)
{
	if (ul == 2 && !memcmp(u, "ns", 2)) return 1e-9;
	if (ul == 2 && (!memcmp(u, "us", 2) || !memcmp(u, "\xc2\xb5s", 2))) return 1e-6;
	if (ul == 2 && !memcmp(u, "ms", 2)) return 1e-3;
	if (ul == 1 && u[0] == 's') return 1.0;
	if (ul == 1 && u[0] == 'm') return 60.0;
	if (ul == 1 && u[0] == 'h') return 3600.0;
	if (ul == 1 && u[0] == 'd') return 86400.0;
	return -1;
}

static vrl_status fn_parse_duration(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	const char *ou; size_t oul;
	if (!avrl_arg_str(a, "unit", 1, &ou, &oul, err)) return VRL_ERR;
	double out_factor = dur_unit_seconds(ou, oul);
	if (out_factor < 0) { *err = vrl_errf("parse_duration: unknown output unit"); return VRL_ERR; }
	double total = 0.0;
	size_t i = 0;
	int any = 0;
	while (i < n) {
		while (i < n && isspace((unsigned char)s[i])) i++;
		if (i >= n) break;
		char *end = NULL;
		double num = strtod(s + i, &end);
		if (end == s + i) { *err = vrl_errf("parse_duration: invalid number"); return VRL_ERR; }
		i = (size_t)(end - s);
		size_t us = i;
		while (i < n && !isdigit((unsigned char)s[i]) && s[i] != '.' && !isspace((unsigned char)s[i])) i++;
		double f = dur_unit_seconds(s + us, i - us);
		if (f < 0) { *err = vrl_errf("parse_duration: unknown unit in '%.*s'", (int)(i - us), s + us); return VRL_ERR; }
		total += num * f;
		any = 1;
	}
	if (!any) { *err = vrl_errf("parse_duration: empty duration"); return VRL_ERR; }
	*out = vrl_float(total / out_factor);
	return VRL_OK;
}

/* ================================================================== */
/* parse_key_value / parse_logfmt / parse_query_string                */
/* ================================================================== */

static vrl_value *parse_kv_core(const char *s, size_t n, char kvd, char fd, int decode)
{
	vrl_value *obj = vrl_object_new();
	size_t i = 0;
	while (i < n) {
		while (i < n && (s[i] == fd || isspace((unsigned char)s[i]))) i++;
		if (i >= n) break;
		size_t kstart = i;
		while (i < n && s[i] != kvd && s[i] != fd) i++;
		size_t kend = i;
		if (i < n && s[i] == kvd) {
			i++; /* skip = */
			size_t vstart, vend;
			if (i < n && s[i] == '"') {
				i++; vstart = i;
				avrl_buf vb; avrl_buf_init(&vb);
				while (i < n && s[i] != '"') {
					if (s[i] == '\\' && i + 1 < n) { avrl_buf_addc(&vb, s[i + 1]); i += 2; }
					else { avrl_buf_addc(&vb, s[i]); i++; }
				}
				if (i < n) i++; /* closing quote */
				vend = 0; (void)vstart; (void)vend;
				vrl_value *key = decode ? pct_decode(s + kstart, kend - kstart) : vrl_bytes(s + kstart, kend - kstart);
				vrl_object_set(obj, key->u.bytes.data, key->u.bytes.len, avrl_buf_to_bytes(&vb));
				vrl_value_unref(key);
				avrl_buf_free(&vb);
			} else {
				vstart = i;
				while (i < n && s[i] != fd) i++;
				vend = i;
				vrl_value *key = decode ? pct_decode(s + kstart, kend - kstart) : vrl_bytes(s + kstart, kend - kstart);
				vrl_value *val = decode ? pct_decode(s + vstart, vend - vstart) : vrl_bytes(s + vstart, vend - vstart);
				/* query string: repeated keys -> array */
				if (decode) {
					vrl_value *ex = vrl_object_get(obj, key->u.bytes.data, key->u.bytes.len);
					if (ex && ex->type == VRL_ARRAY) { vrl_array_push(ex, val); }
					else if (ex) {
						vrl_value *arr = vrl_array_new();
						vrl_array_push(arr, vrl_value_clone(ex));
						vrl_array_push(arr, val);
						vrl_object_set(obj, key->u.bytes.data, key->u.bytes.len, arr);
					} else {
						vrl_object_set(obj, key->u.bytes.data, key->u.bytes.len, val);
					}
				} else {
					vrl_object_set(obj, key->u.bytes.data, key->u.bytes.len, val);
				}
				vrl_value_unref(key);
			}
		} else {
			/* bare key without value */
			vrl_value *key = decode ? pct_decode(s + kstart, kend - kstart) : vrl_bytes(s + kstart, kend - kstart);
			vrl_object_set(obj, key->u.bytes.data, key->u.bytes.len, vrl_bytes_cstr(""));
			vrl_value_unref(key);
		}
	}
	return obj;
}

static vrl_status fn_parse_key_value(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *kvd = vrl_arg(a, "key_value_delimiter", 1);
	vrl_value *fd = vrl_arg(a, "field_delimiter", 2);
	char kv = (kvd && kvd->type == VRL_BYTES && kvd->u.bytes.len) ? kvd->u.bytes.data[0] : '=';
	char f = (fd && fd->type == VRL_BYTES && fd->u.bytes.len) ? fd->u.bytes.data[0] : ' ';
	*out = parse_kv_core(s, n, kv, f, 0);
	return VRL_OK;
}

static vrl_status fn_parse_logfmt(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	*out = parse_kv_core(s, n, '=', ' ', 0);
	return VRL_OK;
}

static vrl_status fn_parse_query_string(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	if (n && s[0] == '?') { s++; n--; }
	*out = parse_kv_core(s, n, '=', '&', 1);
	return VRL_OK;
}

/* ================================================================== */
/* parse_csv / parse_tokens                                           */
/* ================================================================== */

static vrl_status fn_parse_csv(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *delv = vrl_arg(a, "delimiter", 1);
	char delim = (delv && delv->type == VRL_BYTES && delv->u.bytes.len == 1) ? delv->u.bytes.data[0] : ',';
	vrl_value *arr = vrl_array_new();
	size_t i = 0;
	avrl_buf field; avrl_buf_init(&field);
	int in_field = 1;
	while (i <= n) {
		if (i < n && s[i] == '"') {
			i++;
			while (i < n) {
				if (s[i] == '"') {
					if (i + 1 < n && s[i + 1] == '"') { avrl_buf_addc(&field, '"'); i += 2; }
					else { i++; break; }
				} else { avrl_buf_addc(&field, s[i]); i++; }
			}
		} else if (i == n || s[i] == delim) {
			vrl_array_push(arr, avrl_buf_to_bytes(&field));
			avrl_buf_free(&field); avrl_buf_init(&field);
			if (i == n) break;
			i++;
			in_field = 1;
		} else {
			avrl_buf_addc(&field, s[i]); i++;
		}
		(void)in_field;
	}
	avrl_buf_free(&field);
	*out = arr;
	return VRL_OK;
}

static vrl_status fn_parse_tokens(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *arr = vrl_array_new();
	size_t i = 0;
	while (i < n) {
		while (i < n && isspace((unsigned char)s[i])) i++;
		if (i >= n) break;
		if (s[i] == '"') {
			i++; size_t start = i;
			while (i < n && s[i] != '"') i++;
			vrl_array_push(arr, vrl_bytes(s + start, i - start));
			if (i < n) i++;
		} else if (s[i] == '[') {
			i++; size_t start = i;
			while (i < n && s[i] != ']') i++;
			vrl_array_push(arr, vrl_bytes(s + start, i - start));
			if (i < n) i++;
		} else {
			size_t start = i;
			while (i < n && !isspace((unsigned char)s[i])) i++;
			vrl_array_push(arr, vrl_bytes(s + start, i - start));
		}
	}
	*out = arr;
	return VRL_OK;
}

/* ================================================================== */
/* parse_regex_all                                                    */
/* ================================================================== */

static vrl_status fn_parse_regex_all(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *pat = vrl_arg(a, "pattern", 1);
	regex_match *re = NULL; int owned = 0;
	if (pat && pat->type == VRL_REGEX) re = pat->u.regex;
	else if (pat && pat->type == VRL_BYTES) { re = avrl_regex_compile(pat->u.bytes.data); owned = 1; }
	if (!re) { *err = vrl_errf("parse_regex_all: expected regex/pattern"); return VRL_ERR; }
	vrl_value *arr = vrl_array_new();
	int ov[AVRL_OVECCOUNT];
	size_t pos = 0;
	while (pos <= n) {
		int cnt = pcre_exec(re->regex_compiled, re->pcreExtra, s, (int)n, (int)pos, 0, ov, AVRL_OVECCOUNT);
		if (cnt <= 0) break;
		vrl_value *obj = vrl_object_new();
		for (int i = 0; i < cnt; i++) {
			char key[16]; snprintf(key, sizeof(key), "%d", i);
			if (ov[2 * i] < 0) vrl_object_set_cstr(obj, key, vrl_null());
			else vrl_object_set_cstr(obj, key, vrl_bytes(s + ov[2 * i], (size_t)(ov[2 * i + 1] - ov[2 * i])));
		}
		if (re->pcre_name_count > 0 && re->pcre_name_table) {
			const unsigned char *tab = re->pcre_name_table;
			for (int i = 0; i < re->pcre_name_count; i++) {
				const unsigned char *entry = tab + i * re->pcre_name_entry_size;
				int gidx = (entry[0] << 8) | entry[1];
				const char *gname = (const char *)(entry + 2);
				if (gidx < cnt && ov[2 * gidx] >= 0)
					vrl_object_set_cstr(obj, gname, vrl_bytes(s + ov[2 * gidx], (size_t)(ov[2 * gidx + 1] - ov[2 * gidx])));
			}
		}
		vrl_array_push(arr, obj);
		size_t np = (size_t)ov[1];
		if (np == pos) np++;
		pos = np;
	}
	if (owned) avrl_regex_free(re);
	*out = arr;
	return VRL_OK;
}

/* ================================================================== */
/* apache / nginx / common log                                        */
/* ================================================================== */

static const char *NUMERIC_LOG_FIELDS[] = {
	"status", "size", "body_bytes_size", "bytes", "content_length",
	"request_length", "port", "srcport", "dstport", "packets", NULL
};

static vrl_status fn_parse_common_log(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	const char *pat =
		"^(?<host>\\S+)\\s+(?<identity>\\S+)\\s+(?<user>\\S+)\\s+"
		"\\[(?<timestamp>[^\\]]+)\\]\\s+"
		"\"(?<method>\\S+)\\s+(?<path>\\S+)\\s+(?<protocol>[^\"]+)\"\\s+"
		"(?<status>\\d+)\\s+(?<size>\\d+|-)";
	vrl_value *obj = vrl_object_new();
	int r = regex_named_into(pat, s, n, obj, a->ctx->ll);
	if (r <= 0) { vrl_value_unref(obj); *err = vrl_errf("parse_common_log: no match"); return VRL_ERR; }
	convert_numeric_fields(obj, NUMERIC_LOG_FIELDS);
	*out = obj;
	return VRL_OK;
}

static vrl_status fn_parse_apache_log(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *fmtv = vrl_arg(a, "format", 1);
	const char *fmt = (fmtv && fmtv->type == VRL_BYTES) ? fmtv->u.bytes.data : "common";
	const char *pat;
	if (!strcmp(fmt, "combined")) {
		pat = "^(?<host>\\S+)\\s+(?<identity>\\S+)\\s+(?<user>\\S+)\\s+"
		      "\\[(?<timestamp>[^\\]]+)\\]\\s+"
		      "\"(?<method>\\S+)\\s+(?<path>\\S+)\\s+(?<protocol>[^\"]+)\"\\s+"
		      "(?<status>\\d+)\\s+(?<size>\\d+|-)\\s+"
		      "\"(?<referrer>[^\"]*)\"\\s+\"(?<agent>[^\"]*)\"";
	} else if (!strcmp(fmt, "error")) {
		pat = "^\\[(?<timestamp>[^\\]]+)\\]\\s+\\[(?<module>[^\\]]*)\\]?\\s*\\[?(?<severity>[^\\]]*)\\]?\\s*"
		      "(?:\\[client (?<client>[^\\]]+)\\]\\s+)?(?<message>.*)$";
	} else {
		pat = "^(?<host>\\S+)\\s+(?<identity>\\S+)\\s+(?<user>\\S+)\\s+"
		      "\\[(?<timestamp>[^\\]]+)\\]\\s+"
		      "\"(?<method>\\S+)\\s+(?<path>\\S+)\\s+(?<protocol>[^\"]+)\"\\s+"
		      "(?<status>\\d+)\\s+(?<size>\\d+|-)";
	}
	vrl_value *obj = vrl_object_new();
	int r = regex_named_into(pat, s, n, obj, a->ctx->ll);
	if (r <= 0) { vrl_value_unref(obj); *err = vrl_errf("parse_apache_log: no match"); return VRL_ERR; }
	convert_numeric_fields(obj, NUMERIC_LOG_FIELDS);
	*out = obj;
	return VRL_OK;
}

static vrl_status fn_parse_nginx_log(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *fmtv = vrl_arg(a, "format", 1);
	const char *fmt = (fmtv && fmtv->type == VRL_BYTES) ? fmtv->u.bytes.data : "combined";
	const char *pat;
	if (!strcmp(fmt, "error")) {
		pat = "^(?<timestamp>\\d{4}/\\d{2}/\\d{2} \\d{2}:\\d{2}:\\d{2})\\s+"
		      "\\[(?<severity>\\w+)\\]\\s+(?<pid>\\d+)#(?<tid>\\d+):\\s+(?<message>.*)$";
	} else {
		pat = "^(?<client>\\S+)\\s+(?<identity>\\S+)\\s+(?<user>\\S+)\\s+"
		      "\\[(?<timestamp>[^\\]]+)\\]\\s+"
		      "\"(?<method>\\S+)\\s+(?<path>\\S+)\\s+(?<protocol>[^\"]+)\"\\s+"
		      "(?<status>\\d+)\\s+(?<size>\\d+)\\s+"
		      "\"(?<referer>[^\"]*)\"\\s+\"(?<agent>[^\"]*)\"";
	}
	vrl_value *obj = vrl_object_new();
	int r = regex_named_into(pat, s, n, obj, a->ctx->ll);
	if (r <= 0) { vrl_value_unref(obj); *err = vrl_errf("parse_nginx_log: no match"); return VRL_ERR; }
	convert_numeric_fields(obj, NUMERIC_LOG_FIELDS);
	*out = obj;
	return VRL_OK;
}

/* ================================================================== */
/* glog / klog                                                        */
/* ================================================================== */

static vrl_status fn_parse_glog(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	const char *pat =
		"^(?<level>[IWEF])(?<timestamp>\\d{8}\\s+\\d{2}:\\d{2}:\\d{2}\\.\\d+)\\s+"
		"(?<id>\\d+)\\s+(?<file>[^:]+):(?<line>\\d+)\\]\\s+(?<message>.*)$";
	vrl_value *obj = vrl_object_new();
	int r = regex_named_into(pat, s, n, obj, a->ctx->ll);
	if (r <= 0) { vrl_value_unref(obj); *err = vrl_errf("parse_glog: no match"); return VRL_ERR; }
	const char *nums[] = {"id", "line", NULL};
	convert_numeric_fields(obj, nums);
	*out = obj;
	return VRL_OK;
}

static vrl_status fn_parse_klog(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	const char *pat =
		"^(?<level>[IWEF])(?<timestamp>\\d{4}\\s+\\d{2}:\\d{2}:\\d{2}\\.\\d+)\\s+"
		"(?<id>\\d+)\\s+(?<file>[^:]+):(?<line>\\d+)\\]\\s+(?<message>.*)$";
	vrl_value *obj = vrl_object_new();
	int r = regex_named_into(pat, s, n, obj, a->ctx->ll);
	if (r <= 0) { vrl_value_unref(obj); *err = vrl_errf("parse_klog: no match"); return VRL_ERR; }
	const char *nums[] = {"id", "line", NULL};
	convert_numeric_fields(obj, nums);
	*out = obj;
	return VRL_OK;
}

/* ================================================================== */
/* syslog (RFC3164 + RFC5424) + linux_authorization                   */
/* ================================================================== */

static const char *SYSLOG_SEVERITY[] = {
	"emergency", "alert", "critical", "error", "warning", "notice", "informational", "debug"
};
static const char *SYSLOG_FACILITY[] = {
	"kern", "user", "mail", "daemon", "auth", "syslog", "lpr", "news",
	"uucp", "cron", "authpriv", "ftp", "ntp", "security", "console",
	"solaris-cron", "local0", "local1", "local2", "local3", "local4",
	"local5", "local6", "local7"
};

static vrl_value *syslog_parse(const char *s, size_t n, avrl_log_level ll)
{
	vrl_value *obj = vrl_object_new();
	size_t i = 0;
	int pri = -1;
	if (i < n && s[i] == '<') {
		i++; int p = 0, any = 0;
		while (i < n && isdigit((unsigned char)s[i])) { p = p * 10 + (s[i] - '0'); i++; any = 1; }
		if (i < n && s[i] == '>' && any) { pri = p; i++; }
		else i = 0;
	}
	if (pri >= 0) {
		int fac = pri / 8, sev = pri % 8;
		if (fac < (int)(sizeof(SYSLOG_FACILITY)/sizeof(char*)))
			vrl_object_set_cstr(obj, "facility", vrl_bytes_cstr(SYSLOG_FACILITY[fac]));
		vrl_object_set_cstr(obj, "severity", vrl_bytes_cstr(SYSLOG_SEVERITY[sev]));
	}
	/* RFC5424: version digit + space right after PRI */
	if (i < n && isdigit((unsigned char)s[i]) && i + 1 < n && s[i + 1] == ' ') {
		vrl_object_set_cstr(obj, "version", vrl_integer(s[i] - '0'));
		const char *rest = s + i + 2;
		size_t rn = n - i - 2;
		const char *pat =
			"^(?<timestamp>\\S+)\\s+(?<hostname>\\S+)\\s+(?<appname>\\S+)\\s+"
			"(?<procid>\\S+)\\s+(?<msgid>\\S+)\\s+(?:-|\\[.*?\\])\\s*(?<message>.*)$";
		regex_named_into(pat, rest, rn, obj, ll);
		return obj;
	}
	/* RFC3164: "Mmm dd hh:mm:ss host tag[pid]: msg" */
	const char *rest = s + i;
	size_t rn = n - i;
	const char *pat =
		"^(?<timestamp>[A-Z][a-z]{2}\\s+\\d{1,2}\\s+\\d{2}:\\d{2}:\\d{2})\\s+"
		"(?<hostname>\\S+)\\s+(?<appname>[^\\[:\\s]+)(?:\\[(?<procid>\\d+)\\])?:?\\s*(?<message>.*)$";
	if (regex_named_into(pat, rest, rn, obj, ll) <= 0)
		vrl_object_set_cstr(obj, "message", vrl_bytes(rest, rn));
	return obj;
}

static vrl_status fn_parse_syslog(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	*out = syslog_parse(s, n, a->ctx->ll);
	return VRL_OK;
}

static vrl_status fn_parse_linux_authorization(vrl_call_args *a, vrl_value **out, char **err)
{
	return fn_parse_syslog(a, out, err);
}

/* ================================================================== */
/* CEF                                                                */
/* ================================================================== */

static vrl_status fn_parse_cef(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	/* CEF:Version|Vendor|Product|Version|SigID|Name|Severity|Extension */
	if (n < 4 || memcmp(s, "CEF:", 4) != 0) { *err = vrl_errf("parse_cef: missing CEF: prefix"); return VRL_ERR; }
	static const char *fields[] = {
		"cefVersion", "deviceVendor", "deviceProduct", "deviceVersion",
		"deviceEventClassId", "name", "severity"
	};
	vrl_value *obj = vrl_object_new();
	size_t i = 4, fi = 0;
	size_t start = i;
	for (; i <= n && fi < 7; i++) {
		if (i == n || s[i] == '|') {
			vrl_object_set_cstr(obj, fields[fi], vrl_bytes(s + start, i - start));
			fi++; start = i + 1;
			if (fi == 7) break;
		}
	}
	/* extension: key=value pairs (space separated) */
	if (start < n) {
		vrl_value *ext = parse_kv_core(s + start, n - start, '=', ' ', 0);
		for (size_t k = 0; k < ext->u.object.len; k++) {
			vrl_object_entry *e = &ext->u.object.entries[k];
			vrl_object_set(obj, e->key, e->key_len, vrl_value_clone(e->val));
		}
		vrl_value_unref(ext);
	}
	*out = obj;
	return VRL_OK;
}

/* ================================================================== */
/* AWS logs                                                           */
/* ================================================================== */

/* Split a space-separated log line honoring "quoted" fields. */
static vrl_value *split_fields_quoted(const char *s, size_t n)
{
	vrl_value *arr = vrl_array_new();
	size_t i = 0;
	while (i < n) {
		while (i < n && s[i] == ' ') i++;
		if (i >= n) break;
		if (s[i] == '"') {
			i++; size_t start = i;
			while (i < n && s[i] != '"') i++;
			vrl_array_push(arr, vrl_bytes(s + start, i - start));
			if (i < n) i++;
		} else {
			size_t start = i;
			while (i < n && s[i] != ' ') i++;
			vrl_array_push(arr, vrl_bytes(s + start, i - start));
		}
	}
	return arr;
}

static vrl_status fn_parse_aws_vpc_flow_log(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	static const char *v2[] = {
		"version", "account_id", "interface_id", "srcaddr", "dstaddr",
		"srcport", "dstport", "protocol", "packets", "bytes",
		"start", "end", "action", "log_status", NULL
	};
	vrl_value *fields = split_fields_quoted(s, n);
	vrl_value *obj = vrl_object_new();
	for (size_t i = 0; v2[i] && i < fields->u.array.len; i++) {
		vrl_value *f = fields->u.array.items[i];
		if (f->type == VRL_BYTES && !strcmp(f->u.bytes.data, "-"))
			vrl_object_set_cstr(obj, v2[i], vrl_null());
		else
			vrl_object_set_cstr(obj, v2[i], vrl_value_clone(f));
	}
	vrl_value_unref(fields);
	const char *nums[] = {"version","srcport","dstport","protocol","packets","bytes","start","end",NULL};
	convert_numeric_fields(obj, nums);
	*out = obj;
	return VRL_OK;
}

static vrl_status fn_parse_aws_alb_log(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	static const char *cols[] = {
		"type", "timestamp", "elb", "client_host", "target_host",
		"request_processing_time", "target_processing_time", "response_processing_time",
		"elb_status_code", "target_status_code", "received_bytes", "sent_bytes",
		"request", "user_agent", "ssl_cipher", "ssl_protocol", "target_group_arn",
		"trace_id", "domain_name", "chosen_cert_arn", "matched_rule_priority",
		"request_creation_time", "actions_executed", "redirect_url", "error_reason",
		"target_list", "target_status_code_list", "classification", "classification_reason", NULL
	};
	vrl_value *fields = split_fields_quoted(s, n);
	vrl_value *obj = vrl_object_new();
	for (size_t i = 0; cols[i] && i < fields->u.array.len; i++) {
		vrl_value *f = fields->u.array.items[i];
		if (f->type == VRL_BYTES && !strcmp(f->u.bytes.data, "-"))
			vrl_object_set_cstr(obj, cols[i], vrl_null());
		else
			vrl_object_set_cstr(obj, cols[i], vrl_value_clone(f));
	}
	vrl_value_unref(fields);
	const char *nums[] = {"elb_status_code","target_status_code","received_bytes","sent_bytes","matched_rule_priority",NULL};
	convert_numeric_fields(obj, nums);
	*out = obj;
	return VRL_OK;
}

static vrl_status fn_parse_aws_cloudwatch_log_subscription_message(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	char *jerr = NULL;
	vrl_value *v = vrl_json_decode(s, n, &jerr);
	if (!v) { *err = vrl_errf("parse_aws_cloudwatch_log_subscription_message: %s", jerr ? jerr : "invalid json"); free(jerr); return VRL_ERR; }
	if (v->type != VRL_OBJECT) { vrl_value_unref(v); *err = vrl_errf("parse_aws_cloudwatch_log_subscription_message: expected JSON object"); return VRL_ERR; }
	*out = v;
	return VRL_OK;
}

/* ================================================================== */
/* parse_ruby_hash                                                    */
/* ================================================================== */

typedef struct { const char *s; size_t n; size_t i; } rbp;

static void rb_ws(rbp *p) { while (p->i < p->n && isspace((unsigned char)p->s[p->i])) p->i++; }

static vrl_value *rb_value(rbp *p);

static vrl_value *rb_string(rbp *p, char q)
{
	p->i++; /* opening quote */
	avrl_buf b; avrl_buf_init(&b);
	while (p->i < p->n && p->s[p->i] != q) {
		if (p->s[p->i] == '\\' && p->i + 1 < p->n) { avrl_buf_addc(&b, p->s[p->i + 1]); p->i += 2; }
		else { avrl_buf_addc(&b, p->s[p->i]); p->i++; }
	}
	if (p->i < p->n) p->i++;
	vrl_value *v = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return v;
}

static vrl_value *rb_hash(rbp *p)
{
	p->i++; /* { */
	vrl_value *obj = vrl_object_new();
	rb_ws(p);
	while (p->i < p->n && p->s[p->i] != '}') {
		rb_ws(p);
		vrl_value *key = rb_value(p);
		rb_ws(p);
		/* expect => or : */
		if (p->i + 1 < p->n && p->s[p->i] == '=' && p->s[p->i + 1] == '>') p->i += 2;
		else if (p->i < p->n && p->s[p->i] == ':') p->i++;
		rb_ws(p);
		vrl_value *val = rb_value(p);
		size_t kl; char *ks = vrl_value_to_string(key, &kl);
		vrl_object_set(obj, ks, kl, val);
		free(ks); vrl_value_unref(key);
		rb_ws(p);
		if (p->i < p->n && p->s[p->i] == ',') p->i++;
		rb_ws(p);
	}
	if (p->i < p->n) p->i++; /* } */
	return obj;
}

static vrl_value *rb_array(rbp *p)
{
	p->i++; /* [ */
	vrl_value *arr = vrl_array_new();
	rb_ws(p);
	while (p->i < p->n && p->s[p->i] != ']') {
		vrl_array_push(arr, rb_value(p));
		rb_ws(p);
		if (p->i < p->n && p->s[p->i] == ',') p->i++;
		rb_ws(p);
	}
	if (p->i < p->n) p->i++;
	return arr;
}

static vrl_value *rb_value(rbp *p)
{
	rb_ws(p);
	if (p->i >= p->n) return vrl_null();
	char c = p->s[p->i];
	if (c == '{') return rb_hash(p);
	if (c == '[') return rb_array(p);
	if (c == '"' || c == '\'') return rb_string(p, c);
	if (c == ':') { p->i++; size_t start = p->i; while (p->i < p->n && (isalnum((unsigned char)p->s[p->i]) || p->s[p->i] == '_')) p->i++; return vrl_bytes(p->s + start, p->i - start); }
	/* bareword / number / nil / true / false */
	size_t start = p->i;
	while (p->i < p->n && p->s[p->i] != ',' && p->s[p->i] != '}' && p->s[p->i] != ']' &&
	       p->s[p->i] != '=' && !isspace((unsigned char)p->s[p->i])) p->i++;
	size_t len = p->i - start;
	if (len == 3 && !memcmp(p->s + start, "nil", 3)) return vrl_null();
	if (len == 4 && !memcmp(p->s + start, "true", 4)) return vrl_boolean(1);
	if (len == 5 && !memcmp(p->s + start, "false", 5)) return vrl_boolean(0);
	return typed_scalar(p->s + start, len);
}

static vrl_status fn_parse_ruby_hash(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	rbp p = { s, n, 0 };
	rb_ws(&p);
	if (p.i >= n || s[p.i] != '{') { *err = vrl_errf("parse_ruby_hash: expected '{'"); return VRL_ERR; }
	*out = rb_hash(&p);
	return VRL_OK;
}

/* ================================================================== */
/* parse_influxdb                                                     */
/* ================================================================== */

static vrl_status fn_parse_influxdb(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *arr = vrl_array_new();
	size_t ls = 0;
	while (ls <= n) {
		size_t le = ls;
		while (le < n && s[le] != '\n') le++;
		if (le > ls) {
			/* one line: measurement[,tags] fields [timestamp] */
			const char *line = s + ls; size_t ll = le - ls;
			/* split into up to 3 space-separated groups (spaces may be escaped) */
			size_t i = 0;
			/* measurement + tags */
			size_t g1s = 0; while (i < ll && !(line[i] == ' ' && (i == 0 || line[i-1] != '\\'))) i++;
			size_t g1e = i; while (i < ll && line[i] == ' ') i++;
			size_t g2s = i; while (i < ll && !(line[i] == ' ' && line[i-1] != '\\')) i++;
			size_t g2e = i; while (i < ll && line[i] == ' ') i++;
			size_t g3s = i; size_t g3e = ll;

			vrl_value *point = vrl_object_new();
			/* measurement + tags in g1 */
			const char *mt = line + g1s; size_t mtl = g1e - g1s;
			size_t comma = 0; int hascomma = 0;
			for (size_t k = 0; k < mtl; k++) if (mt[k] == ',' && (k == 0 || mt[k-1] != '\\')) { comma = k; hascomma = 1; break; }
			size_t measl = hascomma ? comma : mtl;
			vrl_object_set_cstr(point, "measurement", vrl_bytes(mt, measl));
			vrl_value *tags = vrl_object_new();
			if (hascomma) {
				vrl_value *t = parse_kv_core(mt + comma + 1, mtl - comma - 1, '=', ',', 0);
				vrl_value_unref(tags); tags = t;
			}
			vrl_object_set_cstr(point, "tags", tags);
			/* fields */
			vrl_value *fields = parse_kv_core(line + g2s, g2e - g2s, '=', ',', 0);
			/* coerce field values */
			for (size_t k = 0; k < fields->u.object.len; k++) {
				vrl_object_entry *e = &fields->u.object.entries[k];
				if (e->val->type == VRL_BYTES) {
					const char *fv = e->val->u.bytes.data; size_t fvl = e->val->u.bytes.len;
					if (fvl >= 1 && (fv[fvl-1] == 'i')) {
						vrl_object_set(fields, e->key, e->key_len, typed_scalar(fv, fvl - 1));
					} else {
						vrl_object_set(fields, e->key, e->key_len, typed_scalar(fv, fvl));
					}
				}
			}
			vrl_object_set_cstr(point, "fields", fields);
			if (g3e > g3s)
				vrl_object_set_cstr(point, "timestamp", typed_scalar(line + g3s, g3e - g3s));
			vrl_array_push(arr, point);
		}
		if (le >= n) break;
		ls = le + 1;
	}
	*out = arr;
	return VRL_OK;
}

/* ================================================================== */
/* parse_url                                                          */
/* ================================================================== */

static vrl_status fn_parse_url(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *obj = vrl_object_new();
	size_t i = 0;
	/* scheme */
	size_t sch_end = 0; int have_scheme = 0;
	for (size_t k = 0; k + 2 < n; k++) {
		if (s[k] == ':' && s[k+1] == '/' && s[k+2] == '/') { sch_end = k; have_scheme = 1; break; }
	}
	const char *scheme = ""; size_t schl = 0;
	if (have_scheme) { scheme = s; schl = sch_end; i = sch_end + 3; }
	vrl_object_set_cstr(obj, "scheme", vrl_bytes(scheme, schl));

	/* authority up to /, ?, # */
	size_t auth_start = i;
	while (i < n && s[i] != '/' && s[i] != '?' && s[i] != '#') i++;
	size_t auth_end = i;
	/* userinfo@host:port */
	const char *auth = s + auth_start; size_t authl = auth_end - auth_start;
	size_t at = authl; int have_at = 0;
	for (size_t k = 0; k < authl; k++) if (auth[k] == '@') { at = k; have_at = 1; }
	const char *user = "", *pass = ""; size_t userl = 0, passl = 0;
	size_t hoststart = 0;
	if (have_at) {
		size_t colon = at; int hc = 0;
		for (size_t k = 0; k < at; k++) if (auth[k] == ':') { colon = k; hc = 1; break; }
		user = auth; userl = hc ? colon : at;
		if (hc) { pass = auth + colon + 1; passl = at - colon - 1; }
		hoststart = at + 1;
	}
	const char *hostport = auth + hoststart; size_t hpl = authl - hoststart;
	const char *host = hostport; size_t hostl = hpl;
	int port = -1;
	for (size_t k = 0; k < hpl; k++) {
		if (hostport[k] == ':') {
			hostl = k;
			port = atoi(hostport + k + 1);
			break;
		}
	}
	vrl_object_set_cstr(obj, "username", vrl_bytes(user, userl));
	vrl_object_set_cstr(obj, "password", vrl_bytes(pass, passl));
	vrl_object_set_cstr(obj, "host", vrl_bytes(host, hostl));
	vrl_object_set_cstr(obj, "port", port >= 0 ? vrl_integer(port) : vrl_null());

	/* path */
	size_t path_start = i;
	while (i < n && s[i] != '?' && s[i] != '#') i++;
	vrl_object_set_cstr(obj, "path", vrl_bytes(s + path_start, i - path_start));
	/* query */
	vrl_value *query = vrl_object_new();
	if (i < n && s[i] == '?') {
		i++; size_t q_start = i;
		while (i < n && s[i] != '#') i++;
		vrl_value_unref(query);
		query = parse_kv_core(s + q_start, i - q_start, '=', '&', 1);
	}
	vrl_object_set_cstr(obj, "query", query);
	/* fragment */
	if (i < n && s[i] == '#') { i++; vrl_object_set_cstr(obj, "fragment", vrl_bytes(s + i, n - i)); }
	else vrl_object_set_cstr(obj, "fragment", vrl_null());
	*out = obj;
	return VRL_OK;
}

/* ================================================================== */
/* grok                                                               */
/* ================================================================== */

typedef struct { const char *name; const char *pat; } grok_pat;

static const grok_pat GROK[] = {
	{"USERNAME", "[a-zA-Z0-9._-]+"},
	{"USER", "%{USERNAME}"},
	{"INT", "(?:[+-]?(?:[0-9]+))"},
	{"BASE10NUM", "(?:[+-]?(?:[0-9]+(?:\\.[0-9]+)?)|\\.[0-9]+)"},
	{"NUMBER", "%{BASE10NUM}"},
	{"POSINT", "\\b(?:[1-9][0-9]*)\\b"},
	{"NONNEGINT", "\\b(?:[0-9]+)\\b"},
	{"WORD", "\\b\\w+\\b"},
	{"NOTSPACE", "\\S+"},
	{"SPACE", "\\s*"},
	{"DATA", ".*?"},
	{"GREEDYDATA", ".*"},
	{"QUOTEDSTRING", "(?:\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*')"},
	{"UUID", "[A-Fa-f0-9]{8}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{12}"},
	{"IPV4", "(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)"},
	{"IPV6", "(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}"},
	{"IP", "(?:%{IPV6}|%{IPV4})"},
	{"HOSTNAME", "\\b(?:[0-9A-Za-z][0-9A-Za-z-]{0,62})(?:\\.(?:[0-9A-Za-z][0-9A-Za-z-]{0,62}))*\\b"},
	{"IPORHOST", "(?:%{IP}|%{HOSTNAME})"},
	{"MONTH", "\\b(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[a-z]*\\b"},
	{"MONTHNUM", "(?:0?[1-9]|1[0-2])"},
	{"MONTHDAY", "(?:(?:0[1-9])|(?:[12][0-9])|(?:3[01])|[1-9])"},
	{"YEAR", "(?:\\d\\d){1,2}"},
	{"HOUR", "(?:2[0123]|[01]?[0-9])"},
	{"MINUTE", "(?:[0-5][0-9])"},
	{"SECOND", "(?:(?:[0-5]?[0-9]|60)(?:[:.,][0-9]+)?)"},
	{"TIME", "%{HOUR}:%{MINUTE}:%{SECOND}"},
	{"TIMESTAMP_ISO8601", "%{YEAR}-%{MONTHNUM}-%{MONTHDAY}[T ]%{HOUR}:%{MINUTE}:%{SECOND}(?:Z|[+-]%{HOUR}:?%{MINUTE})?"},
	{"HTTPDATE", "%{MONTHDAY}/%{MONTH}/%{YEAR}:%{TIME} %{INT}"},
	{"LOGLEVEL", "(?:[Aa]lert|ALERT|[Tt]race|TRACE|[Dd]ebug|DEBUG|[Nn]otice|NOTICE|[Ii]nfo|INFO|[Ww]arn(?:ing)?|WARN(?:ING)?|[Ee]rr(?:or)?|ERR(?:OR)?|[Cc]rit(?:ical)?|CRIT(?:ICAL)?|[Ff]atal|FATAL|[Ee]merg(?:ency)?|EMERG(?:ENCY)?)"},
	{"URIPATH", "(?:/[A-Za-z0-9$.+!*'(){},~:;=@#%&_\\-]*)+"},
	{"URIPARAM", "\\?[A-Za-z0-9$.+!*'|(){},~@#%&/=:;_?\\-\\[\\]<>]*"},
	{NULL, NULL},
};

static const char *grok_find(const char *name, size_t len)
{
	for (int i = 0; GROK[i].name; i++)
		if (strlen(GROK[i].name) == len && !memcmp(GROK[i].name, name, len))
			return GROK[i].pat;
	return NULL;
}

/* Expand one pass of %{...}. Returns 1 if any substitution happened. */
static int grok_expand_once(const char *in, size_t inlen, avrl_buf *out, int *ok)
{
	int changed = 0;
	size_t i = 0;
	while (i < inlen) {
		if (i + 1 < inlen && in[i] == '%' && in[i + 1] == '{') {
			size_t j = i + 2;
			while (j < inlen && in[j] != '}') j++;
			if (j >= inlen) { avrl_buf_addc(out, in[i]); i++; continue; }
			/* content between { and } : NAME or NAME:field */
			const char *content = in + i + 2;
			size_t clen = j - (i + 2);
			size_t colon = clen; int has_field = 0;
			for (size_t k = 0; k < clen; k++) if (content[k] == ':') { colon = k; has_field = 1; break; }
			const char *pname = content; size_t pnl = has_field ? colon : clen;
			const char *pat = grok_find(pname, pnl);
			if (!pat) { *ok = 0; avrl_buf_add(out, in + i, j - i + 1); i = j + 1; changed = 1; continue; }
			if (has_field) {
				const char *field = content + colon + 1; size_t fl = clen - colon - 1;
				avrl_buf_puts(out, "(?<");
				avrl_buf_add(out, field, fl);
				avrl_buf_addc(out, '>');
				avrl_buf_puts(out, pat);
				avrl_buf_addc(out, ')');
			} else {
				avrl_buf_puts(out, "(?:");
				avrl_buf_puts(out, pat);
				avrl_buf_addc(out, ')');
			}
			i = j + 1;
			changed = 1;
		} else {
			avrl_buf_addc(out, in[i]);
			i++;
		}
	}
	return changed;
}

static char *grok_compile(const char *pattern, size_t plen)
{
	avrl_buf cur; avrl_buf_init(&cur);
	avrl_buf_add(&cur, pattern, plen);
	int ok = 1;
	for (int iter = 0; iter < 20; iter++) {
		avrl_buf next; avrl_buf_init(&next);
		char *cs = cur.s ? cur.s : (char *)"";
		size_t cl = cur.len;
		int changed = grok_expand_once(cs, cl, &next, &ok);
		if (!changed) { avrl_buf_free(&next); break; }
		avrl_buf_free(&cur);
		cur = next;
	}
	cur.s = realloc(cur.s, cur.len + 1);
	if (!cur.s) cur.s = calloc(1, 1);
	else cur.s[cur.len] = '\0';
	return cur.s; /* caller frees */
}

static vrl_status grok_apply(const char *pat, size_t patlen, const char *s, size_t n,
			     avrl_log_level ll, vrl_value **out, char **err)
{
	char *rx = grok_compile(pat, patlen);
	vrl_value *obj = vrl_object_new();
	int r = regex_named_into(rx, s, n, obj, ll);
	free(rx);
	if (r < 0) { vrl_value_unref(obj); if (err) *err = vrl_errf("parse_grok: invalid pattern"); return VRL_ERR; }
	if (r == 0) { vrl_value_unref(obj); if (err) *err = vrl_errf("parse_grok: no match"); return VRL_ERR; }
	*out = obj;
	return VRL_OK;
}

static vrl_status fn_parse_grok(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	const char *pat; size_t pl;
	if (!avrl_arg_str(a, "pattern", 1, &pat, &pl, err)) return VRL_ERR;
	return grok_apply(pat, pl, s, n, a->ctx->ll, out, err);
}

static vrl_status fn_parse_groks(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *pats = vrl_arg(a, "patterns", 1);
	if (!pats || pats->type != VRL_ARRAY) { *err = vrl_errf("parse_groks: patterns must be an array"); return VRL_ERR; }
	for (size_t i = 0; i < pats->u.array.len; i++) {
		vrl_value *p = pats->u.array.items[i];
		if (p->type != VRL_BYTES) continue;
		char *e2 = NULL;
		vrl_value *res = NULL;
		if (grok_apply(p->u.bytes.data, p->u.bytes.len, s, n, a->ctx->ll, &res, &e2) == VRL_OK) {
			*out = res;
			return VRL_OK;
		}
		free(e2);
	}
	*err = vrl_errf("parse_groks: no pattern matched");
	return VRL_ERR;
}

void vrl_reg_parse(void)
{
	vrl_register("parse_int", fn_parse_int);
	vrl_register("parse_bytes", fn_parse_bytes);
	vrl_register("parse_duration", fn_parse_duration);
	vrl_register("parse_key_value", fn_parse_key_value);
	vrl_register("parse_logfmt", fn_parse_logfmt);
	vrl_register("parse_query_string", fn_parse_query_string);
	vrl_register("parse_csv", fn_parse_csv);
	vrl_register("parse_tokens", fn_parse_tokens);
	vrl_register("parse_regex_all", fn_parse_regex_all);
	vrl_register("parse_common_log", fn_parse_common_log);
	vrl_register("parse_apache_log", fn_parse_apache_log);
	vrl_register("parse_nginx_log", fn_parse_nginx_log);
	vrl_register("parse_glog", fn_parse_glog);
	vrl_register("parse_klog", fn_parse_klog);
	vrl_register("parse_syslog", fn_parse_syslog);
	vrl_register("parse_linux_authorization", fn_parse_linux_authorization);
	vrl_register("parse_cef", fn_parse_cef);
	vrl_register("parse_aws_vpc_flow_log", fn_parse_aws_vpc_flow_log);
	vrl_register("parse_aws_alb_log", fn_parse_aws_alb_log);
	vrl_register("parse_aws_cloudwatch_log_subscription_message", fn_parse_aws_cloudwatch_log_subscription_message);
	vrl_register("parse_ruby_hash", fn_parse_ruby_hash);
	vrl_register("parse_influxdb", fn_parse_influxdb);
	vrl_register("parse_url", fn_parse_url);
	vrl_register("parse_grok", fn_parse_grok);
	vrl_register("parse_groks", fn_parse_groks);
}
