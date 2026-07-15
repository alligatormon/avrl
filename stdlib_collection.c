#define _GNU_SOURCE
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* array: append / pop / chunks / zip                                 */
/* ================================================================== */

static vrl_status fn_append(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *arr = vrl_arg(a, "value", 0);
	vrl_value *items = vrl_arg(a, "items", 1);
	if (!arr || arr->type != VRL_ARRAY) { *err = vrl_errf("append: value must be an array"); return VRL_ERR; }
	if (!items || items->type != VRL_ARRAY) { *err = vrl_errf("append: items must be an array"); return VRL_ERR; }
	vrl_value *res = vrl_value_clone(arr);
	for (size_t i = 0; i < items->u.array.len; i++)
		vrl_array_push(res, vrl_value_clone(items->u.array.items[i]));
	*out = res;
	return VRL_OK;
}

static vrl_status fn_pop(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *arr = vrl_arg(a, "value", 0);
	if (!arr || arr->type != VRL_ARRAY) { *err = vrl_errf("pop: value must be an array"); return VRL_ERR; }
	vrl_value *res = vrl_array_new();
	for (size_t i = 0; i + 1 < arr->u.array.len; i++)
		vrl_array_push(res, vrl_value_clone(arr->u.array.items[i]));
	*out = res;
	return VRL_OK;
}

static vrl_status fn_chunks(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	vrl_value *szv = vrl_arg(a, "chunk_size", 1);
	if (!szv || szv->type != VRL_INTEGER || szv->u.integer <= 0) {
		*err = vrl_errf("chunks: chunk_size must be a positive integer");
		return VRL_ERR;
	}
	size_t sz = (size_t)szv->u.integer;
	vrl_value *res = vrl_array_new();
	if (v && v->type == VRL_ARRAY) {
		for (size_t i = 0; i < v->u.array.len; i += sz) {
			vrl_value *chunk = vrl_array_new();
			for (size_t j = i; j < i + sz && j < v->u.array.len; j++)
				vrl_array_push(chunk, vrl_value_clone(v->u.array.items[j]));
			vrl_array_push(res, chunk);
		}
	} else if (v && v->type == VRL_BYTES) {
		for (size_t i = 0; i < v->u.bytes.len; i += sz) {
			size_t take = (i + sz <= v->u.bytes.len) ? sz : v->u.bytes.len - i;
			vrl_array_push(res, vrl_bytes(v->u.bytes.data + i, take));
		}
	} else {
		vrl_value_unref(res);
		*err = vrl_errf("chunks: value must be an array or string");
		return VRL_ERR;
	}
	*out = res;
	return VRL_OK;
}

static vrl_status fn_zip(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *a0 = vrl_arg(a, "array_0", 0);
	vrl_value *a1 = vrl_arg(a, "array_1", 1);
	if (a0 && a0->type == VRL_ARRAY && !a1) {
		/* transpose: a0 is an array of arrays */
		size_t inner = 0; int first = 1;
		for (size_t i = 0; i < a0->u.array.len; i++) {
			vrl_value *e = a0->u.array.items[i];
			if (e->type != VRL_ARRAY) { *err = vrl_errf("zip: expected array of arrays"); return VRL_ERR; }
			if (first) { inner = e->u.array.len; first = 0; }
			else if (e->u.array.len < inner) inner = e->u.array.len;
		}
		vrl_value *res = vrl_array_new();
		for (size_t j = 0; j < inner; j++) {
			vrl_value *tuple = vrl_array_new();
			for (size_t i = 0; i < a0->u.array.len; i++)
				vrl_array_push(tuple, vrl_value_clone(a0->u.array.items[i]->u.array.items[j]));
			vrl_array_push(res, tuple);
		}
		*out = res;
		return VRL_OK;
	}
	if (!a0 || a0->type != VRL_ARRAY || !a1 || a1->type != VRL_ARRAY) {
		*err = vrl_errf("zip: expected two arrays");
		return VRL_ERR;
	}
	size_t n = a0->u.array.len < a1->u.array.len ? a0->u.array.len : a1->u.array.len;
	vrl_value *res = vrl_array_new();
	for (size_t i = 0; i < n; i++) {
		vrl_value *tuple = vrl_array_new();
		vrl_array_push(tuple, vrl_value_clone(a0->u.array.items[i]));
		vrl_array_push(tuple, vrl_value_clone(a1->u.array.items[i]));
		vrl_array_push(res, tuple);
	}
	*out = res;
	return VRL_OK;
}

/* ================================================================== */
/* enumerate: compact / flatten / unique / includes                   */
/* ================================================================== */

static int value_is_emptyish(const vrl_value *v, int drop_null, int drop_str, int drop_obj, int drop_arr)
{
	if (!v) return drop_null;
	switch (v->type) {
	case VRL_NULL: return drop_null;
	case VRL_BYTES: return drop_str && v->u.bytes.len == 0;
	case VRL_OBJECT: return drop_obj && v->u.object.len == 0;
	case VRL_ARRAY: return drop_arr && v->u.array.len == 0;
	default: return 0;
	}
}

static vrl_value *compact_value(const vrl_value *v, int recursive, int dn, int ds, int dobj, int darr);

static vrl_status fn_compact(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || (v->type != VRL_ARRAY && v->type != VRL_OBJECT)) {
		*err = vrl_errf("compact: expected array or object");
		return VRL_ERR;
	}
	vrl_value *rec = vrl_arg(a, "recursive", -1);
	vrl_value *nul = vrl_arg(a, "null", -1);
	vrl_value *str = vrl_arg(a, "string", -1);
	vrl_value *obj = vrl_arg(a, "object", -1);
	vrl_value *arr = vrl_arg(a, "array", -1);
	int recursive = rec ? vrl_value_truthy(rec) : 1;
	int dn = nul ? vrl_value_truthy(nul) : 1;
	int ds = str ? vrl_value_truthy(str) : 0;
	int dobj = obj ? vrl_value_truthy(obj) : 0;
	int darr = arr ? vrl_value_truthy(arr) : 0;
	*out = compact_value(v, recursive, dn, ds, dobj, darr);
	return VRL_OK;
}

static vrl_value *compact_value(const vrl_value *v, int recursive, int dn, int ds, int dobj, int darr)
{
	if (v->type == VRL_ARRAY) {
		vrl_value *res = vrl_array_new();
		for (size_t i = 0; i < v->u.array.len; i++) {
			vrl_value *e = v->u.array.items[i];
			vrl_value *ne = (recursive && (e->type == VRL_ARRAY || e->type == VRL_OBJECT))
					? compact_value(e, recursive, dn, ds, dobj, darr) : vrl_value_clone(e);
			if (value_is_emptyish(ne, dn, ds, dobj, darr)) { vrl_value_unref(ne); continue; }
			vrl_array_push(res, ne);
		}
		return res;
	}
	vrl_value *res = vrl_object_new();
	for (size_t i = 0; i < v->u.object.len; i++) {
		vrl_object_entry *en = &v->u.object.entries[i];
		vrl_value *e = en->val;
		vrl_value *ne = (recursive && (e->type == VRL_ARRAY || e->type == VRL_OBJECT))
				? compact_value(e, recursive, dn, ds, dobj, darr) : vrl_value_clone(e);
		if (value_is_emptyish(ne, dn, ds, dobj, darr)) { vrl_value_unref(ne); continue; }
		vrl_object_set(res, en->key, en->key_len, ne);
	}
	return res;
}

static void flatten_into(vrl_value *dst, const vrl_value *v, const char *prefix,
			 size_t prefix_len, const char *sep)
{
	if (v->type == VRL_OBJECT) {
		for (size_t i = 0; i < v->u.object.len; i++) {
			vrl_object_entry *e = &v->u.object.entries[i];
			avrl_buf key; avrl_buf_init(&key);
			if (prefix_len) { avrl_buf_add(&key, prefix, prefix_len); avrl_buf_puts(&key, sep); }
			avrl_buf_add(&key, e->key, e->key_len);
			if (e->val->type == VRL_OBJECT || e->val->type == VRL_ARRAY)
				flatten_into(dst, e->val, key.s ? key.s : "", key.len, sep);
			else
				vrl_object_set(dst, key.s ? key.s : "", key.len, vrl_value_clone(e->val));
			avrl_buf_free(&key);
		}
	} else if (v->type == VRL_ARRAY) {
		for (size_t i = 0; i < v->u.array.len; i++) {
			char idx[24];
			int il = snprintf(idx, sizeof(idx), "%zu", i);
			avrl_buf key; avrl_buf_init(&key);
			if (prefix_len) { avrl_buf_add(&key, prefix, prefix_len); avrl_buf_puts(&key, sep); }
			avrl_buf_add(&key, idx, (size_t)il);
			vrl_value *e = v->u.array.items[i];
			if (e->type == VRL_OBJECT || e->type == VRL_ARRAY)
				flatten_into(dst, e, key.s ? key.s : "", key.len, sep);
			else
				vrl_object_set(dst, key.s ? key.s : "", key.len, vrl_value_clone(e));
			avrl_buf_free(&key);
		}
	}
}

static void array_flatten(vrl_value *dst, const vrl_value *src)
{
	for (size_t i = 0; i < src->u.array.len; i++) {
		vrl_value *e = src->u.array.items[i];
		if (e->type == VRL_ARRAY)
			array_flatten(dst, e);
		else
			vrl_array_push(dst, vrl_value_clone(e));
	}
}

static vrl_status fn_flatten(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || (v->type != VRL_OBJECT && v->type != VRL_ARRAY)) {
		*err = vrl_errf("flatten: expected object or array");
		return VRL_ERR;
	}
	vrl_value *sepv = vrl_arg(a, "separator", 1);
	const char *sep = (sepv && sepv->type == VRL_BYTES) ? sepv->u.bytes.data : ".";
	if (v->type == VRL_ARRAY) {
		vrl_value *res = vrl_array_new();
		array_flatten(res, v);
		*out = res;
		return VRL_OK;
	}
	vrl_value *res = vrl_object_new();
	flatten_into(res, v, "", 0, sep);
	*out = res;
	return VRL_OK;
}

static vrl_status fn_unique(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_ARRAY) { *err = vrl_errf("unique: expected array"); return VRL_ERR; }
	vrl_value *res = vrl_array_new();
	for (size_t i = 0; i < v->u.array.len; i++) {
		vrl_value *e = v->u.array.items[i];
		int dup = 0;
		for (size_t j = 0; j < res->u.array.len; j++)
			if (vrl_value_equal(e, res->u.array.items[j])) { dup = 1; break; }
		if (!dup) vrl_array_push(res, vrl_value_clone(e));
	}
	*out = res;
	return VRL_OK;
}

static vrl_status fn_includes(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	vrl_value *item = vrl_arg(a, "item", 1);
	if (!v || v->type != VRL_ARRAY) { *err = vrl_errf("includes: expected array"); return VRL_ERR; }
	int found = 0;
	for (size_t i = 0; i < v->u.array.len; i++)
		if (vrl_value_equal(v->u.array.items[i], item)) { found = 1; break; }
	*out = vrl_boolean(found);
	return VRL_OK;
}

/* ================================================================== */
/* enumerate: for_each / map_keys / match_array                       */
/* ================================================================== */

static vrl_status fn_for_each(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || (v->type != VRL_ARRAY && v->type != VRL_OBJECT)) {
		*err = vrl_errf("for_each: expected array or object");
		return VRL_ERR;
	}
	if (!a->closure) { *err = vrl_errf("for_each: requires a closure"); return VRL_ERR; }
	if (v->type == VRL_OBJECT) {
		for (size_t i = 0; i < v->u.object.len; i++) {
			vrl_object_entry *e = &v->u.object.entries[i];
			vrl_value *kv = vrl_bytes(e->key, e->key_len);
			vrl_value *params[2] = { kv, e->val };
			vrl_value *r = NULL;
			vrl_status st = vrl_invoke_closure(a->ctx, a->closure, params, 2, &r, err);
			vrl_value_unref(kv);
			vrl_value_unref(r);
			if (st != VRL_OK) return st;
		}
	} else {
		for (size_t i = 0; i < v->u.array.len; i++) {
			vrl_value *iv = vrl_integer((int64_t)i);
			vrl_value *params[2] = { iv, v->u.array.items[i] };
			vrl_value *r = NULL;
			vrl_status st = vrl_invoke_closure(a->ctx, a->closure, params, 2, &r, err);
			vrl_value_unref(iv);
			vrl_value_unref(r);
			if (st != VRL_OK) return st;
		}
	}
	*out = vrl_null();
	return VRL_OK;
}

static vrl_status map_keys_rec(vrl_ctx *ctx, vrl_ast *closure, vrl_value *obj,
			       int recursive, vrl_value **out, char **err)
{
	vrl_value *res = vrl_object_new();
	for (size_t i = 0; i < obj->u.object.len; i++) {
		vrl_object_entry *e = &obj->u.object.entries[i];
		vrl_value *kv = vrl_bytes(e->key, e->key_len);
		vrl_value *params[1] = { kv };
		vrl_value *nk = NULL;
		vrl_status st = vrl_invoke_closure(ctx, closure, params, 1, &nk, err);
		vrl_value_unref(kv);
		if (st != VRL_OK) { vrl_value_unref(res); return st; }
		size_t nkl; char *nks = vrl_value_to_string(nk, &nkl);
		vrl_value_unref(nk);
		vrl_value *child;
		if (recursive && e->val->type == VRL_OBJECT) {
			st = map_keys_rec(ctx, closure, e->val, recursive, &child, err);
			if (st != VRL_OK) { free(nks); vrl_value_unref(res); return st; }
		} else {
			child = vrl_value_clone(e->val);
		}
		vrl_object_set(res, nks, nkl, child);
		free(nks);
	}
	*out = res;
	return VRL_OK;
}

static vrl_status fn_map_keys(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_OBJECT) { *err = vrl_errf("map_keys: expected object"); return VRL_ERR; }
	if (!a->closure) { *err = vrl_errf("map_keys: requires a closure"); return VRL_ERR; }
	vrl_value *rec = vrl_arg(a, "recursive", -1);
	int recursive = rec ? vrl_value_truthy(rec) : 0;
	return map_keys_rec(a->ctx, a->closure, v, recursive, out, err);
}

static vrl_status fn_match_array(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_ARRAY) { *err = vrl_errf("match_array: expected array"); return VRL_ERR; }
	vrl_value *pat = vrl_arg(a, "pattern", 1);
	regex_match *re = NULL; int owned = 0;
	if (pat && pat->type == VRL_REGEX) re = pat->u.regex;
	else if (pat && pat->type == VRL_BYTES) { re = avrl_regex_compile(pat->u.bytes.data); owned = 1; }
	if (!re) { *err = vrl_errf("match_array: expected regex/pattern"); return VRL_ERR; }
	vrl_value *allv = vrl_arg(a, "all", 2);
	int all = allv && vrl_value_truthy(allv);
	int result = all ? 1 : 0;
	for (size_t i = 0; i < v->u.array.len; i++) {
		vrl_value *e = v->u.array.items[i];
		int m = 0;
		if (e->type == VRL_BYTES) {
			int ov[AVRL_OVECCOUNT];
			m = avrl_regex_exec(re, e->u.bytes.data, e->u.bytes.len, ov, AVRL_OVECCOUNT, a->ctx->ll) > 0;
		}
		if (all && !m) { result = 0; break; }
		if (!all && m) { result = 1; break; }
	}
	if (owned) avrl_regex_free(re);
	*out = vrl_boolean(result);
	return VRL_OK;
}

/* ================================================================== */
/* enumerate: tally / tally_value                                     */
/* ================================================================== */

static vrl_status fn_tally(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_ARRAY) { *err = vrl_errf("tally: expected array"); return VRL_ERR; }
	vrl_value *res = vrl_object_new();
	for (size_t i = 0; i < v->u.array.len; i++) {
		vrl_value *e = v->u.array.items[i];
		if (e->type != VRL_BYTES) continue;
		vrl_value *cur = vrl_object_get(res, e->u.bytes.data, e->u.bytes.len);
		int64_t c = (cur && cur->type == VRL_INTEGER) ? cur->u.integer : 0;
		vrl_object_set(res, e->u.bytes.data, e->u.bytes.len, vrl_integer(c + 1));
	}
	*out = res;
	return VRL_OK;
}

static vrl_status fn_tally_value(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "array", 0);
	if (!v) v = vrl_arg(a, "value", 0);
	vrl_value *target = vrl_arg(a, "value", 1);
	if (!v || v->type != VRL_ARRAY) { *err = vrl_errf("tally_value: expected array"); return VRL_ERR; }
	int64_t count = 0;
	for (size_t i = 0; i < v->u.array.len; i++)
		if (vrl_value_equal(v->u.array.items[i], target)) count++;
	*out = vrl_integer(count);
	return VRL_OK;
}

/* ================================================================== */
/* object: to_entries / from_entries / object_from_array / unnest     */
/* ================================================================== */

static vrl_status fn_to_entries(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_OBJECT) { *err = vrl_errf("to_entries: expected object"); return VRL_ERR; }
	vrl_value *arr = vrl_array_new();
	for (size_t i = 0; i < v->u.object.len; i++) {
		vrl_object_entry *e = &v->u.object.entries[i];
		vrl_value *pair = vrl_object_new();
		vrl_object_set_cstr(pair, "key", vrl_bytes(e->key, e->key_len));
		vrl_object_set_cstr(pair, "value", vrl_value_clone(e->val));
		vrl_array_push(arr, pair);
	}
	*out = arr;
	return VRL_OK;
}

static vrl_status fn_from_entries(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_ARRAY) { *err = vrl_errf("from_entries: expected array"); return VRL_ERR; }
	vrl_value *obj = vrl_object_new();
	for (size_t i = 0; i < v->u.array.len; i++) {
		vrl_value *e = v->u.array.items[i];
		const char *k = NULL; size_t kl = 0; vrl_value *val = NULL;
		if (e->type == VRL_ARRAY && e->u.array.len >= 2) {
			vrl_value *kk = e->u.array.items[0];
			if (kk->type != VRL_BYTES) { vrl_value_unref(obj); *err = vrl_errf("from_entries: key must be string"); return VRL_ERR; }
			k = kk->u.bytes.data; kl = kk->u.bytes.len; val = e->u.array.items[1];
		} else if (e->type == VRL_OBJECT) {
			vrl_value *kk = vrl_object_get(e, "key", 3);
			val = vrl_object_get(e, "value", 5);
			if (!kk || kk->type != VRL_BYTES) { vrl_value_unref(obj); *err = vrl_errf("from_entries: entry needs string 'key'"); return VRL_ERR; }
			k = kk->u.bytes.data; kl = kk->u.bytes.len;
		} else {
			vrl_value_unref(obj);
			*err = vrl_errf("from_entries: each entry must be [key,value] or {key,value}");
			return VRL_ERR;
		}
		vrl_object_set(obj, k, kl, val ? vrl_value_clone(val) : vrl_null());
	}
	*out = obj;
	return VRL_OK;
}

static vrl_status fn_object_from_array(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *values = vrl_arg(a, "values", 0);
	vrl_value *keys = vrl_arg(a, "keys", 1);
	/* single-arg form: array of [key, value] pairs */
	if (values && values->type == VRL_ARRAY && !keys) {
		vrl_value *obj = vrl_object_new();
		for (size_t i = 0; i < values->u.array.len; i++) {
			vrl_value *e = values->u.array.items[i];
			if (e->type != VRL_ARRAY || e->u.array.len < 2 || e->u.array.items[0]->type != VRL_BYTES) {
				vrl_value_unref(obj); *err = vrl_errf("object_from_array: expected [key,value] pairs"); return VRL_ERR;
			}
			vrl_value *kk = e->u.array.items[0];
			vrl_object_set(obj, kk->u.bytes.data, kk->u.bytes.len, vrl_value_clone(e->u.array.items[1]));
		}
		*out = obj;
		return VRL_OK;
	}
	if (!values || values->type != VRL_ARRAY || !keys || keys->type != VRL_ARRAY) {
		*err = vrl_errf("object_from_array: expected values and keys arrays");
		return VRL_ERR;
	}
	size_t n = keys->u.array.len < values->u.array.len ? keys->u.array.len : values->u.array.len;
	vrl_value *obj = vrl_object_new();
	for (size_t i = 0; i < n; i++) {
		vrl_value *kk = keys->u.array.items[i];
		if (kk->type != VRL_BYTES) { vrl_value_unref(obj); *err = vrl_errf("object_from_array: keys must be strings"); return VRL_ERR; }
		vrl_object_set(obj, kk->u.bytes.data, kk->u.bytes.len, vrl_value_clone(values->u.array.items[i]));
	}
	*out = obj;
	return VRL_OK;
}

/* unnest(object, field): expand an array field into one object per element.
 * Deviates slightly from VRL's path-only signature (documented in notes). */
static vrl_status fn_unnest(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *obj = vrl_arg(a, "value", 0);
	const char *field; size_t fl;
	if (!obj || obj->type != VRL_OBJECT) { *err = vrl_errf("unnest: expected object as first arg"); return VRL_ERR; }
	if (!avrl_arg_str(a, "path", 1, &field, &fl, err)) return VRL_ERR;
	vrl_value *arr = vrl_object_get(obj, field, fl);
	if (!arr || arr->type != VRL_ARRAY) { *err = vrl_errf("unnest: field '%s' is not an array", field); return VRL_ERR; }
	vrl_value *res = vrl_array_new();
	for (size_t i = 0; i < arr->u.array.len; i++) {
		vrl_value *copy = vrl_value_clone(obj);
		vrl_object_set(copy, field, fl, vrl_value_clone(arr->u.array.items[i]));
		vrl_array_push(res, copy);
	}
	*out = res;
	return VRL_OK;
}

/* ================================================================== */
/* unflatten                                                          */
/* ================================================================== */

static vrl_status fn_unflatten(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *v = vrl_arg(a, "value", 0);
	if (!v || v->type != VRL_OBJECT) { *err = vrl_errf("unflatten: expected object"); return VRL_ERR; }
	vrl_value *sepv = vrl_arg(a, "separator", 1);
	const char *sep = (sepv && sepv->type == VRL_BYTES) ? sepv->u.bytes.data : ".";
	size_t seplen = strlen(sep);
	if (seplen == 0) seplen = 1;
	vrl_value *root = vrl_object_new();
	for (size_t i = 0; i < v->u.object.len; i++) {
		vrl_object_entry *e = &v->u.object.entries[i];
		const char *k = e->key; size_t kl = e->key_len;
		vrl_value *cur = root;
		size_t start = 0;
		while (1) {
			/* find next separator at or after `start` */
			size_t p = start;
			int found = 0;
			while (p + seplen <= kl) {
				if (!memcmp(k + p, sep, seplen)) { found = 1; break; }
				p++;
			}
			if (!found) {
				vrl_object_set(cur, k + start, kl - start, vrl_value_clone(e->val));
				break;
			}
			size_t segl = p - start;
			vrl_value *child = vrl_object_get(cur, k + start, segl);
			if (!child || child->type != VRL_OBJECT) {
				vrl_object_set(cur, k + start, segl, vrl_object_new());
				child = vrl_object_get(cur, k + start, segl);
			}
			cur = child;
			start = p + seplen;
		}
	}
	*out = root;
	return VRL_OK;
}

/* ================================================================== */
/* match_datadog_query (subset)                                       */
/* ================================================================== */

/* Supported: parentheses, AND/OR (explicit + implicit), NOT / '-',
 *   field:value, @attr:value, field:* (exists), "quoted phrase",
 *   numeric comparisons field:>n field:>=n field:<n field:<=n,
 *   ranges field:[lo TO hi], and bare full-text terms (matched against
 *   the `message` field). Wildcards '*' and '?' are honoured in values.
 * Not supported: field:(a OR b) grouping, facet/tag-array semantics
 *   beyond direct field lookup. Documented as partial. */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

typedef struct { const char *s; size_t n; size_t i; } dqp;

static void dq_ws(dqp *p) { while (p->i < p->n && isspace((unsigned char)p->s[p->i])) p->i++; }

static int wc_match(const char *pat, size_t pl, const char *txt, size_t tl)
{
	/* '*' matches any run, '?' any single char; anchored full match */
	size_t pi = 0, ti = 0, star_p = (size_t)-1, star_t = 0;
	while (ti < tl) {
		if (pi < pl && (pat[pi] == '?' || pat[pi] == txt[ti])) { pi++; ti++; }
		else if (pi < pl && pat[pi] == '*') { star_p = pi++; star_t = ti; }
		else if (star_p != (size_t)-1) { pi = star_p + 1; ti = ++star_t; }
		else return 0;
	}
	while (pi < pl && pat[pi] == '*') pi++;
	return pi == pl;
}

/* Resolve a dotted path (a.b.c) to a borrowed value, or NULL. */
static vrl_value *dq_lookup(vrl_value *obj, const char *field, size_t fl)
{
	vrl_value *cur = obj;
	size_t start = 0;
	while (start <= fl && cur && cur->type == VRL_OBJECT) {
		size_t p = start;
		while (p < fl && field[p] != '.') p++;
		cur = vrl_object_get(cur, field + start, p - start);
		if (p >= fl) return cur;
		start = p + 1;
	}
	return NULL;
}

static int dq_num(const vrl_value *v, double *d)
{
	if (!v) return 0;
	if (v->type == VRL_INTEGER) { *d = (double)v->u.integer; return 1; }
	if (v->type == VRL_FLOAT) { *d = v->u.flt; return 1; }
	if (v->type == VRL_BYTES) { char *e; *d = strtod(v->u.bytes.data, &e); return e != v->u.bytes.data; }
	return 0;
}

static int dq_term(dqp *p, vrl_value *obj)
{
	dq_ws(p);
	/* try to read field: */
	size_t fs = p->i;
	while (p->i < p->n) {
		char c = p->s[p->i];
		if (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '-') p->i++;
		else break;
	}
	size_t fe = p->i;
	const char *field = NULL; size_t fl = 0;
	if (p->i < p->n && p->s[p->i] == ':' && fe > fs) {
		field = p->s + fs; fl = fe - fs;
		if (field[0] == '@') { field++; fl--; }
		p->i++; /* skip ':' */
	} else {
		p->i = fs; /* not a field term */
	}

	/* read value token */
	dq_ws(p);
	if (p->i >= p->n) return 0;
	char c = p->s[p->i];
	/* range [lo TO hi] */
	if (field && c == '[') {
		p->i++;
		size_t ls = p->i; while (p->i < p->n && p->s[p->i] != ' ') p->i++;
		size_t le = p->i; dq_ws(p);
		/* expect TO */
		if (p->i + 2 <= p->n && (p->s[p->i] == 'T' || p->s[p->i] == 't')) p->i += 2;
		dq_ws(p);
		size_t hs = p->i; while (p->i < p->n && p->s[p->i] != ']') p->i++;
		size_t he = p->i; if (p->i < p->n) p->i++;
		double lo = strtod(p->s + ls, NULL), hi = strtod(p->s + hs, NULL);
		(void)le; (void)he;
		vrl_value *v = dq_lookup(obj, field, fl);
		double dv;
		return dq_num(v, &dv) && dv >= lo && dv <= hi;
	}
	/* comparison >, >=, <, <= */
	if (field && (c == '>' || c == '<')) {
		int op = c; p->i++;
		int eq = (p->i < p->n && p->s[p->i] == '='); if (eq) p->i++;
		size_t vs = p->i; while (p->i < p->n && !isspace((unsigned char)p->s[p->i]) && p->s[p->i] != ')') p->i++;
		double bound = strtod(p->s + vs, NULL);
		vrl_value *v = dq_lookup(obj, field, fl);
		double dv;
		if (!dq_num(v, &dv)) return 0;
		if (op == '>') return eq ? dv >= bound : dv > bound;
		return eq ? dv <= bound : dv < bound;
	}
	/* value token (quoted or bare) */
	const char *vs; size_t vl;
	if (c == '"') {
		p->i++; vs = p->s + p->i;
		while (p->i < p->n && p->s[p->i] != '"') p->i++;
		vl = (p->s + p->i) - vs; if (p->i < p->n) p->i++;
	} else {
		vs = p->s + p->i;
		while (p->i < p->n && !isspace((unsigned char)p->s[p->i]) && p->s[p->i] != ')') p->i++;
		vl = (p->s + p->i) - vs;
	}
	if (field) {
		vrl_value *v = dq_lookup(obj, field, fl);
		if (vl == 1 && vs[0] == '*') return v != NULL && v->type != VRL_NULL; /* exists */
		if (!v) return 0;
		size_t sl; char *sv = vrl_value_to_string(v, &sl);
		int m = wc_match(vs, vl, sv, sl);
		free(sv);
		return m;
	}
	/* bare full-text: match against message field */
	vrl_value *msg = vrl_object_get(obj, "message", 7);
	if (!msg || msg->type != VRL_BYTES) return 0;
	/* substring/wildcard: wrap with implicit * on both ends if no wildcard */
	int has_wc = 0; for (size_t k = 0; k < vl; k++) if (vs[k] == '*' || vs[k] == '?') has_wc = 1;
	if (has_wc) return wc_match(vs, vl, msg->u.bytes.data, msg->u.bytes.len);
	if (vl == 0) return 1;
	for (size_t k = 0; k + vl <= msg->u.bytes.len; k++)
		if (!memcmp(msg->u.bytes.data + k, vs, vl)) return 1;
	return 0;
}

static int dq_or(dqp *p, vrl_value *obj);

static int dq_not(dqp *p, vrl_value *obj)
{
	dq_ws(p);
	if (p->i < p->n && p->s[p->i] == '-') { p->i++; return !dq_not(p, obj); }
	if (p->i + 3 <= p->n && !strncasecmp(p->s + p->i, "NOT", 3) &&
	    (p->i + 3 == p->n || isspace((unsigned char)p->s[p->i + 3]))) {
		p->i += 3; return !dq_not(p, obj);
	}
	dq_ws(p);
	if (p->i < p->n && p->s[p->i] == '(') {
		p->i++;
		int r = dq_or(p, obj);
		dq_ws(p);
		if (p->i < p->n && p->s[p->i] == ')') p->i++;
		return r;
	}
	return dq_term(p, obj);
}

/* peek an operator keyword; returns 1=OR, 2=AND, 0=none (does not consume) */
static int dq_peek_op(dqp *p)
{
	size_t save = p->i;
	dq_ws(p);
	int op = 0;
	if (p->i + 2 <= p->n && !strncasecmp(p->s + p->i, "OR", 2) &&
	    (p->i + 2 == p->n || isspace((unsigned char)p->s[p->i + 2]))) op = 1;
	else if (p->i + 3 <= p->n && !strncasecmp(p->s + p->i, "AND", 3) &&
		 (p->i + 3 == p->n || isspace((unsigned char)p->s[p->i + 3]))) op = 2;
	p->i = save;
	return op;
}

static int dq_and(dqp *p, vrl_value *obj)
{
	int r = dq_not(p, obj);
	while (1) {
		dq_ws(p);
		if (p->i >= p->n || p->s[p->i] == ')') break;
		int op = dq_peek_op(p);
		if (op == 1) break;          /* leave OR for dq_or */
		if (op == 2) { dq_ws(p); p->i += 3; } /* explicit AND */
		int r2 = dq_not(p, obj);
		r = r && r2;
	}
	return r;
}

static int dq_or(dqp *p, vrl_value *obj)
{
	int r = dq_and(p, obj);
	while (1) {
		if (dq_peek_op(p) != 1) break;
		dq_ws(p); p->i += 2; /* consume OR */
		int r2 = dq_and(p, obj);
		r = r || r2;
	}
	return r;
}

static vrl_status fn_match_datadog_query(vrl_call_args *a, vrl_value **out, char **err)
{
	vrl_value *obj = vrl_arg(a, "value", 0);
	if (!obj || obj->type != VRL_OBJECT) { *err = vrl_errf("match_datadog_query: value must be an object"); return VRL_ERR; }
	const char *q; size_t ql;
	if (!avrl_arg_str(a, "query", 1, &q, &ql, err)) return VRL_ERR;
	dqp p = { q, ql, 0 };
	*out = vrl_boolean(dq_or(&p, obj));
	return VRL_OK;
}

void vrl_reg_collection(void)
{
	vrl_register("match_datadog_query", fn_match_datadog_query);
	vrl_register("append", fn_append);
	vrl_register("pop", fn_pop);
	vrl_register("chunks", fn_chunks);
	vrl_register("zip", fn_zip);
	vrl_register("compact", fn_compact);
	vrl_register("flatten", fn_flatten);
	vrl_register("unique", fn_unique);
	vrl_register("includes", fn_includes);
	vrl_register("for_each", fn_for_each);
	vrl_register("map_keys", fn_map_keys);
	vrl_register("match_array", fn_match_array);
	vrl_register("tally", fn_tally);
	vrl_register("tally_value", fn_tally_value);
	vrl_register("to_entries", fn_to_entries);
	vrl_register("from_entries", fn_from_entries);
	vrl_register("object_from_array", fn_object_from_array);
	vrl_register("unnest", fn_unnest);
	vrl_register("unflatten", fn_unflatten);
}
