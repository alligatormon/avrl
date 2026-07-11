#pragma once
#include <stdint.h>
#include <stddef.h>
#include "common/pcre_parser.h"

/*
 * VRL value model.
 *
 * A tagged union covering the VRL type system:
 *   null, boolean, integer (i64), float (f64), bytes (string), timestamp,
 *   regex, array, object.
 *
 * Memory management is reference counted with MOVE semantics:
 *   - Every constructor returns a value with refcount == 1 (owned by caller).
 *   - Containers (array/object) OWN a reference to each child. Inserting a
 *     value (vrl_array_push / vrl_object_set) CONSUMES the caller's reference
 *     (a move); ref it first if you still need it.
 *   - Accessors (vrl_array_get / vrl_object_get) return a BORROWED pointer
 *     (no ref taken); ref it if you want to keep it beyond the container's
 *     lifetime.
 *   - vrl_value_ref()/vrl_value_unref() adjust the count; unref frees at 0.
 *
 * This keeps the core dependency-free (libc only). PCRE is only pulled in for
 * the regex value type (already an alligator dependency).
 */

typedef enum {
	VRL_NULL = 0,
	VRL_BOOLEAN,
	VRL_INTEGER,
	VRL_FLOAT,
	VRL_BYTES,     /* string / arbitrary bytes */
	VRL_TIMESTAMP, /* seconds since unix epoch, fractional */
	VRL_REGEX,
	VRL_ARRAY,
	VRL_OBJECT,
} vrl_value_type;

typedef struct vrl_value vrl_value;

typedef struct vrl_object_entry {
	char *key;
	size_t key_len;
	vrl_value *val;
} vrl_object_entry;

typedef struct vrl_object {
	vrl_object_entry *entries;
	size_t len;
	size_t cap;
} vrl_object;

typedef struct vrl_array {
	vrl_value **items;
	size_t len;
	size_t cap;
} vrl_array;

struct vrl_value {
	vrl_value_type type;
	int32_t refcount;
	union {
		int8_t boolean;
		int64_t integer;
		double flt;
		double timestamp; /* unix seconds, fractional */
		struct {
			char *data;
			size_t len;
		} bytes;
		regex_match *regex; /* owned; freed with the value */
		vrl_array array;
		vrl_object object;
	} u;
};

/* --- lifecycle --- */
vrl_value *vrl_value_ref(vrl_value *v);
void vrl_value_unref(vrl_value *v);
vrl_value *vrl_value_clone(const vrl_value *v); /* deep copy, refcount 1 */

/* --- constructors (refcount 1) --- */
vrl_value *vrl_null(void);
vrl_value *vrl_boolean(int b);
vrl_value *vrl_integer(int64_t i);
vrl_value *vrl_float(double f);
vrl_value *vrl_timestamp(double unix_seconds);
vrl_value *vrl_bytes(const char *data, size_t len);
vrl_value *vrl_bytes_cstr(const char *cstr);
vrl_value *vrl_bytes_take(char *data, size_t len); /* takes ownership of data */
vrl_value *vrl_regex_take(regex_match *re);        /* takes ownership of re */
vrl_value *vrl_array_new(void);
vrl_value *vrl_object_new(void);

/* --- array ops --- */
void vrl_array_push(vrl_value *arr, vrl_value *item /* consumed */);
vrl_value *vrl_array_get(vrl_value *arr, size_t idx); /* borrowed, NULL if oob */
int vrl_array_set(vrl_value *arr, size_t idx, vrl_value *item /* consumed */);
size_t vrl_array_len(const vrl_value *arr);

/* --- object ops --- */
/* set: consumes val; replaces existing key. returns 0 on ok. */
int vrl_object_set(vrl_value *obj, const char *key, size_t key_len, vrl_value *val);
int vrl_object_set_cstr(vrl_value *obj, const char *key, vrl_value *val);
vrl_value *vrl_object_get(vrl_value *obj, const char *key, size_t key_len); /* borrowed */
int vrl_object_del(vrl_value *obj, const char *key, size_t key_len);
size_t vrl_object_len(const vrl_value *obj);

/* --- helpers --- */
const char *vrl_type_name(vrl_value_type t);
int vrl_value_equal(const vrl_value *a, const vrl_value *b);
int vrl_value_truthy(const vrl_value *v);
/* Human/display string of a value (allocates; caller frees with free()).
 * out_len optional. Used by to_string() and string coercion. */
char *vrl_value_to_string(const vrl_value *v, size_t *out_len);
