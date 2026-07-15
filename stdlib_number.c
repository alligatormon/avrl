#define _GNU_SOURCE
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* ================================================================== */
/* format_int / format_number                                         */
/* ================================================================== */

static vrl_status fn_format_int(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_INTEGER) { *err = vrl_errf("format_int: expected integer"); return VRL_ERR; }
	vrl_value *bv = vrl_arg(a, "base", 1);
	int base = (bv && bv->type == VRL_INTEGER) ? (int)bv->u.integer : 10;
	if (base < 2 || base > 36) { *err = vrl_errf("format_int: base must be 2..36"); return VRL_ERR; }
	int64_t n = v->u.integer;
	int neg = n < 0;
	uint64_t u = neg ? (uint64_t)(-(n + 1)) + 1 : (uint64_t)n;
	char tmp[80]; int ti = 0;
	static const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
	if (u == 0) tmp[ti++] = '0';
	while (u) { tmp[ti++] = digits[u % (uint64_t)base]; u /= (uint64_t)base; }
	avrl_buf b; avrl_buf_init(&b);
	if (neg) avrl_buf_addc(&b, '-');
	while (ti) avrl_buf_addc(&b, tmp[--ti]);
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

static vrl_status fn_format_number(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	double x;
	if (v && v->type == VRL_INTEGER) x = (double)v->u.integer;
	else if (v && v->type == VRL_FLOAT) x = v->u.flt;
	else { *err = vrl_errf("format_number: expected number"); return VRL_ERR; }
	vrl_value *scalev = vrl_arg(a, "scale", 1);
	vrl_value *decv = vrl_arg(a, "decimal_separator", 2);
	vrl_value *grpv = vrl_arg(a, "grouping_separator", 3);
	const char *dec = (decv && decv->type == VRL_BYTES) ? decv->u.bytes.data : ".";
	const char *grp = (grpv && grpv->type == VRL_BYTES) ? grpv->u.bytes.data : ",";
	int scale = (scalev && scalev->type == VRL_INTEGER) ? (int)scalev->u.integer : -1;

	char numbuf[64];
	if (scale >= 0) snprintf(numbuf, sizeof(numbuf), "%.*f", scale, x);
	else snprintf(numbuf, sizeof(numbuf), "%g", x);

	char *dot = strchr(numbuf, '.');
	char *intpart = numbuf;
	int neg = (numbuf[0] == '-');
	if (neg) intpart++;
	size_t intlen = dot ? (size_t)(dot - intpart) : strlen(intpart);
	avrl_buf b; avrl_buf_init(&b);
	if (neg) avrl_buf_addc(&b, '-');
	for (size_t i = 0; i < intlen; i++) {
		if (i > 0 && (intlen - i) % 3 == 0)
			avrl_buf_puts(&b, grp);
		avrl_buf_addc(&b, intpart[i]);
	}
	if (dot) {
		avrl_buf_puts(&b, dec);
		avrl_buf_puts(&b, dot + 1);
	}
	*out = avrl_buf_to_bytes(&b);
	avrl_buf_free(&b);
	return VRL_OK;
}

/* ================================================================== */
/* unix timestamp conversions                                         */
/* ================================================================== */

static double unit_scale(vrl_call_args *a)
{
	vrl_value *u = vrl_arg(a, "unit", 1);
	if (u && u->type == VRL_BYTES) {
		if (!strcmp(u->u.bytes.data, "milliseconds")) return 1e3;
		if (!strcmp(u->u.bytes.data, "nanoseconds")) return 1e9;
	}
	return 1.0;
}

static vrl_status fn_from_unix_timestamp(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	double n;
	if (v && v->type == VRL_INTEGER) n = (double)v->u.integer;
	else if (v && v->type == VRL_FLOAT) n = v->u.flt;
	else { *err = vrl_errf("from_unix_timestamp: expected number"); return VRL_ERR; }
	*out = vrl_timestamp(n / unit_scale(a));
	return VRL_OK;
}

static vrl_status fn_to_unix_timestamp(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_TIMESTAMP) { *err = vrl_errf("to_unix_timestamp: expected timestamp"); return VRL_ERR; }
	double scale = unit_scale(a);
	*out = vrl_integer((int64_t)(v->u.timestamp * scale));
	return VRL_OK;
}

/* ================================================================== */
/* syslog converters                                                  */
/* ================================================================== */

static const char *SEVERITY[] = {
	"emergency", "alert", "critical", "error",
	"warning", "notice", "informational", "debug"
};

static const char *FACILITY[] = {
	"kern", "user", "mail", "daemon", "auth", "syslog", "lpr", "news",
	"uucp", "cron", "authpriv", "ftp", "ntp", "security", "console",
	"solaris-cron", "local0", "local1", "local2", "local3", "local4",
	"local5", "local6", "local7"
};

static vrl_status fn_to_syslog_level(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_INTEGER || v->u.integer < 0 || v->u.integer > 7) {
		*err = vrl_errf("to_syslog_level: severity must be 0..7"); return VRL_ERR;
	}
	*out = vrl_bytes_cstr(SEVERITY[v->u.integer]);
	return VRL_OK;
}

static vrl_status fn_to_syslog_severity(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	for (int i = 0; i < 8; i++)
		if (!strcasecmp(s, SEVERITY[i])) { *out = vrl_integer(i); return VRL_OK; }
	/* common short aliases */
	if (!strcasecmp(s, "emerg")) { *out = vrl_integer(0); return VRL_OK; }
	if (!strcasecmp(s, "crit")) { *out = vrl_integer(2); return VRL_OK; }
	if (!strcasecmp(s, "err")) { *out = vrl_integer(3); return VRL_OK; }
	if (!strcasecmp(s, "warn")) { *out = vrl_integer(4); return VRL_OK; }
	if (!strcasecmp(s, "info")) { *out = vrl_integer(6); return VRL_OK; }
	*err = vrl_errf("to_syslog_severity: unknown level '%s'", s);
	return VRL_ERR;
}

static vrl_status fn_to_syslog_facility(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	int cnt = (int)(sizeof(FACILITY) / sizeof(FACILITY[0]));
	if (!v || v->type != VRL_INTEGER || v->u.integer < 0 || v->u.integer >= cnt) {
		*err = vrl_errf("to_syslog_facility: code out of range"); return VRL_ERR;
	}
	*out = vrl_bytes_cstr(FACILITY[v->u.integer]);
	return VRL_OK;
}

static vrl_status fn_to_syslog_facility_code(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	int cnt = (int)(sizeof(FACILITY) / sizeof(FACILITY[0]));
	for (int i = 0; i < cnt; i++)
		if (!strcasecmp(s, FACILITY[i])) { *out = vrl_integer(i); return VRL_OK; }
	*err = vrl_errf("to_syslog_facility_code: unknown facility '%s'", s);
	return VRL_ERR;
}

/* ================================================================== */
/* system                                                             */
/* ================================================================== */

static vrl_status fn_get_env_var(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "name", 0, &s, &n, err)) return VRL_ERR;
	const char *val = getenv(s);
	if (!val) { *err = vrl_errf("get_env_var: '%s' is not set", s); return VRL_ERR; }
	*out = vrl_bytes_cstr(val);
	return VRL_OK;
}

static vrl_status fn_get_hostname(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)a;
	char buf[256];
	if (gethostname(buf, sizeof(buf)) != 0) { *err = vrl_errf("get_hostname: failed"); return VRL_ERR; }
	buf[sizeof(buf) - 1] = '\0';
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

static vrl_status fn_get_timezone_name(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)a; (void)err;
	const char *tz = getenv("TZ");
	if (tz && *tz) { *out = vrl_bytes_cstr(tz); return VRL_OK; }
	tzset();
	if (tzname[0] && *tzname[0]) { *out = vrl_bytes_cstr(tzname[0]); return VRL_OK; }
	*out = vrl_bytes_cstr("UTC");
	return VRL_OK;
}

/* ================================================================== */
/* haversine                                                          */
/* ================================================================== */

static vrl_status fn_haversine(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *la1 = vrl_arg(a, "latitude1", 0);
	vrl_value *lo1 = vrl_arg(a, "longitude1", 1);
	vrl_value *la2 = vrl_arg(a, "latitude2", 2);
	vrl_value *lo2 = vrl_arg(a, "longitude2", 3);
	double v[4]; vrl_value *args[4] = { la1, lo1, la2, lo2 };
	for (int i = 0; i < 4; i++) {
		if (args[i] && args[i]->type == VRL_INTEGER) v[i] = (double)args[i]->u.integer;
		else if (args[i] && args[i]->type == VRL_FLOAT) v[i] = args[i]->u.flt;
		else { *err = vrl_errf("haversine: expected 4 numeric coordinates"); return VRL_ERR; }
	}
	double R = 6371.0; /* km */
	vrl_value *ms = vrl_arg(a, "measurement_system", 4);
	if (ms && ms->type == VRL_BYTES && !strcmp(ms->u.bytes.data, "imperial"))
		R = 3958.8; /* miles */
	double rad = M_PI / 180.0;
	double dlat = (v[2] - v[0]) * rad;
	double dlon = (v[3] - v[1]) * rad;
	double h = sin(dlat / 2) * sin(dlat / 2) +
		   cos(v[0] * rad) * cos(v[2] * rad) * sin(dlon / 2) * sin(dlon / 2);
	double dist = 2 * R * asin(sqrt(h));
	*out = vrl_float(dist);
	return VRL_OK;
}

/* ================================================================== */
/* checksum: crc / xxhash / seahash                                   */
/* ================================================================== */

static uint32_t crc32_ieee(const unsigned char *s, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		crc ^= s[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
	}
	return crc ^ 0xFFFFFFFFu;
}

static vrl_status fn_crc(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	uint32_t crc = crc32_ieee((const unsigned char *)s, n);
	char buf[32];
	snprintf(buf, sizeof(buf), "%u", crc);
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

/* XXH64 */
#define XXP1 11400714785074694791ULL
#define XXP2 14029467366897019727ULL
#define XXP3 1609587929392839161ULL
#define XXP4 9650029242287828579ULL
#define XXP5 2870177450012600261ULL

static uint64_t xx_rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t xx_read64(const unsigned char *p)
{
	uint64_t v; memcpy(&v, p, 8); return v; /* assume little-endian host */
}
static uint32_t xx_read32(const unsigned char *p)
{
	uint32_t v; memcpy(&v, p, 4); return v;
}
static uint64_t xx_round(uint64_t acc, uint64_t input)
{
	acc += input * XXP2;
	acc = xx_rotl(acc, 31);
	acc *= XXP1;
	return acc;
}
static uint64_t xxh64(const unsigned char *p, size_t len, uint64_t seed)
{
	const unsigned char *end = p + len;
	uint64_t h;
	if (len >= 32) {
		const unsigned char *limit = end - 32;
		uint64_t v1 = seed + XXP1 + XXP2, v2 = seed + XXP2, v3 = seed, v4 = seed - XXP1;
		do {
			v1 = xx_round(v1, xx_read64(p)); p += 8;
			v2 = xx_round(v2, xx_read64(p)); p += 8;
			v3 = xx_round(v3, xx_read64(p)); p += 8;
			v4 = xx_round(v4, xx_read64(p)); p += 8;
		} while (p <= limit);
		h = xx_rotl(v1, 1) + xx_rotl(v2, 7) + xx_rotl(v3, 12) + xx_rotl(v4, 18);
		h = (h ^ (xx_round(0, v1))) * XXP1 + XXP4;
		h = (h ^ (xx_round(0, v2))) * XXP1 + XXP4;
		h = (h ^ (xx_round(0, v3))) * XXP1 + XXP4;
		h = (h ^ (xx_round(0, v4))) * XXP1 + XXP4;
	} else {
		h = seed + XXP5;
	}
	h += (uint64_t)len;
	while (p + 8 <= end) {
		h ^= xx_round(0, xx_read64(p));
		h = xx_rotl(h, 27) * XXP1 + XXP4;
		p += 8;
	}
	if (p + 4 <= end) {
		h ^= (uint64_t)xx_read32(p) * XXP1;
		h = xx_rotl(h, 23) * XXP2 + XXP3;
		p += 4;
	}
	while (p < end) {
		h ^= (uint64_t)(*p) * XXP5;
		h = xx_rotl(h, 11) * XXP1;
		p++;
	}
	h ^= h >> 33;
	h *= XXP2;
	h ^= h >> 29;
	h *= XXP3;
	h ^= h >> 32;
	return h;
}

static vrl_status fn_xxhash(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	uint64_t h = xxh64((const unsigned char *)s, n, 0);
	*out = vrl_integer((int64_t)h);
	return VRL_OK;
}

/* SeaHash (reference algorithm) */
static uint64_t sea_diffuse(uint64_t x)
{
	x *= 0x6eed0e9da4d94a4fULL;
	uint64_t a = x >> 32;
	uint64_t b = x >> 60;
	x ^= a >> b;
	x *= 0x6eed0e9da4d94a4fULL;
	return x;
}

static uint64_t sea_read_int(const unsigned char *p, size_t n)
{
	uint64_t x = 0;
	for (size_t i = 0; i < n; i++) x |= (uint64_t)p[i] << (8 * i);
	return x;
}

static uint64_t seahash(const unsigned char *buf, size_t len)
{
	uint64_t a = 0x16f11fe89b0d677cULL, b = 0xb480a793d8e6c86cULL;
	uint64_t c = 0x6fe2e5aaf078ebc9ULL, d = 0x14f994a4c5259381ULL;
	size_t i = 0;
	while (i + 32 <= len) {
		a = sea_diffuse(a ^ sea_read_int(buf + i, 8));
		b = sea_diffuse(b ^ sea_read_int(buf + i + 8, 8));
		c = sea_diffuse(c ^ sea_read_int(buf + i + 16, 8));
		d = sea_diffuse(d ^ sea_read_int(buf + i + 24, 8));
		i += 32;
	}
	size_t excessive = len - i;
	const unsigned char *p = buf + i;
	if (excessive > 0) {
		if (excessive <= 8) {
			a = sea_diffuse(a ^ sea_read_int(p, excessive));
		} else if (excessive <= 16) {
			a = sea_diffuse(a ^ sea_read_int(p, 8));
			b = sea_diffuse(b ^ sea_read_int(p + 8, excessive - 8));
		} else if (excessive <= 24) {
			a = sea_diffuse(a ^ sea_read_int(p, 8));
			b = sea_diffuse(b ^ sea_read_int(p + 8, 8));
			c = sea_diffuse(c ^ sea_read_int(p + 16, excessive - 16));
		} else {
			a = sea_diffuse(a ^ sea_read_int(p, 8));
			b = sea_diffuse(b ^ sea_read_int(p + 8, 8));
			c = sea_diffuse(c ^ sea_read_int(p + 16, 8));
			d = sea_diffuse(d ^ sea_read_int(p + 24, excessive - 24));
		}
	}
	a ^= b;
	c ^= d;
	a ^= c;
	a ^= (uint64_t)len;
	return sea_diffuse(a);
}

static vrl_status fn_seahash(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	uint64_t h = seahash((const unsigned char *)s, n);
	*out = vrl_integer((int64_t)h);
	return VRL_OK;
}

/* ================================================================== */
/* IP functions                                                       */
/* ================================================================== */

static int str_of(vrl_value *v, const char **s)
{
	if (v && v->type == VRL_BYTES) { *s = v->u.bytes.data; return 1; }
	return 0;
}

static vrl_status fn_is_ipv4(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	const char *s; struct in_addr x;
	*out = vrl_boolean(str_of(vrl_arg(a, "value", 0), &s) && inet_pton(AF_INET, s, &x) == 1);
	return VRL_OK;
}

static vrl_status fn_is_ipv6(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	const char *s; struct in6_addr x;
	*out = vrl_boolean(str_of(vrl_arg(a, "value", 0), &s) && inet_pton(AF_INET6, s, &x) == 1);
	return VRL_OK;
}

static vrl_status fn_ip_aton(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	struct in_addr x;
	if (inet_pton(AF_INET, s, &x) != 1) { *err = vrl_errf("ip_aton: invalid IPv4 '%s'", s); return VRL_ERR; }
	*out = vrl_integer((int64_t)ntohl(x.s_addr));
	return VRL_OK;
}

static vrl_status fn_ip_ntoa(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_INTEGER) { *err = vrl_errf("ip_ntoa: expected integer"); return VRL_ERR; }
	struct in_addr x; x.s_addr = htonl((uint32_t)v->u.integer);
	char buf[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &x, buf, sizeof(buf));
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

static vrl_status fn_ip_pton(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	unsigned char buf[16];
	if (inet_pton(AF_INET, s, buf) == 1) { *out = vrl_bytes((char *)buf, 4); return VRL_OK; }
	if (inet_pton(AF_INET6, s, buf) == 1) { *out = vrl_bytes((char *)buf, 16); return VRL_OK; }
	*err = vrl_errf("ip_pton: invalid IP '%s'", s);
	return VRL_ERR;
}

static vrl_status fn_ip_ntop(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_BYTES) { *err = vrl_errf("ip_ntop: expected bytes"); return VRL_ERR; }
	char buf[INET6_ADDRSTRLEN];
	if (v->u.bytes.len == 4) {
		inet_ntop(AF_INET, v->u.bytes.data, buf, sizeof(buf));
		*out = vrl_bytes_cstr(buf); return VRL_OK;
	}
	if (v->u.bytes.len == 16) {
		inet_ntop(AF_INET6, v->u.bytes.data, buf, sizeof(buf));
		*out = vrl_bytes_cstr(buf); return VRL_OK;
	}
	*err = vrl_errf("ip_ntop: expected 4 or 16 bytes");
	return VRL_ERR;
}

static vrl_status fn_ip_to_ipv6(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	struct in_addr x;
	if (inet_pton(AF_INET, s, &x) != 1) { *err = vrl_errf("ip_to_ipv6: invalid IPv4 '%s'", s); return VRL_ERR; }
	unsigned char v6[16] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff,0,0,0,0};
	memcpy(v6 + 12, &x.s_addr, 4);
	char buf[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, v6, buf, sizeof(buf));
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

static vrl_status fn_ipv6_to_ipv4(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	unsigned char v6[16];
	if (inet_pton(AF_INET6, s, v6) != 1) { *err = vrl_errf("ipv6_to_ipv4: invalid IPv6 '%s'", s); return VRL_ERR; }
	static const unsigned char prefix[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
	if (memcmp(v6, prefix, 12) != 0) { *err = vrl_errf("ipv6_to_ipv4: not an IPv4-mapped address"); return VRL_ERR; }
	struct in_addr x; memcpy(&x.s_addr, v6 + 12, 4);
	char buf[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &x, buf, sizeof(buf));
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

/* Parse an IP into a family + 16-byte buffer (v4 stored in first 4). */
static int parse_ip(const char *s, int *fam, unsigned char *buf)
{
	if (inet_pton(AF_INET, s, buf) == 1) { *fam = AF_INET; return 1; }
	if (inet_pton(AF_INET6, s, buf) == 1) { *fam = AF_INET6; return 1; }
	return 0;
}

static void apply_mask(unsigned char *buf, int nbytes, int prefix)
{
	for (int i = 0; i < nbytes; i++) {
		int bits = prefix - i * 8;
		if (bits >= 8) continue;
		if (bits <= 0) { buf[i] = 0; continue; }
		buf[i] &= (unsigned char)(0xFF << (8 - bits));
	}
}

static vrl_status fn_ip_cidr_contains(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *cidr; size_t cl; const char *ip; size_t il;
	if (!avrl_arg_str(a, "cidr", 0, &cidr, &cl, err)) return VRL_ERR;
	if (!avrl_arg_str(a, "value", 1, &ip, &il, err)) return VRL_ERR;
	char cbuf[128];
	if (cl >= sizeof(cbuf)) { *err = vrl_errf("ip_cidr_contains: cidr too long"); return VRL_ERR; }
	memcpy(cbuf, cidr, cl); cbuf[cl] = '\0';
	char *slash = strchr(cbuf, '/');
	int prefix = -1;
	if (slash) { *slash = '\0'; prefix = atoi(slash + 1); }
	int fam1, fam2; unsigned char net[16] = {0}, addr[16] = {0};
	if (!parse_ip(cbuf, &fam1, net)) { *err = vrl_errf("ip_cidr_contains: invalid cidr address"); return VRL_ERR; }
	if (!parse_ip(ip, &fam2, addr)) { *err = vrl_errf("ip_cidr_contains: invalid IP '%s'", ip); return VRL_ERR; }
	if (fam1 != fam2) { *out = vrl_boolean(0); return VRL_OK; }
	int nbytes = (fam1 == AF_INET) ? 4 : 16;
	if (prefix < 0) prefix = nbytes * 8;
	apply_mask(net, nbytes, prefix);
	apply_mask(addr, nbytes, prefix);
	*out = vrl_boolean(memcmp(net, addr, nbytes) == 0);
	return VRL_OK;
}

static vrl_status fn_ip_subnet(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *ip; size_t il; const char *sub; size_t sl;
	if (!avrl_arg_str(a, "value", 0, &ip, &il, err)) return VRL_ERR;
	if (!avrl_arg_str(a, "subnet", 1, &sub, &sl, err)) return VRL_ERR;
	int fam; unsigned char addr[16] = {0};
	if (!parse_ip(ip, &fam, addr)) { *err = vrl_errf("ip_subnet: invalid IP '%s'", ip); return VRL_ERR; }
	int nbytes = (fam == AF_INET) ? 4 : 16;
	int prefix;
	if (sub[0] == '/') {
		prefix = atoi(sub + 1);
	} else {
		/* netmask form, e.g. 255.255.255.0 */
		int mf; unsigned char mask[16] = {0};
		if (!parse_ip(sub, &mf, mask) || mf != fam) { *err = vrl_errf("ip_subnet: invalid subnet mask '%s'", sub); return VRL_ERR; }
		prefix = 0;
		for (int i = 0; i < nbytes; i++)
			for (int b = 7; b >= 0; b--)
				if (mask[i] & (1 << b)) prefix++;
	}
	apply_mask(addr, nbytes, prefix);
	char buf[INET6_ADDRSTRLEN];
	inet_ntop(fam, addr, buf, sizeof(buf));
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

void vrl_reg_number(void)
{
	vrl_register("format_int", fn_format_int);
	vrl_register("format_number", fn_format_number);
	vrl_register("from_unix_timestamp", fn_from_unix_timestamp);
	vrl_register("to_unix_timestamp", fn_to_unix_timestamp);
	vrl_register("to_syslog_level", fn_to_syslog_level);
	vrl_register("to_syslog_severity", fn_to_syslog_severity);
	vrl_register("to_syslog_facility", fn_to_syslog_facility);
	vrl_register("to_syslog_facility_code", fn_to_syslog_facility_code);
	vrl_register("get_env_var", fn_get_env_var);
	vrl_register("get_hostname", fn_get_hostname);
	vrl_register("get_timezone_name", fn_get_timezone_name);
	vrl_register("haversine", fn_haversine);
	vrl_register("crc", fn_crc);
	vrl_register("xxhash", fn_xxhash);
	vrl_register("seahash", fn_seahash);
	vrl_register("is_ipv4", fn_is_ipv4);
	vrl_register("is_ipv6", fn_is_ipv6);
	vrl_register("ip_aton", fn_ip_aton);
	vrl_register("ip_ntoa", fn_ip_ntoa);
	vrl_register("ip_pton", fn_ip_pton);
	vrl_register("ip_ntop", fn_ip_ntop);
	vrl_register("ip_to_ipv6", fn_ip_to_ipv6);
	vrl_register("ipv6_to_ipv4", fn_ipv6_to_ipv4);
	vrl_register("ip_cidr_contains", fn_ip_cidr_contains);
	vrl_register("ip_subnet", fn_ip_subnet);
}
