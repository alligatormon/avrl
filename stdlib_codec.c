#define _GNU_SOURCE
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* ================================================================== */
/* base16 (hex)                                                       */
/* ================================================================== */

static vrl_status fn_encode_base16(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	static const char *hex = "0123456789abcdef";
	char *buf = malloc(n * 2 + 1);
	for (size_t i = 0; i < n; i++) {
		buf[i * 2] = hex[(unsigned char)s[i] >> 4];
		buf[i * 2 + 1] = hex[(unsigned char)s[i] & 0xf];
	}
	buf[n * 2] = '\0';
	*out = vrl_bytes_take(buf, n * 2);
	return VRL_OK;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static vrl_status fn_decode_base16(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	if (n % 2) { *err = vrl_errf("decode_base16: odd length"); return VRL_ERR; }
	char *buf = malloc(n / 2 + 1);
	for (size_t i = 0; i < n; i += 2) {
		int hi = hexval((unsigned char)s[i]), lo = hexval((unsigned char)s[i + 1]);
		if (hi < 0 || lo < 0) { free(buf); *err = vrl_errf("decode_base16: invalid hex"); return VRL_ERR; }
		buf[i / 2] = (char)((hi << 4) | lo);
	}
	buf[n / 2] = '\0';
	*out = vrl_bytes_take(buf, n / 2);
	return VRL_OK;
}

/* ================================================================== */
/* base64                                                             */
/* ================================================================== */

static const char B64_STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static vrl_status fn_encode_base64(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	vrl_value *cs = vrl_arg(a, "charset", -1);
	const char *alpha = (cs && cs->type == VRL_BYTES && !strcmp(cs->u.bytes.data, "url_safe")) ? B64_URL : B64_STD;
	vrl_value *padv = vrl_arg(a, "padding", -1);
	int pad = padv ? vrl_value_truthy(padv) : 1;
	avrl_buf b; avrl_buf_init(&b);
	size_t i = 0;
	for (; i + 3 <= n; i += 3) {
		unsigned v = ((unsigned char)s[i] << 16) | ((unsigned char)s[i + 1] << 8) | (unsigned char)s[i + 2];
		avrl_buf_addc(&b, alpha[(v >> 18) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 12) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 6) & 63]);
		avrl_buf_addc(&b, alpha[v & 63]);
	}
	size_t rem = n - i;
	if (rem == 1) {
		unsigned v = (unsigned char)s[i] << 16;
		avrl_buf_addc(&b, alpha[(v >> 18) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 12) & 63]);
		if (pad) { avrl_buf_addc(&b, '='); avrl_buf_addc(&b, '='); }
	} else if (rem == 2) {
		unsigned v = ((unsigned char)s[i] << 16) | ((unsigned char)s[i + 1] << 8);
		avrl_buf_addc(&b, alpha[(v >> 18) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 12) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 6) & 63]);
		if (pad) avrl_buf_addc(&b, '=');
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

static vrl_status fn_decode_base64(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	avrl_buf b; avrl_buf_init(&b);
	int quad[4], qn = 0;
	for (size_t i = 0; i < n; i++) {
		int c = (unsigned char)s[i];
		if (c == '=' || isspace(c)) continue;
		int v = b64val(c);
		if (v < 0) { avrl_buf_free(&b); *err = vrl_errf("decode_base64: invalid character"); return VRL_ERR; }
		quad[qn++] = v;
		if (qn == 4) {
			avrl_buf_addc(&b, (char)((quad[0] << 2) | (quad[1] >> 4)));
			avrl_buf_addc(&b, (char)((quad[1] << 4) | (quad[2] >> 2)));
			avrl_buf_addc(&b, (char)((quad[2] << 6) | quad[3]));
			qn = 0;
		}
	}
	if (qn == 2) {
		avrl_buf_addc(&b, (char)((quad[0] << 2) | (quad[1] >> 4)));
	} else if (qn == 3) {
		avrl_buf_addc(&b, (char)((quad[0] << 2) | (quad[1] >> 4)));
		avrl_buf_addc(&b, (char)((quad[1] << 4) | (quad[2] >> 2)));
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

/* ================================================================== */
/* percent (URL) encoding                                             */
/* ================================================================== */

static int pct_unreserved(int c)
{
	return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static vrl_status fn_encode_percent(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	static const char *hex = "0123456789ABCDEF";
	avrl_buf b; avrl_buf_init(&b);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (pct_unreserved(c)) {
			avrl_buf_addc(&b, (char)c);
		} else {
			avrl_buf_addc(&b, '%');
			avrl_buf_addc(&b, hex[c >> 4]);
			avrl_buf_addc(&b, hex[c & 0xf]);
		}
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

static vrl_status fn_decode_percent(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	avrl_buf b; avrl_buf_init(&b);
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '%' && i + 2 < n) {
			int hi = hexval((unsigned char)s[i + 1]), lo = hexval((unsigned char)s[i + 2]);
			if (hi >= 0 && lo >= 0) {
				avrl_buf_addc(&b, (char)((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		avrl_buf_addc(&b, s[i]);
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

/* ================================================================== */
/* decode_mime_q                                                      */
/* ================================================================== */

/* Decode a single "=?charset?enc?text?=" encoded-word run within the string. */
static vrl_status fn_decode_mime_q(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	avrl_buf b; avrl_buf_init(&b);
	size_t i = 0;
	while (i < n) {
		if (i + 1 < n && s[i] == '=' && s[i + 1] == '?') {
			/* parse =?charset?E?text?= */
			size_t p = i + 2;
			/* charset */
			while (p < n && s[p] != '?') p++;
			if (p >= n) { avrl_buf_addc(&b, s[i]); i++; continue; }
			p++; /* skip ? */
			if (p >= n) break;
			char enc = (char)toupper((unsigned char)s[p]);
			p++;
			if (p >= n || s[p] != '?') { avrl_buf_addc(&b, s[i]); i++; continue; }
			p++; /* skip ? before text */
			size_t tstart = p;
			while (p + 1 < n && !(s[p] == '?' && s[p + 1] == '=')) p++;
			size_t tend = p;
			if (enc == 'B') {
				/* base64 the [tstart,tend) */
				int quad[4], qn = 0;
				for (size_t k = tstart; k < tend; k++) {
					int c = (unsigned char)s[k];
					if (c == '=' ) continue;
					int v = b64val(c);
					if (v < 0) continue;
					quad[qn++] = v;
					if (qn == 4) {
						avrl_buf_addc(&b, (char)((quad[0] << 2) | (quad[1] >> 4)));
						avrl_buf_addc(&b, (char)((quad[1] << 4) | (quad[2] >> 2)));
						avrl_buf_addc(&b, (char)((quad[2] << 6) | quad[3]));
						qn = 0;
					}
				}
				if (qn == 2) avrl_buf_addc(&b, (char)((quad[0] << 2) | (quad[1] >> 4)));
				else if (qn == 3) {
					avrl_buf_addc(&b, (char)((quad[0] << 2) | (quad[1] >> 4)));
					avrl_buf_addc(&b, (char)((quad[1] << 4) | (quad[2] >> 2)));
				}
			} else { /* Q encoding */
				for (size_t k = tstart; k < tend; k++) {
					if (s[k] == '_') { avrl_buf_addc(&b, ' '); }
					else if (s[k] == '=' && k + 2 < tend) {
						int hi = hexval((unsigned char)s[k + 1]), lo = hexval((unsigned char)s[k + 2]);
						if (hi >= 0 && lo >= 0) { avrl_buf_addc(&b, (char)((hi << 4) | lo)); k += 2; }
						else avrl_buf_addc(&b, s[k]);
					} else avrl_buf_addc(&b, s[k]);
				}
			}
			i = (tend + 1 < n) ? tend + 2 : n; /* skip closing ?= */
		} else {
			avrl_buf_addc(&b, s[i]);
			i++;
		}
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

/* ================================================================== */
/* punycode (RFC 3492)                                                */
/* ================================================================== */

#define PUNY_BASE 36
#define PUNY_TMIN 1
#define PUNY_TMAX 26
#define PUNY_SKEW 38
#define PUNY_DAMP 700
#define PUNY_INITIAL_BIAS 72
#define PUNY_INITIAL_N 128

static uint32_t puny_adapt(uint32_t delta, uint32_t numpoints, int firsttime)
{
	uint32_t k = 0;
	delta = firsttime ? delta / PUNY_DAMP : delta / 2;
	delta += delta / numpoints;
	while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) {
		delta /= (PUNY_BASE - PUNY_TMIN);
		k += PUNY_BASE;
	}
	return k + (((PUNY_BASE - PUNY_TMIN + 1) * delta) / (delta + PUNY_SKEW));
}

static int puny_digit(int cp)
{
	if (cp >= '0' && cp <= '9') return cp - '0' + 26;
	if (cp >= 'A' && cp <= 'Z') return cp - 'A';
	if (cp >= 'a' && cp <= 'z') return cp - 'a';
	return -1;
}

static char puny_encode_digit(uint32_t d)
{
	return (char)(d < 26 ? d + 'a' : d - 26 + '0');
}

/* utf-8 helpers */
static size_t utf8_decode_all(const char *s, size_t n, uint32_t *cps, size_t maxcp)
{
	size_t ci = 0;
	for (size_t i = 0; i < n && ci < maxcp; ) {
		unsigned char c = (unsigned char)s[i];
		uint32_t cp; size_t adv;
		if (c < 0x80) { cp = c; adv = 1; }
		else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); adv = 2; }
		else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); adv = 3; }
		else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); adv = 4; }
		else { cp = c; adv = 1; }
		cps[ci++] = cp;
		i += adv;
	}
	return ci;
}

static void utf8_encode_cp(avrl_buf *b, uint32_t cp)
{
	if (cp < 0x80) avrl_buf_addc(b, (char)cp);
	else if (cp < 0x800) {
		avrl_buf_addc(b, (char)(0xC0 | (cp >> 6)));
		avrl_buf_addc(b, (char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		avrl_buf_addc(b, (char)(0xE0 | (cp >> 12)));
		avrl_buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		avrl_buf_addc(b, (char)(0x80 | (cp & 0x3F)));
	} else {
		avrl_buf_addc(b, (char)(0xF0 | (cp >> 18)));
		avrl_buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
		avrl_buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		avrl_buf_addc(b, (char)(0x80 | (cp & 0x3F)));
	}
}

/* Encode a single label (no xn-- prefix) of code points into ascii punycode.
 * Returns 1 on success. */
static int puny_encode_label(const uint32_t *cps, size_t len, avrl_buf *out)
{
	uint32_t n = PUNY_INITIAL_N, delta = 0, bias = PUNY_INITIAL_BIAS;
	size_t h = 0, basic = 0;
	for (size_t i = 0; i < len; i++)
		if (cps[i] < 0x80) { avrl_buf_addc(out, (char)cps[i]); basic++; }
	h = basic;
	if (basic > 0) avrl_buf_addc(out, '-');
	while (h < len) {
		uint32_t m = 0xFFFFFFFF;
		for (size_t i = 0; i < len; i++)
			if (cps[i] >= n && cps[i] < m) m = cps[i];
		delta += (m - n) * (uint32_t)(h + 1);
		n = m;
		for (size_t i = 0; i < len; i++) {
			if (cps[i] < n) delta++;
			if (cps[i] == n) {
				uint32_t q = delta;
				for (uint32_t k = PUNY_BASE;; k += PUNY_BASE) {
					uint32_t t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
					if (q < t) break;
					avrl_buf_addc(out, puny_encode_digit(t + (q - t) % (PUNY_BASE - t)));
					q = (q - t) / (PUNY_BASE - t);
				}
				avrl_buf_addc(out, puny_encode_digit(q));
				bias = puny_adapt(delta, (uint32_t)(h + 1), h == basic);
				delta = 0;
				h++;
			}
		}
		delta++;
		n++;
	}
	return 1;
}

/* Decode a single punycode label (without xn-- prefix) into code points buf. */
static int puny_decode_label(const char *s, size_t len, avrl_buf *out)
{
	uint32_t n = PUNY_INITIAL_N, i = 0, bias = PUNY_INITIAL_BIAS;
	/* find last delimiter */
	size_t basic = 0;
	for (size_t j = 0; j < len; j++) if (s[j] == '-') basic = j + 1;
	/* output codepoints stored in a temp array */
	size_t cap = len + 8;
	uint32_t *output = malloc(cap * sizeof(uint32_t));
	size_t olen = 0;
	/* copy basic code points (excluding the delimiter itself) */
	if (basic > 0) {
		for (size_t j = 0; j < basic - 1; j++) {
			if (olen >= cap) { cap *= 2; output = realloc(output, cap * sizeof(uint32_t)); }
			output[olen++] = (unsigned char)s[j];
		}
	}
	size_t pos = basic;
	while (pos < len) {
		uint32_t oldi = i, w = 1;
		for (uint32_t k = PUNY_BASE;; k += PUNY_BASE) {
			if (pos >= len) { free(output); return 0; }
			int d = puny_digit((unsigned char)s[pos++]);
			if (d < 0) { free(output); return 0; }
			i += (uint32_t)d * w;
			uint32_t t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
			if ((uint32_t)d < t) break;
			w *= (PUNY_BASE - t);
		}
		uint32_t outlen1 = (uint32_t)olen + 1;
		bias = puny_adapt(i - oldi, outlen1, oldi == 0);
		n += i / outlen1;
		i %= outlen1;
		/* insert n at position i */
		if (olen >= cap) { cap *= 2; output = realloc(output, cap * sizeof(uint32_t)); }
		for (size_t j = olen; j > i; j--) output[j] = output[j - 1];
		output[i] = n;
		olen++;
		i++;
	}
	for (size_t j = 0; j < olen; j++) utf8_encode_cp(out, output[j]);
	free(output);
	return 1;
}

static vrl_status fn_encode_punycode(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	avrl_buf res; avrl_buf_init(&res);
	size_t start = 0;
	while (start <= n) {
		size_t end = start;
		while (end < n && s[end] != '.') end++;
		/* label [start,end) */
		size_t llen = end - start;
		int has_non_ascii = 0;
		for (size_t i = start; i < end; i++) if ((unsigned char)s[i] >= 0x80) { has_non_ascii = 1; break; }
		if (has_non_ascii) {
			uint32_t *cps = malloc((llen + 1) * sizeof(uint32_t));
			size_t ncp = utf8_decode_all(s + start, llen, cps, llen + 1);
			avrl_buf_puts(&res, "xn--");
			puny_encode_label(cps, ncp, &res);
			free(cps);
		} else {
			avrl_buf_add(&res, s + start, llen);
		}
		if (end < n) avrl_buf_addc(&res, '.');
		if (end >= n) break;
		start = end + 1;
	}
	*out = avrl_buf_to_bytes(&res);
	avrl_buf_free(&res);
	return VRL_OK;
}

static vrl_status fn_decode_punycode(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	avrl_buf res; avrl_buf_init(&res);
	size_t start = 0;
	while (start <= n) {
		size_t end = start;
		while (end < n && s[end] != '.') end++;
		size_t llen = end - start;
		if (llen >= 4 && !strncasecmp(s + start, "xn--", 4)) {
			if (!puny_decode_label(s + start + 4, llen - 4, &res)) {
				avrl_buf_free(&res);
				*err = vrl_errf("decode_punycode: invalid input");
				return VRL_ERR;
			}
		} else {
			avrl_buf_add(&res, s + start, llen);
		}
		if (end < n) avrl_buf_addc(&res, '.');
		if (end >= n) break;
		start = end + 1;
	}
	*out = avrl_buf_to_bytes(&res);
	avrl_buf_free(&res);
	return VRL_OK;
}

/* ================================================================== */
/* encode_csv / encode_key_value / encode_logfmt                      */
/* ================================================================== */

static vrl_status fn_encode_csv(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_ARRAY) { *err = vrl_errf("encode_csv: expected array"); return VRL_ERR; }
	vrl_value *delv = vrl_arg(a, "delimiter", -1);
	char delim = (delv && delv->type == VRL_BYTES && delv->u.bytes.len == 1) ? delv->u.bytes.data[0] : ',';
	avrl_buf b; avrl_buf_init(&b);
	for (size_t i = 0; i < v->u.array.len; i++) {
		if (i) avrl_buf_addc(&b, delim);
		size_t fl; char *f = vrl_value_to_string(v->u.array.items[i], &fl);
		int need_quote = 0;
		for (size_t j = 0; j < fl; j++)
			if (f[j] == delim || f[j] == '"' || f[j] == '\n' || f[j] == '\r') { need_quote = 1; break; }
		if (need_quote) {
			avrl_buf_addc(&b, '"');
			for (size_t j = 0; j < fl; j++) {
				if (f[j] == '"') avrl_buf_addc(&b, '"');
				avrl_buf_addc(&b, f[j]);
			}
			avrl_buf_addc(&b, '"');
		} else {
			avrl_buf_add(&b, f, fl);
		}
		free(f);
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

static void kv_emit_value(avrl_buf *b, vrl_value *val)
{
	size_t vl; char *vs = vrl_value_to_string(val, &vl);
	int need_quote = (vl == 0);
	for (size_t j = 0; j < vl; j++)
		if (vs[j] == ' ' || vs[j] == '"' || vs[j] == '=') { need_quote = 1; break; }
	if (need_quote) {
		avrl_buf_addc(b, '"');
		for (size_t j = 0; j < vl; j++) {
			if (vs[j] == '"' || vs[j] == '\\') avrl_buf_addc(b, '\\');
			avrl_buf_addc(b, vs[j]);
		}
		avrl_buf_addc(b, '"');
	} else {
		avrl_buf_add(b, vs, vl);
	}
	free(vs);
}

static vrl_status encode_kv(vrl_call_args *a, vrl_value **out, char **err,
			    const char *default_field_delim)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_OBJECT) { *err = vrl_errf("expected object"); return VRL_ERR; }
	vrl_value *kvd = vrl_arg(a, "key_value_delimiter", -1);
	vrl_value *fd = vrl_arg(a, "field_delimiter", -1);
	const char *kv_delim = (kvd && kvd->type == VRL_BYTES) ? kvd->u.bytes.data : "=";
	const char *field_delim = (fd && fd->type == VRL_BYTES) ? fd->u.bytes.data : default_field_delim;
	avrl_buf b; avrl_buf_init(&b);
	for (size_t i = 0; i < v->u.object.len; i++) {
		vrl_object_entry *e = &v->u.object.entries[i];
		if (i) avrl_buf_puts(&b, field_delim);
		avrl_buf_add(&b, e->key, e->key_len);
		avrl_buf_puts(&b, kv_delim);
		kv_emit_value(&b, e->val);
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

static vrl_status fn_encode_key_value(vrl_call_args *a, vrl_value **out, char **err)
{
	return encode_kv(a, out, err, " ");
}

static vrl_status fn_encode_logfmt(vrl_call_args *a, vrl_value **out, char **err)
{
	return encode_kv(a, out, err, " ");
}

void vrl_reg_codec(void)
{
	vrl_register("encode_base16", fn_encode_base16);
	vrl_register("decode_base16", fn_decode_base16);
	vrl_register("encode_base64", fn_encode_base64);
	vrl_register("decode_base64", fn_decode_base64);
	vrl_register("encode_percent", fn_encode_percent);
	vrl_register("decode_percent", fn_decode_percent);
	vrl_register("decode_mime_q", fn_decode_mime_q);
	vrl_register("encode_punycode", fn_encode_punycode);
	vrl_register("decode_punycode", fn_decode_punycode);
	vrl_register("encode_csv", fn_encode_csv);
	vrl_register("encode_key_value", fn_encode_key_value);
	vrl_register("encode_logfmt", fn_encode_logfmt);
}
