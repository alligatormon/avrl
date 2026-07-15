#define _GNU_SOURCE
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ================================================================== */
/* word tokenising for case conversions                               */
/* ================================================================== */

/* Break a string into lowercase word tokens on non-alnum separators and
 * camelCase boundaries. Returns an array of bytes values. */
static void words_of(const char *s, size_t n, vrl_value *arr)
{
	avrl_buf w;
	avrl_buf_init(&w);
	int prev_alnum_lower = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isalnum(c)) {
			if (isupper(c) && prev_alnum_lower && w.len > 0) {
				vrl_array_push(arr, avrl_buf_to_bytes(&w));
			}
			avrl_buf_addc(&w, (char)tolower(c));
			prev_alnum_lower = islower(c) || isdigit(c);
		} else {
			if (w.len > 0)
				vrl_array_push(arr, avrl_buf_to_bytes(&w));
			prev_alnum_lower = 0;
		}
	}
	if (w.len > 0)
		vrl_array_push(arr, avrl_buf_to_bytes(&w));
	avrl_buf_free(&w);
}

typedef enum { CASE_SNAKE, CASE_SCREAM, CASE_KEBAB, CASE_CAMEL, CASE_PASCAL } case_style;

static vrl_status case_convert(vrl_call_args *a, vrl_value **out, char **err, case_style style)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *words = vrl_array_new();
	words_of(s, n, words);
	avrl_buf b;
	avrl_buf_init(&b);
	for (size_t i = 0; i < words->u.array.len; i++) {
		vrl_value *wv = words->u.array.items[i];
		const char *w = wv->u.bytes.data;
		size_t wl = wv->u.bytes.len;
		if (i > 0 && (style == CASE_SNAKE || style == CASE_SCREAM))
			avrl_buf_addc(&b, '_');
		if (i > 0 && style == CASE_KEBAB)
			avrl_buf_addc(&b, '-');
		for (size_t j = 0; j < wl; j++) {
			char c = w[j];
			switch (style) {
			case CASE_SCREAM: c = (char)toupper((unsigned char)c); break;
			case CASE_PASCAL: if (j == 0) c = (char)toupper((unsigned char)c); break;
			case CASE_CAMEL:  if (j == 0 && i > 0) c = (char)toupper((unsigned char)c); break;
			default: break; /* snake / kebab already lowercase */
			}
			avrl_buf_addc(&b, c);
		}
	}
	vrl_value_unref(words);
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

static vrl_status fn_snakecase(vrl_call_args *a, vrl_value **out, char **err) { return case_convert(a, out, err, CASE_SNAKE); }
static vrl_status fn_screamingsnakecase(vrl_call_args *a, vrl_value **out, char **err) { return case_convert(a, out, err, CASE_SCREAM); }
static vrl_status fn_kebabcase(vrl_call_args *a, vrl_value **out, char **err) { return case_convert(a, out, err, CASE_KEBAB); }
static vrl_status fn_camelcase(vrl_call_args *a, vrl_value **out, char **err) { return case_convert(a, out, err, CASE_CAMEL); }
static vrl_status fn_pascalcase(vrl_call_args *a, vrl_value **out, char **err) { return case_convert(a, out, err, CASE_PASCAL); }

/* ================================================================== */
/* basename / dirname / split_path                                    */
/* ================================================================== */

static vrl_status fn_basename(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	while (n > 1 && s[n - 1] == '/') n--; /* trim trailing slashes */
	size_t start = n;
	while (start > 0 && s[start - 1] != '/') start--;
	*out = vrl_bytes(s + start, n - start);
	return VRL_OK;
}

static vrl_status fn_dirname(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	while (n > 1 && s[n - 1] == '/') n--;
	size_t end = n;
	while (end > 0 && s[end - 1] != '/') end--;
	if (end == 0) { *out = vrl_bytes_cstr("."); return VRL_OK; }
	while (end > 1 && s[end - 1] == '/') end--; /* drop separating slash */
	*out = vrl_bytes(s, end);
	return VRL_OK;
}

static vrl_status fn_split_path(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *arr = vrl_array_new();
	size_t i = 0;
	while (i < n) {
		while (i < n && s[i] == '/') i++;
		size_t start = i;
		while (i < n && s[i] != '/') i++;
		if (i > start)
			vrl_array_push(arr, vrl_bytes(s + start, i - start));
	}
	*out = arr;
	return VRL_OK;
}

/* ================================================================== */
/* truncate / strlen (UTF-8 aware)                                    */
/* ================================================================== */

static size_t utf8_len(const char *s, size_t n)
{
	size_t count = 0;
	for (size_t i = 0; i < n; ) {
		unsigned char c = (unsigned char)s[i];
		size_t adv = 1;
		if (c >= 0xF0) adv = 4; else if (c >= 0xE0) adv = 3; else if (c >= 0xC0) adv = 2;
		i += adv;
		count++;
	}
	return count;
}

/* byte offset of the nth utf-8 char (clamped to n) */
static size_t utf8_offset(const char *s, size_t n, size_t chars)
{
	size_t i = 0, count = 0;
	while (i < n && count < chars) {
		unsigned char c = (unsigned char)s[i];
		size_t adv = 1;
		if (c >= 0xF0) adv = 4; else if (c >= 0xE0) adv = 3; else if (c >= 0xC0) adv = 2;
		if (i + adv > n) adv = n - i;
		i += adv;
		count++;
	}
	return i;
}

static vrl_status fn_strlen(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	*out = vrl_integer((int64_t)utf8_len(s, n));
	return VRL_OK;
}

static vrl_status fn_truncate(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *limv = vrl_arg(a, "limit", 1);
	if (!limv || limv->type != VRL_INTEGER) { *err = vrl_errf("truncate: limit must be integer"); return VRL_ERR; }
	int64_t limit = limv->u.integer;
	if (limit < 0) limit = 0;
	vrl_value *ell = vrl_arg(a, "ellipsis", 2);
	if (!ell) ell = vrl_arg(a, "suffix", 2);
	int add_ellipsis = ell && vrl_value_truthy(ell);
	size_t off = utf8_offset(s, n, (size_t)limit);
	if (off >= n) { *out = vrl_bytes(s, n); return VRL_OK; }
	avrl_buf b; avrl_buf_init(&b);
	avrl_buf_add(&b, s, off);
	if (add_ellipsis) avrl_buf_puts(&b, "...");
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

/* ================================================================== */
/* find / contains_all / match_any                                    */
/* ================================================================== */

static vrl_status fn_find(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *pat = vrl_arg(a, "pattern", 1);
	vrl_value *fromv = vrl_arg(a, "from", 2);
	size_t from = (fromv && fromv->type == VRL_INTEGER && fromv->u.integer > 0)
		      ? (size_t)fromv->u.integer : 0;
	if (from > n) from = n;
	if (pat && pat->type == VRL_REGEX) {
		int ov[AVRL_OVECCOUNT];
		int cnt = pcre_exec(pat->u.regex->regex_compiled, pat->u.regex->pcreExtra,
				    s, (int)n, (int)from, 0, ov, AVRL_OVECCOUNT);
		*out = vrl_integer(cnt > 0 ? ov[0] : -1);
		return VRL_OK;
	}
	if (!pat || pat->type != VRL_BYTES) { *err = vrl_errf("find: pattern must be string or regex"); return VRL_ERR; }
	const char *sub = pat->u.bytes.data; size_t sl = pat->u.bytes.len;
	int64_t idx = -1;
	if (sl == 0) idx = (int64_t)from;
	else for (size_t i = from; i + sl <= n; i++)
		if (!memcmp(s + i, sub, sl)) { idx = (int64_t)i; break; }
	*out = vrl_integer(idx);
	return VRL_OK;
}

static int str_contains(const char *s, size_t n, const char *sub, size_t sl, int ci)
{
	if (sl == 0) return 1;
	if (sl > n) return 0;
	for (size_t i = 0; i + sl <= n; i++) {
		int eq = 1;
		for (size_t j = 0; j < sl; j++) {
			char x = s[i + j], y = sub[j];
			if (ci) { x = (char)tolower((unsigned char)x); y = (char)tolower((unsigned char)y); }
			if (x != y) { eq = 0; break; }
		}
		if (eq) return 1;
	}
	return 0;
}

static vrl_status fn_contains_all(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *subs = vrl_arg(a, "substrings", 1);
	if (!subs || subs->type != VRL_ARRAY) { *err = vrl_errf("contains_all: substrings must be an array"); return VRL_ERR; }
	vrl_value *csv = vrl_arg(a, "case_sensitive", 2);
	int ci = csv && csv->type == VRL_BOOLEAN && !csv->u.boolean;
	int all = 1;
	for (size_t i = 0; i < subs->u.array.len; i++) {
		vrl_value *e = subs->u.array.items[i];
		if (e->type != VRL_BYTES) { all = 0; break; }
		if (!str_contains(s, n, e->u.bytes.data, e->u.bytes.len, ci)) { all = 0; break; }
	}
	*out = vrl_boolean(all);
	return VRL_OK;
}

static vrl_status fn_match_any(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *pats = vrl_arg(a, "patterns", 1);
	if (!pats || pats->type != VRL_ARRAY) { *err = vrl_errf("match_any: patterns must be an array"); return VRL_ERR; }
	int matched = 0;
	for (size_t i = 0; i < pats->u.array.len && !matched; i++) {
		vrl_value *p = pats->u.array.items[i];
		regex_match *re = NULL; int owned = 0;
		if (p->type == VRL_REGEX) re = p->u.regex;
		else if (p->type == VRL_BYTES) { re = avrl_regex_compile(p->u.bytes.data); owned = 1; }
		else continue;
		if (!re) continue;
		int ov[AVRL_OVECCOUNT];
		int cnt = avrl_regex_exec(re, s, n, ov, AVRL_OVECCOUNT, a->ctx->ll);
		if (cnt > 0) matched = 1;
		if (owned) avrl_regex_free(re);
	}
	*out = vrl_boolean(matched);
	return VRL_OK;
}

/* ================================================================== */
/* strip_ansi_escape_codes                                            */
/* ================================================================== */

static vrl_status fn_strip_ansi(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	avrl_buf b; avrl_buf_init(&b);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == 0x1b) { /* ESC */
			if (i + 1 < n && s[i + 1] == '[') {
				i += 2;
				while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e))
					i++;
				/* i now points at final byte; loop's i++ skips it */
			} else if (i + 1 < n && s[i + 1] == ']') {
				/* OSC: terminated by BEL or ST (ESC \) */
				i += 2;
				while (i < n && (unsigned char)s[i] != 0x07 &&
				       !((unsigned char)s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\'))
					i++;
				if (i < n && (unsigned char)s[i] == 0x1b) i++;
			} else {
				i++; /* skip the single char after ESC */
			}
			continue;
		}
		avrl_buf_addc(&b, (char)c);
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

/* ================================================================== */
/* shannon_entropy                                                    */
/* ================================================================== */

static vrl_status fn_shannon_entropy(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	if (n == 0) { *out = vrl_float(0.0); return VRL_OK; }
	size_t freq[256] = {0};
	for (size_t i = 0; i < n; i++) freq[(unsigned char)s[i]]++;
	double ent = 0.0;
	for (int i = 0; i < 256; i++) {
		if (!freq[i]) continue;
		double p = (double)freq[i] / (double)n;
		ent -= p * log2(p);
	}
	*out = vrl_float(ent);
	return VRL_OK;
}

/* ================================================================== */
/* parse_float                                                        */
/* ================================================================== */

static vrl_status fn_parse_float(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	char buf[128];
	if (n >= sizeof(buf)) { *err = vrl_errf("parse_float: input too long"); return VRL_ERR; }
	memcpy(buf, s, n); buf[n] = '\0';
	char *end = NULL;
	double d = strtod(buf, &end);
	if (end == buf) { *err = vrl_errf("parse_float: '%s' is not a float", buf); return VRL_ERR; }
	*out = vrl_float(d);
	return VRL_OK;
}

/* ================================================================== */
/* redact                                                             */
/* ================================================================== */

/* Apply one regex to a string, replacing every match with `repl`. */
static char *redact_regex(const char *s, size_t n, regex_match *re,
			  const char *repl, size_t *outlen)
{
	avrl_buf b; avrl_buf_init(&b);
	int ov[AVRL_OVECCOUNT];
	size_t pos = 0;
	while (pos <= n) {
		int cnt = pcre_exec(re->regex_compiled, re->pcreExtra, s, (int)n,
				    (int)pos, 0, ov, AVRL_OVECCOUNT);
		if (cnt <= 0) break;
		avrl_buf_add(&b, s + pos, (size_t)ov[0] - pos);
		avrl_buf_puts(&b, repl);
		size_t np = ov[1];
		if (np == pos) { if (pos < n) avrl_buf_addc(&b, s[pos]); np = pos + 1; }
		pos = np;
	}
	if (pos < n) avrl_buf_add(&b, s + pos, n - pos);
	b.s = realloc(b.s, b.len + 1);
	b.s[b.len] = '\0';
	*outlen = b.len;
	return b.s;
}

static vrl_value *redact_value(const vrl_value *v, vrl_value *filters, const char *repl);

static vrl_value *redact_string(const char *s, size_t n, vrl_value *filters, const char *repl)
{
	char *cur = malloc(n + 1);
	memcpy(cur, s, n); cur[n] = '\0';
	size_t curlen = n;
	for (size_t i = 0; i < filters->u.array.len; i++) {
		vrl_value *f = filters->u.array.items[i];
		regex_match *re = NULL; int owned = 0;
		if (f->type == VRL_REGEX) re = f->u.regex;
		else if (f->type == VRL_BYTES) { re = avrl_regex_compile(f->u.bytes.data); owned = 1; }
		else continue;
		if (!re) continue;
		size_t nl;
		char *nn = redact_regex(cur, curlen, re, repl, &nl);
		free(cur); cur = nn; curlen = nl;
		if (owned) avrl_regex_free(re);
	}
	return vrl_bytes_take(cur, curlen);
}

static vrl_value *redact_value(const vrl_value *v, vrl_value *filters, const char *repl)
{
	if (!v) return vrl_null();
	switch (v->type) {
	case VRL_BYTES:
		return redact_string(v->u.bytes.data, v->u.bytes.len, filters, repl);
	case VRL_ARRAY: {
		vrl_value *arr = vrl_array_new();
		for (size_t i = 0; i < v->u.array.len; i++)
			vrl_array_push(arr, redact_value(v->u.array.items[i], filters, repl));
		return arr;
	}
	case VRL_OBJECT: {
		vrl_value *obj = vrl_object_new();
		for (size_t i = 0; i < v->u.object.len; i++) {
			vrl_object_entry *e = &v->u.object.entries[i];
			vrl_object_set(obj, e->key, e->key_len, redact_value(e->val, filters, repl));
		}
		return obj;
	}
	default:
		return vrl_value_clone(v);
	}
}

static vrl_status fn_redact(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v) { *err = vrl_errf("redact: missing value"); return VRL_ERR; }
	vrl_value *filters = vrl_arg(a, "filters", 1);
	if (!filters || filters->type != VRL_ARRAY) {
		*err = vrl_errf("redact: filters must be an array of patterns/regexes");
		return VRL_ERR;
	}
	vrl_value *rv = vrl_arg(a, "redactor", 2);
	const char *repl = (rv && rv->type == VRL_BYTES) ? rv->u.bytes.data : "[REDACTED]";
	*out = redact_value(v, filters, repl);
	return VRL_OK;
}

/* ================================================================== */
/* sieve                                                              */
/* ================================================================== */

static vrl_status fn_sieve(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	int owned = 0;
	regex_match *re = NULL;
	vrl_value *pat = vrl_arg(a, "filters", 1);
	if (!pat) pat = vrl_arg(a, "pattern", 1);
	if (pat && pat->type == VRL_REGEX) re = pat->u.regex;
	else if (pat && pat->type == VRL_BYTES) { re = avrl_regex_compile(pat->u.bytes.data); owned = 1; }
	if (!re) { *err = vrl_errf("sieve: expected regex/pattern filter"); return VRL_ERR; }
	vrl_value *rs = vrl_arg(a, "replace_single", 2);
	vrl_value *rr = vrl_arg(a, "replace_repeated", 3);
	const char *single = (rs && rs->type == VRL_BYTES) ? rs->u.bytes.data : "";
	const char *repeated = (rr && rr->type == VRL_BYTES) ? rr->u.bytes.data : single;
	avrl_buf b; avrl_buf_init(&b);
	int ov[AVRL_OVECCOUNT];
	size_t run = 0;
	for (size_t i = 0; i < n; i++) {
		int cnt = pcre_exec(re->regex_compiled, re->pcreExtra, s, (int)n,
				    (int)i, PCRE_ANCHORED, ov, AVRL_OVECCOUNT);
		if (cnt > 0 && ov[0] == (int)i && ov[1] > (int)i) {
			run = 0;
			avrl_buf_add(&b, s + i, (size_t)(ov[1] - ov[0]));
			i = (size_t)ov[1] - 1;
		} else {
			run++;
			avrl_buf_puts(&b, run > 1 ? repeated : single);
		}
	}
	if (owned) avrl_regex_free(re);
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

/* ================================================================== */
/* replace_with (closure-based)                                       */
/* ================================================================== */

static vrl_value *match_object(const char *s, regex_match *re, int *ov, int cnt)
{
	vrl_value *m = vrl_object_new();
	vrl_object_set_cstr(m, "string", vrl_bytes(s + ov[0], (size_t)(ov[1] - ov[0])));
	vrl_value *caps = vrl_array_new();
	for (int i = 0; i < cnt; i++) {
		if (ov[2 * i] < 0) vrl_array_push(caps, vrl_null());
		else vrl_array_push(caps, vrl_bytes(s + ov[2 * i], (size_t)(ov[2 * i + 1] - ov[2 * i])));
	}
	vrl_object_set_cstr(m, "captures", caps);
	if (re->pcre_name_count > 0 && re->pcre_name_table) {
		const unsigned char *tab = re->pcre_name_table;
		for (int i = 0; i < re->pcre_name_count; i++) {
			const unsigned char *entry = tab + i * re->pcre_name_entry_size;
			int gidx = (entry[0] << 8) | entry[1];
			const char *gname = (const char *)(entry + 2);
			if (gidx < cnt && ov[2 * gidx] >= 0)
				vrl_object_set_cstr(m, gname, vrl_bytes(s + ov[2 * gidx], (size_t)(ov[2 * gidx + 1] - ov[2 * gidx])));
			else
				vrl_object_set_cstr(m, gname, vrl_null());
		}
	}
	return m;
}

static vrl_status fn_replace_with(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *pat = vrl_arg(a, "pattern", 1);
	regex_match *re = NULL; int owned = 0;
	if (pat && pat->type == VRL_REGEX) re = pat->u.regex;
	else if (pat && pat->type == VRL_BYTES) { re = avrl_regex_compile(pat->u.bytes.data); owned = 1; }
	if (!re) { *err = vrl_errf("replace_with: expected regex/pattern"); return VRL_ERR; }
	if (!a->closure) { if (owned) avrl_regex_free(re); *err = vrl_errf("replace_with: requires a closure"); return VRL_ERR; }
	vrl_value *countv = vrl_arg(a, "count", 2);
	int64_t max = (countv && countv->type == VRL_INTEGER) ? countv->u.integer : -1;

	avrl_buf b; avrl_buf_init(&b);
	int ov[AVRL_OVECCOUNT];
	size_t pos = 0; int64_t done = 0;
	vrl_status st = VRL_OK;
	while (pos <= n) {
		if (max >= 0 && done >= max) break;
		int cnt = pcre_exec(re->regex_compiled, re->pcreExtra, s, (int)n, (int)pos, 0, ov, AVRL_OVECCOUNT);
		if (cnt <= 0) break;
		avrl_buf_add(&b, s + pos, (size_t)ov[0] - pos);
		vrl_value *m = match_object(s, re, ov, cnt);
		vrl_value *params[1] = { m };
		vrl_value *repl = NULL;
		st = vrl_invoke_closure(a->ctx, a->closure, params, 1, &repl, err);
		vrl_value_unref(m);
		if (st != VRL_OK) { avrl_buf_free(&b); if (owned) avrl_regex_free(re); return st; }
		size_t rl; char *rs = vrl_value_to_string(repl, &rl);
		avrl_buf_add(&b, rs, rl);
		free(rs);
		vrl_value_unref(repl);
		done++;
		size_t np = ov[1];
		if (np == pos) { if (pos < n) avrl_buf_addc(&b, s[pos]); np = pos + 1; }
		pos = np;
	}
	if (pos < n) avrl_buf_add(&b, s + pos, n - pos);
	if (owned) avrl_regex_free(re);
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

void vrl_reg_string(void)
{
	vrl_register("snakecase", fn_snakecase);
	vrl_register("screamingsnakecase", fn_screamingsnakecase);
	vrl_register("kebabcase", fn_kebabcase);
	vrl_register("camelcase", fn_camelcase);
	vrl_register("pascalcase", fn_pascalcase);
	vrl_register("basename", fn_basename);
	vrl_register("dirname", fn_dirname);
	vrl_register("split_path", fn_split_path);
	vrl_register("strlen", fn_strlen);
	vrl_register("truncate", fn_truncate);
	vrl_register("find", fn_find);
	vrl_register("contains_all", fn_contains_all);
	vrl_register("match_any", fn_match_any);
	vrl_register("strip_ansi_escape_codes", fn_strip_ansi);
	vrl_register("shannon_entropy", fn_shannon_entropy);
	vrl_register("parse_float", fn_parse_float);
	vrl_register("redact", fn_redact);
	vrl_register("sieve", fn_sieve);
	vrl_register("replace_with", fn_replace_with);
}
