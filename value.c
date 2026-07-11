#define _GNU_SOURCE
#include "value.h"
#include "pcre_wrap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

static vrl_value *vrl_alloc(vrl_value_type t)
{
	vrl_value *v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->type = t;
	v->refcount = 1;
	return v;
}

vrl_value *vrl_value_ref(vrl_value *v)
{
	if (v)
		v->refcount++;
	return v;
}

static void vrl_value_destroy(vrl_value *v)
{
	switch (v->type) {
	case VRL_BYTES:
		free(v->u.bytes.data);
		break;
	case VRL_REGEX:
		if (v->u.regex)
			avrl_regex_free(v->u.regex);
		break;
	case VRL_ARRAY:
		for (size_t i = 0; i < v->u.array.len; i++)
			vrl_value_unref(v->u.array.items[i]);
		free(v->u.array.items);
		break;
	case VRL_OBJECT:
		for (size_t i = 0; i < v->u.object.len; i++) {
			free(v->u.object.entries[i].key);
			vrl_value_unref(v->u.object.entries[i].val);
		}
		free(v->u.object.entries);
		break;
	default:
		break;
	}
	free(v);
}

void vrl_value_unref(vrl_value *v)
{
	if (!v)
		return;
	if (--v->refcount > 0)
		return;
	vrl_value_destroy(v);
}

/* ------------------------------------------------------------------ */
/* constructors                                                        */
/* ------------------------------------------------------------------ */

vrl_value *vrl_null(void)
{
	return vrl_alloc(VRL_NULL);
}

vrl_value *vrl_boolean(int b)
{
	vrl_value *v = vrl_alloc(VRL_BOOLEAN);
	if (v)
		v->u.boolean = b ? 1 : 0;
	return v;
}

vrl_value *vrl_integer(int64_t i)
{
	vrl_value *v = vrl_alloc(VRL_INTEGER);
	if (v)
		v->u.integer = i;
	return v;
}

vrl_value *vrl_float(double f)
{
	vrl_value *v = vrl_alloc(VRL_FLOAT);
	if (v)
		v->u.flt = f;
	return v;
}

vrl_value *vrl_timestamp(double unix_seconds)
{
	vrl_value *v = vrl_alloc(VRL_TIMESTAMP);
	if (v)
		v->u.timestamp = unix_seconds;
	return v;
}

vrl_value *vrl_bytes(const char *data, size_t len)
{
	vrl_value *v = vrl_alloc(VRL_BYTES);
	if (!v)
		return NULL;
	v->u.bytes.data = malloc(len + 1);
	if (!v->u.bytes.data) {
		free(v);
		return NULL;
	}
	if (len && data)
		memcpy(v->u.bytes.data, data, len);
	v->u.bytes.data[len] = '\0';
	v->u.bytes.len = len;
	return v;
}

vrl_value *vrl_bytes_cstr(const char *cstr)
{
	return vrl_bytes(cstr, cstr ? strlen(cstr) : 0);
}

vrl_value *vrl_bytes_take(char *data, size_t len)
{
	vrl_value *v = vrl_alloc(VRL_BYTES);
	if (!v) {
		free(data);
		return NULL;
	}
	v->u.bytes.data = data;
	v->u.bytes.len = len;
	return v;
}

vrl_value *vrl_regex_take(regex_match *re)
{
	vrl_value *v = vrl_alloc(VRL_REGEX);
	if (!v) {
		if (re)
			avrl_regex_free(re);
		return NULL;
	}
	v->u.regex = re;
	return v;
}

vrl_value *vrl_array_new(void)
{
	return vrl_alloc(VRL_ARRAY);
}

vrl_value *vrl_object_new(void)
{
	return vrl_alloc(VRL_OBJECT);
}

/* ------------------------------------------------------------------ */
/* array                                                               */
/* ------------------------------------------------------------------ */

static int vrl_array_reserve(vrl_value *arr, size_t need)
{
	if (need <= arr->u.array.cap)
		return 0;
	size_t cap = arr->u.array.cap ? arr->u.array.cap * 2 : 8;
	while (cap < need)
		cap *= 2;
	vrl_value **items = realloc(arr->u.array.items, cap * sizeof(*items));
	if (!items)
		return -1;
	arr->u.array.items = items;
	arr->u.array.cap = cap;
	return 0;
}

void vrl_array_push(vrl_value *arr, vrl_value *item)
{
	if (!arr || arr->type != VRL_ARRAY) {
		vrl_value_unref(item);
		return;
	}
	if (vrl_array_reserve(arr, arr->u.array.len + 1) != 0) {
		vrl_value_unref(item);
		return;
	}
	arr->u.array.items[arr->u.array.len++] = item;
}

vrl_value *vrl_array_get(vrl_value *arr, size_t idx)
{
	if (!arr || arr->type != VRL_ARRAY || idx >= arr->u.array.len)
		return NULL;
	return arr->u.array.items[idx];
}

int vrl_array_set(vrl_value *arr, size_t idx, vrl_value *item)
{
	if (!arr || arr->type != VRL_ARRAY) {
		vrl_value_unref(item);
		return -1;
	}
	/* grow with nulls to reach idx (VRL allows sparse assignment via index) */
	if (idx >= arr->u.array.len) {
		if (vrl_array_reserve(arr, idx + 1) != 0) {
			vrl_value_unref(item);
			return -1;
		}
		while (arr->u.array.len <= idx)
			arr->u.array.items[arr->u.array.len++] = vrl_null();
	}
	vrl_value_unref(arr->u.array.items[idx]);
	arr->u.array.items[idx] = item;
	return 0;
}

size_t vrl_array_len(const vrl_value *arr)
{
	if (!arr || arr->type != VRL_ARRAY)
		return 0;
	return arr->u.array.len;
}

/* ------------------------------------------------------------------ */
/* object                                                              */
/* ------------------------------------------------------------------ */

static vrl_object_entry *vrl_object_find(vrl_value *obj, const char *key, size_t key_len)
{
	for (size_t i = 0; i < obj->u.object.len; i++) {
		vrl_object_entry *e = &obj->u.object.entries[i];
		if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
			return e;
	}
	return NULL;
}

int vrl_object_set(vrl_value *obj, const char *key, size_t key_len, vrl_value *val)
{
	if (!obj || obj->type != VRL_OBJECT) {
		vrl_value_unref(val);
		return -1;
	}
	vrl_object_entry *e = vrl_object_find(obj, key, key_len);
	if (e) {
		vrl_value_unref(e->val);
		e->val = val;
		return 0;
	}
	if (obj->u.object.len >= obj->u.object.cap) {
		size_t cap = obj->u.object.cap ? obj->u.object.cap * 2 : 8;
		vrl_object_entry *ne = realloc(obj->u.object.entries, cap * sizeof(*ne));
		if (!ne) {
			vrl_value_unref(val);
			return -1;
		}
		obj->u.object.entries = ne;
		obj->u.object.cap = cap;
	}
	e = &obj->u.object.entries[obj->u.object.len++];
	e->key = malloc(key_len + 1);
	if (!e->key) {
		obj->u.object.len--;
		vrl_value_unref(val);
		return -1;
	}
	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->val = val;
	return 0;
}

int vrl_object_set_cstr(vrl_value *obj, const char *key, vrl_value *val)
{
	return vrl_object_set(obj, key, strlen(key), val);
}

vrl_value *vrl_object_get(vrl_value *obj, const char *key, size_t key_len)
{
	if (!obj || obj->type != VRL_OBJECT)
		return NULL;
	vrl_object_entry *e = vrl_object_find(obj, key, key_len);
	return e ? e->val : NULL;
}

int vrl_object_del(vrl_value *obj, const char *key, size_t key_len)
{
	if (!obj || obj->type != VRL_OBJECT)
		return -1;
	for (size_t i = 0; i < obj->u.object.len; i++) {
		vrl_object_entry *e = &obj->u.object.entries[i];
		if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
			free(e->key);
			vrl_value_unref(e->val);
			memmove(&obj->u.object.entries[i], &obj->u.object.entries[i + 1],
				(obj->u.object.len - i - 1) * sizeof(*e));
			obj->u.object.len--;
			return 0;
		}
	}
	return -1;
}

size_t vrl_object_len(const vrl_value *obj)
{
	if (!obj || obj->type != VRL_OBJECT)
		return 0;
	return obj->u.object.len;
}

/* ------------------------------------------------------------------ */
/* clone                                                               */
/* ------------------------------------------------------------------ */

vrl_value *vrl_value_clone(const vrl_value *v)
{
	if (!v)
		return NULL;
	switch (v->type) {
	case VRL_NULL:
		return vrl_null();
	case VRL_BOOLEAN:
		return vrl_boolean(v->u.boolean);
	case VRL_INTEGER:
		return vrl_integer(v->u.integer);
	case VRL_FLOAT:
		return vrl_float(v->u.flt);
	case VRL_TIMESTAMP:
		return vrl_timestamp(v->u.timestamp);
	case VRL_BYTES:
		return vrl_bytes(v->u.bytes.data, v->u.bytes.len);
	case VRL_REGEX:
		/* regex is immutable at runtime; share by recompiling pattern */
		if (v->u.regex && v->u.regex->pattern)
			return vrl_regex_take(avrl_regex_compile(v->u.regex->pattern));
		return vrl_null();
	case VRL_ARRAY: {
		vrl_value *out = vrl_array_new();
		for (size_t i = 0; i < v->u.array.len; i++)
			vrl_array_push(out, vrl_value_clone(v->u.array.items[i]));
		return out;
	}
	case VRL_OBJECT: {
		vrl_value *out = vrl_object_new();
		for (size_t i = 0; i < v->u.object.len; i++) {
			vrl_object_entry *e = &v->u.object.entries[i];
			vrl_object_set(out, e->key, e->key_len, vrl_value_clone(e->val));
		}
		return out;
	}
	}
	return vrl_null();
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

const char *vrl_type_name(vrl_value_type t)
{
	switch (t) {
	case VRL_NULL:      return "null";
	case VRL_BOOLEAN:   return "boolean";
	case VRL_INTEGER:   return "integer";
	case VRL_FLOAT:     return "float";
	case VRL_BYTES:     return "string";
	case VRL_TIMESTAMP: return "timestamp";
	case VRL_REGEX:     return "regex";
	case VRL_ARRAY:     return "array";
	case VRL_OBJECT:    return "object";
	}
	return "unknown";
}

int vrl_value_equal(const vrl_value *a, const vrl_value *b)
{
	if (a == b)
		return 1;
	if (!a || !b || a->type != b->type)
		return 0;
	switch (a->type) {
	case VRL_NULL:      return 1;
	case VRL_BOOLEAN:   return a->u.boolean == b->u.boolean;
	case VRL_INTEGER:   return a->u.integer == b->u.integer;
	case VRL_FLOAT:     return a->u.flt == b->u.flt;
	case VRL_TIMESTAMP: return a->u.timestamp == b->u.timestamp;
	case VRL_BYTES:
		return a->u.bytes.len == b->u.bytes.len &&
		       memcmp(a->u.bytes.data, b->u.bytes.data, a->u.bytes.len) == 0;
	case VRL_REGEX:
		return a->u.regex && b->u.regex && a->u.regex->pattern &&
		       b->u.regex->pattern &&
		       strcmp(a->u.regex->pattern, b->u.regex->pattern) == 0;
	case VRL_ARRAY:
		if (a->u.array.len != b->u.array.len)
			return 0;
		for (size_t i = 0; i < a->u.array.len; i++)
			if (!vrl_value_equal(a->u.array.items[i], b->u.array.items[i]))
				return 0;
		return 1;
	case VRL_OBJECT:
		if (a->u.object.len != b->u.object.len)
			return 0;
		for (size_t i = 0; i < a->u.object.len; i++) {
			vrl_object_entry *e = &a->u.object.entries[i];
			vrl_value *bv = vrl_object_get((vrl_value *)b, e->key, e->key_len);
			if (!bv || !vrl_value_equal(e->val, bv))
				return 0;
		}
		return 1;
	}
	return 0;
}

int vrl_value_truthy(const vrl_value *v)
{
	if (!v)
		return 0;
	switch (v->type) {
	case VRL_NULL:      return 0;
	case VRL_BOOLEAN:   return v->u.boolean;
	case VRL_INTEGER:   return v->u.integer != 0;
	case VRL_FLOAT:     return v->u.flt != 0.0;
	case VRL_BYTES:     return v->u.bytes.len != 0;
	case VRL_ARRAY:     return v->u.array.len != 0;
	case VRL_OBJECT:    return v->u.object.len != 0;
	default:            return 1;
	}
}

/* small growable buffer for to_string */
typedef struct {
	char *s;
	size_t len;
	size_t cap;
} sbuf;

static void sbuf_add(sbuf *b, const char *data, size_t len)
{
	if (b->len + len + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 64;
		while (cap < b->len + len + 1)
			cap *= 2;
		char *ns = realloc(b->s, cap);
		if (!ns)
			return;
		b->s = ns;
		b->cap = cap;
	}
	memcpy(b->s + b->len, data, len);
	b->len += len;
	b->s[b->len] = '\0';
}

static void sbuf_cstr(sbuf *b, const char *s)
{
	sbuf_add(b, s, strlen(s));
}

static void vrl_format_double(sbuf *b, double d)
{
	char tmp[64];
	if (isnan(d))
		sbuf_cstr(b, "NaN");
	else if (isinf(d))
		sbuf_cstr(b, d < 0 ? "-inf" : "inf");
	else {
		snprintf(tmp, sizeof(tmp), "%g", d);
		sbuf_cstr(b, tmp);
	}
}

static void vrl_stringify(sbuf *b, const vrl_value *v, int quote_strings)
{
	char tmp[64];
	if (!v) {
		sbuf_cstr(b, "null");
		return;
	}
	switch (v->type) {
	case VRL_NULL:
		sbuf_cstr(b, "null");
		break;
	case VRL_BOOLEAN:
		sbuf_cstr(b, v->u.boolean ? "true" : "false");
		break;
	case VRL_INTEGER:
		snprintf(tmp, sizeof(tmp), "%lld", (long long)v->u.integer);
		sbuf_cstr(b, tmp);
		break;
	case VRL_FLOAT:
		vrl_format_double(b, v->u.flt);
		break;
	case VRL_TIMESTAMP: {
		time_t secs = (time_t)v->u.timestamp;
		struct tm tmv;
		gmtime_r(&secs, &tmv);
		char ts[40];
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
		if (quote_strings)
			sbuf_cstr(b, "\"");
		sbuf_cstr(b, ts);
		if (quote_strings)
			sbuf_cstr(b, "\"");
		break;
	}
	case VRL_BYTES:
		if (quote_strings) {
			sbuf_cstr(b, "\"");
			for (size_t i = 0; i < v->u.bytes.len; i++) {
				char c = v->u.bytes.data[i];
				if (c == '"' || c == '\\') {
					sbuf_add(b, "\\", 1);
					sbuf_add(b, &c, 1);
				} else if (c == '\n') {
					sbuf_cstr(b, "\\n");
				} else if (c == '\t') {
					sbuf_cstr(b, "\\t");
				} else {
					sbuf_add(b, &c, 1);
				}
			}
			sbuf_cstr(b, "\"");
		} else {
			sbuf_add(b, v->u.bytes.data, v->u.bytes.len);
		}
		break;
	case VRL_REGEX:
		sbuf_cstr(b, "r'");
		if (v->u.regex && v->u.regex->pattern)
			sbuf_cstr(b, v->u.regex->pattern);
		sbuf_cstr(b, "'");
		break;
	case VRL_ARRAY:
		sbuf_cstr(b, "[");
		for (size_t i = 0; i < v->u.array.len; i++) {
			if (i)
				sbuf_cstr(b, ",");
			vrl_stringify(b, v->u.array.items[i], 1);
		}
		sbuf_cstr(b, "]");
		break;
	case VRL_OBJECT:
		sbuf_cstr(b, "{");
		for (size_t i = 0; i < v->u.object.len; i++) {
			if (i)
				sbuf_cstr(b, ",");
			vrl_object_entry *e = &v->u.object.entries[i];
			sbuf_cstr(b, "\"");
			sbuf_add(b, e->key, e->key_len);
			sbuf_cstr(b, "\":");
			vrl_stringify(b, e->val, 1);
		}
		sbuf_cstr(b, "}");
		break;
	}
}

char *vrl_value_to_string(const vrl_value *v, size_t *out_len)
{
	sbuf b = {0};
	/* top-level: strings/timestamps unquoted (to_string semantics);
	 * nested containers get JSON-ish quoting for readability. */
	int quote = (v && (v->type == VRL_ARRAY || v->type == VRL_OBJECT));
	(void)quote;
	vrl_stringify(&b, v, 0);
	if (!b.s) {
		b.s = malloc(1);
		if (b.s)
			b.s[0] = '\0';
		b.len = 0;
	}
	if (out_len)
		*out_len = b.len;
	return b.s;
}
