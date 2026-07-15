#define _GNU_SOURCE
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * Dynamic path functions: get / set / remove.
 * The path argument is an array of segments: strings select object keys,
 * integers select array indices.
 *
 * del() and exists() are handled specially in interp.c because they take a
 * path *expression* (AST) rather than an evaluated value.
 */

/* ---- get ---- */

static vrl_value *get_by_path(vrl_value *cur, vrl_value *path)
{
	for (size_t i = 0; i < path->u.array.len && cur; i++) {
		vrl_value *seg = path->u.array.items[i];
		if (seg->type == VRL_BYTES && cur->type == VRL_OBJECT) {
			cur = vrl_object_get(cur, seg->u.bytes.data, seg->u.bytes.len);
		} else if (seg->type == VRL_INTEGER && cur->type == VRL_ARRAY) {
			int64_t idx = seg->u.integer;
			if (idx < 0) idx += (int64_t)cur->u.array.len;
			cur = (idx >= 0 && (size_t)idx < cur->u.array.len) ? cur->u.array.items[idx] : NULL;
		} else {
			return NULL;
		}
	}
	return cur;
}

static vrl_status fn_get(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	vrl_value *path = vrl_arg(a, "path", 1);
	if (!path || path->type != VRL_ARRAY) { *err = vrl_errf("get: path must be an array"); return VRL_ERR; }
	vrl_value *r = get_by_path(v, path);
	*out = r ? vrl_value_clone(r) : vrl_null();
	return VRL_OK;
}

/* ---- set ---- */

static vrl_value *path_set(const vrl_value *cur, vrl_value **segs, size_t nsegs,
			  size_t i, vrl_value *data /* owned */)
{
	if (i == nsegs)
		return data;
	vrl_value *seg = segs[i];
	if (seg->type == VRL_BYTES) {
		vrl_value *obj = (cur && cur->type == VRL_OBJECT) ? vrl_value_clone(cur) : vrl_object_new();
		vrl_value *child = vrl_object_get(obj, seg->u.bytes.data, seg->u.bytes.len);
		vrl_value *nc = path_set(child, segs, nsegs, i + 1, data);
		vrl_object_set(obj, seg->u.bytes.data, seg->u.bytes.len, nc);
		return obj;
	}
	/* integer index */
	vrl_value *arr = (cur && cur->type == VRL_ARRAY) ? vrl_value_clone(cur) : vrl_array_new();
	int64_t idx = seg->u.integer;
	if (idx < 0) idx = 0;
	vrl_value *child = (idx >= 0 && (size_t)idx < arr->u.array.len) ? arr->u.array.items[idx] : NULL;
	vrl_value *nc = path_set(child, segs, nsegs, i + 1, data);
	vrl_array_set(arr, (size_t)idx, nc);
	return arr;
}

static vrl_status fn_set(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	vrl_value *path = vrl_arg(a, "path", 1);
	vrl_value *data = vrl_arg(a, "data", 2);
	if (!path || path->type != VRL_ARRAY) { *err = vrl_errf("set: path must be an array"); return VRL_ERR; }
	for (size_t i = 0; i < path->u.array.len; i++) {
		vrl_value *seg = path->u.array.items[i];
		if (seg->type != VRL_BYTES && seg->type != VRL_INTEGER) {
			*err = vrl_errf("set: path segments must be strings or integers");
			return VRL_ERR;
		}
	}
	*out = path_set(v, path->u.array.items, path->u.array.len, 0,
			data ? vrl_value_clone(data) : vrl_null());
	return VRL_OK;
}

/* ---- remove ---- */

static vrl_value *path_remove(const vrl_value *cur, vrl_value **segs, size_t nsegs, size_t i)
{
	if (!cur)
		return vrl_null();
	vrl_value *seg = segs[i];
	int last = (i + 1 == nsegs);
	if (seg->type == VRL_BYTES && cur->type == VRL_OBJECT) {
		vrl_value *obj = vrl_value_clone(cur);
		if (last) {
			vrl_object_del(obj, seg->u.bytes.data, seg->u.bytes.len);
		} else {
			vrl_value *child = vrl_object_get(obj, seg->u.bytes.data, seg->u.bytes.len);
			if (child) {
				vrl_value *nc = path_remove(child, segs, nsegs, i + 1);
				vrl_object_set(obj, seg->u.bytes.data, seg->u.bytes.len, nc);
			}
		}
		return obj;
	}
	if (seg->type == VRL_INTEGER && cur->type == VRL_ARRAY) {
		vrl_value *arr = vrl_value_clone(cur);
		int64_t idx = seg->u.integer;
		if (idx < 0) idx += (int64_t)arr->u.array.len;
		if (idx >= 0 && (size_t)idx < arr->u.array.len) {
			if (last) {
				vrl_value_unref(arr->u.array.items[idx]);
				memmove(&arr->u.array.items[idx], &arr->u.array.items[idx + 1],
					(arr->u.array.len - (size_t)idx - 1) * sizeof(vrl_value *));
				arr->u.array.len--;
			} else {
				vrl_value *child = arr->u.array.items[idx];
				vrl_value *nc = path_remove(child, segs, nsegs, i + 1);
				vrl_array_set(arr, (size_t)idx, nc);
			}
		}
		return arr;
	}
	return vrl_value_clone(cur);
}

static vrl_status fn_remove(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	vrl_value *path = vrl_arg(a, "path", 1);
	if (!path || path->type != VRL_ARRAY) { *err = vrl_errf("remove: path must be an array"); return VRL_ERR; }
	if (!v || (v->type != VRL_OBJECT && v->type != VRL_ARRAY)) { *err = vrl_errf("remove: value must be object or array"); return VRL_ERR; }
	if (path->u.array.len == 0) { *out = vrl_value_clone(v); return VRL_OK; }
	/* compact option: drop now-empty parents (best-effort, not implemented deeply) */
	*out = path_remove(v, path->u.array.items, path->u.array.len, 0);
	return VRL_OK;
}

void vrl_reg_path(void)
{
	vrl_register("get", fn_get);
	vrl_register("set", fn_set);
	vrl_register("remove", fn_remove);
}
