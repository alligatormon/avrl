#define _GNU_SOURCE
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

/* ================================================================== */
/* rng: xorshift128+ lazily seeded from /dev/urandom (or time)        */
/* ================================================================== */

static uint64_t RNG_S[2];
static int RNG_seeded = 0;

static void rng_seed(void)
{
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		size_t got = fread(RNG_S, 1, sizeof(RNG_S), f);
		fclose(f);
		if (got == sizeof(RNG_S) && (RNG_S[0] || RNG_S[1])) { RNG_seeded = 1; return; }
	}
	struct timeval tv;
	gettimeofday(&tv, NULL);
	RNG_S[0] = (uint64_t)tv.tv_sec * 1000000007ULL + (uint64_t)tv.tv_usec;
	RNG_S[1] = (uint64_t)getpid() * 2654435761ULL + 0x9e3779b97f4a7c15ULL;
	if (!RNG_S[0] && !RNG_S[1]) RNG_S[0] = 0x1234567890abcdefULL;
	RNG_seeded = 1;
}

static uint64_t rng_next(void)
{
	if (!RNG_seeded) rng_seed();
	uint64_t x = RNG_S[0], y = RNG_S[1];
	RNG_S[0] = y;
	x ^= x << 23;
	RNG_S[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
	return RNG_S[1] + y;
}

static void rng_fill(unsigned char *buf, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint64_t r = rng_next();
		for (int k = 0; k < 8 && i < n; k++, i++)
			buf[i] = (unsigned char)(r >> (8 * k));
	}
}

/* ================================================================== */
/* random_* functions                                                 */
/* ================================================================== */

static vrl_status fn_random_bool(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)a; (void)err;
	*out = vrl_boolean((int)(rng_next() & 1));
	return VRL_OK;
}

static vrl_status fn_random_int(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *lo = vrl_arg(a, "min", 0);
	vrl_value *hi = vrl_arg(a, "max", 1);
	if (!lo || lo->type != VRL_INTEGER || !hi || hi->type != VRL_INTEGER) {
		*err = vrl_errf("random_int: min and max must be integers"); return VRL_ERR;
	}
	int64_t min = lo->u.integer, max = hi->u.integer;
	if (max <= min) { *err = vrl_errf("random_int: max must be greater than min"); return VRL_ERR; }
	uint64_t span = (uint64_t)(max - min);
	*out = vrl_integer(min + (int64_t)(rng_next() % span));
	return VRL_OK;
}

static vrl_status fn_random_float(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *lo = vrl_arg(a, "min", 0);
	vrl_value *hi = vrl_arg(a, "max", 1);
	double min = 0.0, max = 1.0;
	if (lo && lo->type == VRL_FLOAT) min = lo->u.flt; else if (lo && lo->type == VRL_INTEGER) min = (double)lo->u.integer;
	if (hi && hi->type == VRL_FLOAT) max = hi->u.flt; else if (hi && hi->type == VRL_INTEGER) max = (double)hi->u.integer;
	if (max <= min) { *err = vrl_errf("random_float: max must be greater than min"); return VRL_ERR; }
	double frac = (double)(rng_next() >> 11) / (double)(1ULL << 53);
	*out = vrl_float(min + frac * (max - min));
	return VRL_OK;
}

static vrl_status fn_random_bytes(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *lv = vrl_arg(a, "length", 0);
	if (!lv || lv->type != VRL_INTEGER || lv->u.integer < 0) {
		*err = vrl_errf("random_bytes: length must be a non-negative integer"); return VRL_ERR;
	}
	if (lv->u.integer > 65536) { *err = vrl_errf("random_bytes: length exceeds 64KiB"); return VRL_ERR; }
	size_t n = (size_t)lv->u.integer;
	char *buf = malloc(n + 1);
	rng_fill((unsigned char *)buf, n);
	buf[n] = '\0';
	*out = vrl_bytes_take(buf, n);
	return VRL_OK;
}

/* ================================================================== */
/* uuid v4 / v7                                                       */
/* ================================================================== */

static void format_uuid(const unsigned char b[16], char out[37])
{
	static const char *hex = "0123456789abcdef";
	int p = 0;
	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
		out[p++] = hex[b[i] >> 4];
		out[p++] = hex[b[i] & 0xf];
	}
	out[p] = '\0';
}

static vrl_status fn_uuid_v4(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)a; (void)err;
	unsigned char b[16];
	rng_fill(b, 16);
	b[6] = (b[6] & 0x0f) | 0x40; /* version 4 */
	b[8] = (b[8] & 0x3f) | 0x80; /* variant */
	char buf[37];
	format_uuid(b, buf);
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

static vrl_status fn_uuid_v7(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)a; (void)err;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64_t ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
	unsigned char b[16];
	b[0] = (unsigned char)(ms >> 40);
	b[1] = (unsigned char)(ms >> 32);
	b[2] = (unsigned char)(ms >> 24);
	b[3] = (unsigned char)(ms >> 16);
	b[4] = (unsigned char)(ms >> 8);
	b[5] = (unsigned char)(ms);
	rng_fill(b + 6, 10);
	b[6] = (b[6] & 0x0f) | 0x70; /* version 7 */
	b[8] = (b[8] & 0x3f) | 0x80; /* variant */
	char buf[37];
	format_uuid(b, buf);
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

/* ================================================================== */
/* uuid_from_friendly_id (base62 -> uuid)                             */
/* ================================================================== */

static int base62_digit(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
	if (c >= 'a' && c <= 'z') return c - 'a' + 36;
	return -1;
}

static vrl_status fn_uuid_from_friendly_id(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s; size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err)) return VRL_ERR;
	unsigned char b[16] = {0};
	for (size_t i = 0; i < n; i++) {
		int d = base62_digit((unsigned char)s[i]);
		if (d < 0) { *err = vrl_errf("uuid_from_friendly_id: invalid base62 char"); return VRL_ERR; }
		/* b = b * 62 + d (big-endian 128-bit) */
		unsigned carry = (unsigned)d;
		for (int j = 15; j >= 0; j--) {
			unsigned v = (unsigned)b[j] * 62u + carry;
			b[j] = (unsigned char)(v & 0xff);
			carry = v >> 8;
		}
		if (carry) { *err = vrl_errf("uuid_from_friendly_id: value overflows 128 bits"); return VRL_ERR; }
	}
	char buf[37];
	format_uuid(b, buf);
	*out = vrl_bytes_cstr(buf);
	return VRL_OK;
}

void vrl_reg_random(void)
{
	vrl_register("random_bool", fn_random_bool);
	vrl_register("random_int", fn_random_int);
	vrl_register("random_float", fn_random_float);
	vrl_register("random_bytes", fn_random_bytes);
	vrl_register("uuid_v4", fn_uuid_v4);
	vrl_register("uuid_v7", fn_uuid_v7);
	vrl_register("uuid_from_friendly_id", fn_uuid_from_friendly_id);
}
