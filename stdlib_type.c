#define _GNU_SOURCE
#include "stdlib_internal.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ================================================================== */
/* type assertions: array/bool/float/int/object/string/timestamp      */
/* ================================================================== */

#define ASSERT_FN(fname, TYPE, tname) \
	static vrl_status fname(vrl_call_args *a, vrl_value **out, char **err) { \
		vrl_value *v = vrl_arg(a, "value", 0); \
		if (!v || v->type != TYPE) { \
			*err = vrl_errf("expected " tname ", got %s", \
					v ? vrl_type_name(v->type) : "null"); \
			return VRL_ERR; \
		} \
		*out = vrl_value_ref(v); return VRL_OK; }

ASSERT_FN(fn_array, VRL_ARRAY, "array")
ASSERT_FN(fn_bool, VRL_BOOLEAN, "boolean")
ASSERT_FN(fn_float, VRL_FLOAT, "float")
ASSERT_FN(fn_int, VRL_INTEGER, "integer")
ASSERT_FN(fn_object, VRL_OBJECT, "object")
ASSERT_FN(fn_string, VRL_BYTES, "string")
ASSERT_FN(fn_timestamp, VRL_TIMESTAMP, "timestamp")

/* ================================================================== */
/* predicates                                                         */
/* ================================================================== */

static vrl_status fn_is_empty(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v) { *err = vrl_errf("is_empty: missing argument"); return VRL_ERR; }
	int e;
	switch (v->type) {
	case VRL_BYTES:  e = v->u.bytes.len == 0; break;
	case VRL_ARRAY:  e = v->u.array.len == 0; break;
	case VRL_OBJECT: e = v->u.object.len == 0; break;
	default:
		*err = vrl_errf("is_empty: expected string/array/object, got %s",
				vrl_type_name(v->type));
		return VRL_ERR;
	}
	*out = vrl_boolean(e);
	return VRL_OK;
}

static vrl_status fn_is_nullish(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	vrl_value *v = vrl_arg(a, "value", 0);
	int nullish = 0;
	if (!v || v->type == VRL_NULL) {
		nullish = 1;
	} else if (v->type == VRL_BYTES) {
		const char *s = v->u.bytes.data;
		size_t n = v->u.bytes.len;
		size_t b = 0, e = n;
		while (b < e && isspace((unsigned char)s[b])) b++;
		while (e > b && isspace((unsigned char)s[e - 1])) e--;
		size_t tl = e - b;
		if (tl == 0)
			nullish = 1;
		else if (tl == 1 && s[b] == '-')
			nullish = 1;
	}
	*out = vrl_boolean(nullish);
	return VRL_OK;
}

static vrl_status fn_is_json(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_BYTES) { *out = vrl_boolean(0); return VRL_OK; }
	char *jerr = NULL;
	vrl_value *parsed = vrl_json_decode(v->u.bytes.data, v->u.bytes.len, &jerr);
	free(jerr);
	if (!parsed) { *out = vrl_boolean(0); return VRL_OK; }
	/* optional variant: restrict to a specific json top type */
	vrl_value *variant = vrl_arg(a, "variant", 1);
	int ok = 1;
	if (variant && variant->type == VRL_BYTES) {
		const char *want = variant->u.bytes.data;
		vrl_value_type t = parsed->type;
		if (!strcmp(want, "object")) ok = (t == VRL_OBJECT);
		else if (!strcmp(want, "array")) ok = (t == VRL_ARRAY);
		else if (!strcmp(want, "string")) ok = (t == VRL_BYTES);
		else if (!strcmp(want, "bool")) ok = (t == VRL_BOOLEAN);
		else if (!strcmp(want, "number")) ok = (t == VRL_INTEGER || t == VRL_FLOAT);
		else if (!strcmp(want, "null")) ok = (t == VRL_NULL);
	}
	vrl_value_unref(parsed);
	*out = vrl_boolean(ok);
	return VRL_OK;
}

/* ================================================================== */
/* coerce: to_regex                                                   */
/* ================================================================== */

static vrl_status fn_to_regex(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (v && v->type == VRL_REGEX) { *out = vrl_value_ref(v); return VRL_OK; }
	if (!v || v->type != VRL_BYTES) {
		*err = vrl_errf("to_regex: expected string pattern");
		return VRL_ERR;
	}
	regex_match *re = avrl_regex_compile(v->u.bytes.data);
	if (!re) { *err = vrl_errf("to_regex: invalid pattern"); return VRL_ERR; }
	*out = vrl_regex_take(re);
	return VRL_OK;
}

/* ================================================================== */
/* number: mod (function form)                                        */
/* ================================================================== */

static int num_double(const vrl_value *v, double *d)
{
	if (!v) return 0;
	if (v->type == VRL_INTEGER) { *d = (double)v->u.integer; return 1; }
	if (v->type == VRL_FLOAT) { *d = v->u.flt; return 1; }
	return 0;
}

static vrl_status fn_mod(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *x = vrl_arg(a, "value", 0);
	vrl_value *m = vrl_arg(a, "modulus", 1);
	double xd, md;
	if (!num_double(x, &xd) || !num_double(m, &md)) {
		*err = vrl_errf("mod: expected numbers");
		return VRL_ERR;
	}
	if (md == 0.0) { *err = vrl_errf("mod: modulo by zero"); return VRL_ERR; }
	if (x->type == VRL_INTEGER && m->type == VRL_INTEGER)
		*out = vrl_integer(x->u.integer % m->u.integer);
	else
		*out = vrl_float(fmod(xd, md));
	return VRL_OK;
}

/* ================================================================== */
/* debug: assert / assert_eq                                          */
/* ================================================================== */

static char *assert_msg(vrl_call_args *a, int idx, const char *dflt)
{
	vrl_value *m = vrl_arg(a, "message", idx);
	if (m && m->type == VRL_BYTES)
		return strdup(m->u.bytes.data);
	return strdup(dflt);
}

static vrl_status fn_assert(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *cond = vrl_arg(a, "condition", 0);
	if (!cond || cond->type != VRL_BOOLEAN) {
		*err = vrl_errf("assert: condition must be boolean");
		return VRL_ERR;
	}
	if (!cond->u.boolean) {
		*err = assert_msg(a, 1, "assertion failed");
		return VRL_ERR;
	}
	*out = vrl_boolean(1);
	return VRL_OK;
}

static vrl_status fn_assert_eq(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *l = vrl_arg(a, "left", 0);
	vrl_value *r = vrl_arg(a, "right", 1);
	if (!vrl_value_equal(l, r)) {
		*err = assert_msg(a, 2, "assertion failed: values are not equal");
		return VRL_ERR;
	}
	*out = vrl_boolean(1);
	return VRL_OK;
}

/* ================================================================== */
/* tag_types_externally / type_def                                    */
/* ================================================================== */

static vrl_value *tag_types(const vrl_value *v)
{
	if (!v)
		return vrl_null();
	switch (v->type) {
	case VRL_NULL:
		return vrl_null();
	case VRL_ARRAY: {
		vrl_value *arr = vrl_array_new();
		for (size_t i = 0; i < v->u.array.len; i++)
			vrl_array_push(arr, tag_types(v->u.array.items[i]));
		return arr;
	}
	case VRL_OBJECT: {
		vrl_value *obj = vrl_object_new();
		for (size_t i = 0; i < v->u.object.len; i++) {
			vrl_object_entry *e = &v->u.object.entries[i];
			vrl_object_set(obj, e->key, e->key_len, tag_types(e->val));
		}
		return obj;
	}
	default: {
		vrl_value *obj = vrl_object_new();
		vrl_object_set_cstr(obj, vrl_type_name(v->type), vrl_value_clone(v));
		return obj;
	}
	}
}

static vrl_status fn_tag_types_externally(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	vrl_value *v = vrl_arg(a, "value", 0);
	*out = tag_types(v);
	return VRL_OK;
}

/* Runtime approximation: real VRL type_def is a compile-time type. */
static vrl_status fn_type_def(vrl_call_args *a, vrl_value **out, char **err)
{
	(void)err;
	vrl_value *v = vrl_arg(a, "value", 0);
	vrl_value *obj = vrl_object_new();
	vrl_object_set_cstr(obj, "type", vrl_bytes_cstr(v ? vrl_type_name(v->type) : "null"));
	*out = obj;
	return VRL_OK;
}

void vrl_reg_type(void)
{
	vrl_register("array", fn_array);
	vrl_register("bool", fn_bool);
	vrl_register("float", fn_float);
	vrl_register("int", fn_int);
	vrl_register("object", fn_object);
	vrl_register("string", fn_string);   /* override lenient alias with assertion */
	vrl_register("timestamp", fn_timestamp);
	vrl_register("is_empty", fn_is_empty);
	vrl_register("is_nullish", fn_is_nullish);
	vrl_register("is_json", fn_is_json);
	vrl_register("to_regex", fn_to_regex);
	vrl_register("mod", fn_mod);
	vrl_register("assert", fn_assert);
	vrl_register("assert_eq", fn_assert_eq);
	vrl_register("tag_types_externally", fn_tag_types_externally);
	vrl_register("type_def", fn_type_def);
}
