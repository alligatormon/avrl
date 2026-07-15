#define _GNU_SOURCE
#include "interp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* ctx                                                                 */
/* ------------------------------------------------------------------ */

vrl_ctx *vrl_ctx_new(avrl_log_level ll)
{
	vrl_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->event = vrl_object_new();
	ctx->vars = vrl_object_new();
	ctx->ll = ll;
	return ctx;
}

void vrl_ctx_free(vrl_ctx *ctx)
{
	if (!ctx)
		return;
	vrl_value_unref(ctx->event);
	vrl_value_unref(ctx->vars);
	free(ctx->error);
	free(ctx->abort_msg);
	free(ctx);
}

void vrl_ctx_set_event(vrl_ctx *ctx, vrl_value *event)
{
	vrl_value_unref(ctx->event);
	ctx->event = event;
}

void vrl_ctx_reset(vrl_ctx *ctx)
{
	vrl_value_unref(ctx->vars);
	ctx->vars = vrl_object_new();
	free(ctx->error);
	ctx->error = NULL;
	free(ctx->abort_msg);
	ctx->abort_msg = NULL;
	ctx->aborted = 0;
}

char *vrl_errf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return strdup(buf);
}

static void ctx_set_error(vrl_ctx *ctx, char *msg /* owned */)
{
	free(ctx->error);
	ctx->error = msg;
}

/* ------------------------------------------------------------------ */
/* numeric helpers                                                     */
/* ------------------------------------------------------------------ */

static int as_double(const vrl_value *v, double *out)
{
	if (!v)
		return 0;
	switch (v->type) {
	case VRL_INTEGER: *out = (double)v->u.integer; return 1;
	case VRL_FLOAT:   *out = v->u.flt; return 1;
	case VRL_TIMESTAMP: *out = v->u.timestamp; return 1;
	default: return 0;
	}
}

static int both_int(const vrl_value *a, const vrl_value *b)
{
	return a->type == VRL_INTEGER && b->type == VRL_INTEGER;
}

/* ------------------------------------------------------------------ */
/* variables                                                           */
/* ------------------------------------------------------------------ */

static vrl_value *var_get(vrl_ctx *ctx, const char *name, size_t len)
{
	return vrl_object_get(ctx->vars, name, len);
}

static void var_set(vrl_ctx *ctx, const char *name, size_t len, vrl_value *v /* owned */)
{
	vrl_object_set(ctx->vars, name, len, v);
}

/* ------------------------------------------------------------------ */
/* lvalue resolution                                                   */
/* ------------------------------------------------------------------ */

typedef enum { WANT_OBJECT, WANT_ARRAY } want_type;

/* forward */
static vrl_status eval(vrl_ctx *ctx, vrl_ast *a, vrl_value **out);

/* Return a borrowed, mutable container for the value denoted by `node`,
 * creating objects/arrays along the way. Returns NULL on failure. */
static vrl_value *get_or_create(vrl_ctx *ctx, vrl_ast *node, want_type want)
{
	switch (node->kind) {
	case AST_EVENT_ROOT:
		if (ctx->event->type != VRL_OBJECT && want == WANT_OBJECT) {
			vrl_ctx_set_event(ctx, vrl_object_new());
		}
		return ctx->event;
	case AST_IDENT: {
		vrl_value *cur = var_get(ctx, node->u.ident.name, node->u.ident.name_len);
		vrl_value_type need = want == WANT_OBJECT ? VRL_OBJECT : VRL_ARRAY;
		if (cur && cur->type == need)
			return cur;
		vrl_value *nc = want == WANT_OBJECT ? vrl_object_new() : vrl_array_new();
		var_set(ctx, node->u.ident.name, node->u.ident.name_len, nc);
		return var_get(ctx, node->u.ident.name, node->u.ident.name_len);
	}
	case AST_FIELD: {
		vrl_value *parent = get_or_create(ctx, node->u.field.target, WANT_OBJECT);
		if (!parent)
			return NULL;
		vrl_value_type need = want == WANT_OBJECT ? VRL_OBJECT : VRL_ARRAY;
		vrl_value *cur = vrl_object_get(parent, node->u.field.name, node->u.field.name_len);
		if (cur && cur->type == need)
			return cur;
		vrl_value *nc = want == WANT_OBJECT ? vrl_object_new() : vrl_array_new();
		vrl_object_set(parent, node->u.field.name, node->u.field.name_len, nc);
		return vrl_object_get(parent, node->u.field.name, node->u.field.name_len);
	}
	case AST_INDEX: {
		vrl_value *idx = NULL;
		if (eval(ctx, node->u.index.index, &idx) != VRL_OK)
			return NULL;
		if (idx->type == VRL_INTEGER) {
			int64_t i = idx->u.integer;
			vrl_value_unref(idx);
			if (i < 0)
				return NULL;
			vrl_value *parent = get_or_create(ctx, node->u.index.target, WANT_ARRAY);
			if (!parent)
				return NULL;
			vrl_value_type need = want == WANT_OBJECT ? VRL_OBJECT : VRL_ARRAY;
			vrl_value *cur = vrl_array_get(parent, (size_t)i);
			if (cur && cur->type == need)
				return cur;
			vrl_value *nc = want == WANT_OBJECT ? vrl_object_new() : vrl_array_new();
			vrl_array_set(parent, (size_t)i, nc);
			return vrl_array_get(parent, (size_t)i);
		} else if (idx->type == VRL_BYTES) {
			vrl_value *parent = get_or_create(ctx, node->u.index.target, WANT_OBJECT);
			char *k = idx->u.bytes.data;
			size_t kl = idx->u.bytes.len;
			vrl_value *ret = NULL;
			if (parent) {
				vrl_value_type need = want == WANT_OBJECT ? VRL_OBJECT : VRL_ARRAY;
				vrl_value *cur = vrl_object_get(parent, k, kl);
				if (cur && cur->type == need) {
					vrl_value_unref(idx);
					return cur;
				}
				vrl_value *nc = want == WANT_OBJECT ? vrl_object_new() : vrl_array_new();
				vrl_object_set(parent, k, kl, nc);
				ret = vrl_object_get(parent, k, kl);
			}
			vrl_value_unref(idx);
			return ret;
		}
		vrl_value_unref(idx);
		return NULL;
	}
	default:
		return NULL;
	}
}

/* Assign `value` (owned) to the lvalue `target`. */
static vrl_status set_path(vrl_ctx *ctx, vrl_ast *target, vrl_value *value)
{
	switch (target->kind) {
	case AST_EVENT_ROOT:
		vrl_ctx_set_event(ctx, value);
		return VRL_OK;
	case AST_IDENT:
		var_set(ctx, target->u.ident.name, target->u.ident.name_len, value);
		return VRL_OK;
	case AST_FIELD: {
		vrl_value *parent = get_or_create(ctx, target->u.field.target, WANT_OBJECT);
		if (!parent) {
			vrl_value_unref(value);
			ctx_set_error(ctx, vrl_errf("cannot assign to path"));
			return VRL_ERR;
		}
		vrl_object_set(parent, target->u.field.name, target->u.field.name_len, value);
		return VRL_OK;
	}
	case AST_INDEX: {
		vrl_value *idx = NULL;
		if (eval(ctx, target->u.index.index, &idx) != VRL_OK) {
			vrl_value_unref(value);
			return VRL_ERR;
		}
		if (idx->type == VRL_INTEGER) {
			int64_t i = idx->u.integer;
			vrl_value_unref(idx);
			vrl_value *parent = get_or_create(ctx, target->u.index.target, WANT_ARRAY);
			if (!parent || i < 0) {
				vrl_value_unref(value);
				ctx_set_error(ctx, vrl_errf("invalid array index assignment"));
				return VRL_ERR;
			}
			vrl_array_set(parent, (size_t)i, value);
			return VRL_OK;
		} else if (idx->type == VRL_BYTES) {
			vrl_value *parent = get_or_create(ctx, target->u.index.target, WANT_OBJECT);
			if (!parent) {
				vrl_value_unref(idx);
				vrl_value_unref(value);
				return VRL_ERR;
			}
			vrl_object_set(parent, idx->u.bytes.data, idx->u.bytes.len, value);
			vrl_value_unref(idx);
			return VRL_OK;
		}
		vrl_value_unref(idx);
		vrl_value_unref(value);
		ctx_set_error(ctx, vrl_errf("index must be integer or string"));
		return VRL_ERR;
	}
	default:
		vrl_value_unref(value);
		ctx_set_error(ctx, vrl_errf("invalid assignment target"));
		return VRL_ERR;
	}
}

/* ------------------------------------------------------------------ */
/* operators                                                           */
/* ------------------------------------------------------------------ */

static vrl_status binop_arith(vrl_ctx *ctx, vrl_binop op, vrl_value *l, vrl_value *r, vrl_value **out)
{
	/* string concatenation for + */
	if (op == OP_ADD && l->type == VRL_BYTES && r->type == VRL_BYTES) {
		size_t n = l->u.bytes.len + r->u.bytes.len;
		char *buf = malloc(n + 1);
		memcpy(buf, l->u.bytes.data, l->u.bytes.len);
		memcpy(buf + l->u.bytes.len, r->u.bytes.data, r->u.bytes.len);
		buf[n] = '\0';
		*out = vrl_bytes_take(buf, n);
		return VRL_OK;
	}
	double a, b;
	if (!as_double(l, &a) || !as_double(r, &b)) {
		ctx_set_error(ctx, vrl_errf("arithmetic on non-numeric (%s, %s)",
					    vrl_type_name(l->type), vrl_type_name(r->type)));
		return VRL_ERR;
	}
	switch (op) {
	case OP_ADD:
		*out = both_int(l, r) ? vrl_integer(l->u.integer + r->u.integer) : vrl_float(a + b);
		return VRL_OK;
	case OP_SUB:
		*out = both_int(l, r) ? vrl_integer(l->u.integer - r->u.integer) : vrl_float(a - b);
		return VRL_OK;
	case OP_MUL:
		*out = both_int(l, r) ? vrl_integer(l->u.integer * r->u.integer) : vrl_float(a * b);
		return VRL_OK;
	case OP_DIV:
		if (b == 0.0) {
			ctx_set_error(ctx, vrl_errf("division by zero"));
			return VRL_ERR;
		}
		*out = vrl_float(a / b);
		return VRL_OK;
	case OP_MOD:
		if (b == 0.0) {
			ctx_set_error(ctx, vrl_errf("modulo by zero"));
			return VRL_ERR;
		}
		*out = both_int(l, r) ? vrl_integer(l->u.integer % r->u.integer) : vrl_float(fmod(a, b));
		return VRL_OK;
	default:
		return VRL_ERR;
	}
}

static vrl_status binop_compare(vrl_ctx *ctx, vrl_binop op, vrl_value *l, vrl_value *r, vrl_value **out)
{
	int cmp;
	double a, b;
	if (as_double(l, &a) && as_double(r, &b)) {
		cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
	} else if (l->type == VRL_BYTES && r->type == VRL_BYTES) {
		size_t n = l->u.bytes.len < r->u.bytes.len ? l->u.bytes.len : r->u.bytes.len;
		cmp = memcmp(l->u.bytes.data, r->u.bytes.data, n);
		if (cmp == 0)
			cmp = (l->u.bytes.len < r->u.bytes.len) ? -1 : (l->u.bytes.len > r->u.bytes.len) ? 1 : 0;
	} else {
		ctx_set_error(ctx, vrl_errf("cannot compare %s and %s",
					    vrl_type_name(l->type), vrl_type_name(r->type)));
		return VRL_ERR;
	}
	int res = 0;
	switch (op) {
	case OP_LT: res = cmp < 0; break;
	case OP_LE: res = cmp <= 0; break;
	case OP_GT: res = cmp > 0; break;
	case OP_GE: res = cmp >= 0; break;
	default: break;
	}
	*out = vrl_boolean(res);
	return VRL_OK;
}

/* ------------------------------------------------------------------ */
/* calls                                                               */
/* ------------------------------------------------------------------ */

vrl_value *vrl_arg(vrl_call_args *a, const char *name, int idx)
{
	if (name) {
		for (size_t i = 0; i < a->n; i++)
			if (a->names[i] && strcmp(a->names[i], name) == 0)
				return a->args[i];
	}
	if (idx >= 0) {
		int count = 0;
		for (size_t i = 0; i < a->n; i++) {
			if (a->names[i])
				continue;
			if (count == idx)
				return a->args[i];
			count++;
		}
	}
	return NULL;
}

/* Read-only path resolution: returns a BORROWED value or NULL if the path
 * does not exist. Does not auto-create containers. */
static vrl_value *resolve_ro(vrl_ctx *ctx, vrl_ast *node)
{
	switch (node->kind) {
	case AST_EVENT_ROOT:
		return ctx->event;
	case AST_IDENT:
		return var_get(ctx, node->u.ident.name, node->u.ident.name_len);
	case AST_FIELD: {
		vrl_value *p = resolve_ro(ctx, node->u.field.target);
		if (!p || p->type != VRL_OBJECT)
			return NULL;
		return vrl_object_get(p, node->u.field.name, node->u.field.name_len);
	}
	case AST_INDEX: {
		vrl_value *p = resolve_ro(ctx, node->u.index.target);
		if (!p)
			return NULL;
		vrl_value *idx = NULL;
		if (eval(ctx, node->u.index.index, &idx) != VRL_OK)
			return NULL;
		vrl_value *res = NULL;
		if (idx->type == VRL_INTEGER && p->type == VRL_ARRAY) {
			int64_t i = idx->u.integer;
			if (i < 0) i += (int64_t)vrl_array_len(p);
			if (i >= 0 && (size_t)i < vrl_array_len(p))
				res = vrl_array_get(p, (size_t)i);
		} else if (idx->type == VRL_BYTES && p->type == VRL_OBJECT) {
			res = vrl_object_get(p, idx->u.bytes.data, idx->u.bytes.len);
		}
		vrl_value_unref(idx);
		return res;
	}
	default:
		return NULL;
	}
}

static vrl_status eval_exists(vrl_ctx *ctx, vrl_ast *node, vrl_value **out)
{
	if (node->u.call.n != 1) {
		ctx_set_error(ctx, vrl_errf("exists: expects one path argument"));
		return VRL_ERR;
	}
	vrl_ast *arg = node->u.call.args[0];
	*out = vrl_boolean(resolve_ro(ctx, arg) != NULL);
	return VRL_OK;
}

/* Remove one array element by index, shifting the tail down. */
static void array_remove_at(vrl_value *arr, size_t idx)
{
	if (idx >= arr->u.array.len)
		return;
	vrl_value_unref(arr->u.array.items[idx]);
	memmove(&arr->u.array.items[idx], &arr->u.array.items[idx + 1],
		(arr->u.array.len - idx - 1) * sizeof(vrl_value *));
	arr->u.array.len--;
}

static vrl_status eval_del(vrl_ctx *ctx, vrl_ast *node, vrl_value **out)
{
	if (node->u.call.n != 1) {
		ctx_set_error(ctx, vrl_errf("del: expects one path argument"));
		return VRL_ERR;
	}
	vrl_ast *arg = node->u.call.args[0];
	if (arg->kind == AST_FIELD) {
		vrl_value *p = resolve_ro(ctx, arg->u.field.target);
		if (p && p->type == VRL_OBJECT) {
			vrl_value *cur = vrl_object_get(p, arg->u.field.name, arg->u.field.name_len);
			if (cur) {
				*out = vrl_value_clone(cur);
				vrl_object_del(p, arg->u.field.name, arg->u.field.name_len);
				return VRL_OK;
			}
		}
		*out = vrl_null();
		return VRL_OK;
	}
	if (arg->kind == AST_INDEX) {
		vrl_value *p = resolve_ro(ctx, arg->u.index.target);
		vrl_value *idx = NULL;
		if (p && eval(ctx, arg->u.index.index, &idx) == VRL_OK) {
			if (idx->type == VRL_INTEGER && p->type == VRL_ARRAY) {
				int64_t i = idx->u.integer;
				if (i < 0) i += (int64_t)vrl_array_len(p);
				if (i >= 0 && (size_t)i < vrl_array_len(p)) {
					*out = vrl_value_clone(vrl_array_get(p, (size_t)i));
					array_remove_at(p, (size_t)i);
					vrl_value_unref(idx);
					return VRL_OK;
				}
			} else if (idx->type == VRL_BYTES && p->type == VRL_OBJECT) {
				vrl_value *cur = vrl_object_get(p, idx->u.bytes.data, idx->u.bytes.len);
				if (cur) {
					*out = vrl_value_clone(cur);
					vrl_object_del(p, idx->u.bytes.data, idx->u.bytes.len);
					vrl_value_unref(idx);
					return VRL_OK;
				}
			}
			vrl_value_unref(idx);
		}
		*out = vrl_null();
		return VRL_OK;
	}
	if (arg->kind == AST_IDENT) {
		vrl_value *cur = var_get(ctx, arg->u.ident.name, arg->u.ident.name_len);
		*out = cur ? vrl_value_clone(cur) : vrl_null();
		vrl_object_del(ctx->vars, arg->u.ident.name, arg->u.ident.name_len);
		return VRL_OK;
	}
	*out = vrl_null();
	return VRL_OK;
}

static vrl_status eval_call(vrl_ctx *ctx, vrl_ast *node, vrl_value **out)
{
	/* path-AST builtins operate on the unevaluated path expression */
	if (node->u.call.name_len == 3 && !memcmp(node->u.call.name, "del", 3))
		return eval_del(ctx, node, out);
	if (node->u.call.name_len == 6 && !memcmp(node->u.call.name, "exists", 6))
		return eval_exists(ctx, node, out);

	vrl_fn fn = vrl_stdlib_lookup(node->u.call.name, node->u.call.name_len);
	if (!fn) {
		ctx_set_error(ctx, vrl_errf("unknown function '%s'", node->u.call.name));
		return VRL_ABORT; /* unknown function is a program error */
	}

	size_t n = node->u.call.n;
	vrl_value **argv = n ? calloc(n, sizeof(vrl_value *)) : NULL;
	vrl_status st = VRL_OK;
	for (size_t i = 0; i < n; i++) {
		st = eval(ctx, node->u.call.args[i], &argv[i]);
		if (st != VRL_OK) {
			for (size_t j = 0; j < i; j++)
				vrl_value_unref(argv[j]);
			free(argv);
			return st;
		}
	}

	vrl_call_args ca = {.ctx = ctx, .args = argv, .names = node->u.call.arg_names,
			    .n = n, .closure = node->u.call.closure};
	char *err = NULL;
	vrl_value *result = NULL;
	st = fn(&ca, &result, &err);

	for (size_t i = 0; i < n; i++)
		vrl_value_unref(argv[i]);
	free(argv);

	if (st == VRL_OK) {
		*out = result ? result : vrl_null();
		return VRL_OK;
	}

	/* function errored */
	if (node->u.call.fallible) {
		/* `f!()` : abort program on error */
		ctx->aborted = 1;
		free(ctx->abort_msg);
		ctx->abort_msg = err ? err : vrl_errf("function '%s' failed", node->u.call.name);
		return VRL_ABORT;
	}
	ctx_set_error(ctx, err ? err : vrl_errf("function '%s' failed", node->u.call.name));
	return VRL_ERR;
}

/* ------------------------------------------------------------------ */
/* eval                                                                */
/* ------------------------------------------------------------------ */

static vrl_status eval_block(vrl_ctx *ctx, vrl_ast *blk, vrl_value **out)
{
	vrl_value *last = NULL;
	for (size_t i = 0; i < blk->u.block.n; i++) {
		vrl_value_unref(last);
		last = NULL;
		vrl_status st = eval(ctx, blk->u.block.stmts[i], &last);
		if (st != VRL_OK) {
			vrl_value_unref(last);
			return st;
		}
	}
	*out = last ? last : vrl_null();
	return VRL_OK;
}

static vrl_status eval(vrl_ctx *ctx, vrl_ast *a, vrl_value **out)
{
	*out = NULL;
	switch (a->kind) {
	case AST_LITERAL:
		if (a->u.literal->type == VRL_ARRAY || a->u.literal->type == VRL_OBJECT)
			*out = vrl_value_clone(a->u.literal);
		else
			*out = vrl_value_ref(a->u.literal);
		return VRL_OK;

	case AST_EVENT_ROOT:
		*out = vrl_value_ref(ctx->event);
		return VRL_OK;

	case AST_IDENT: {
		vrl_value *v = var_get(ctx, a->u.ident.name, a->u.ident.name_len);
		*out = v ? vrl_value_ref(v) : vrl_null();
		return VRL_OK;
	}

	case AST_FIELD: {
		vrl_value *tv = NULL;
		vrl_status st = eval(ctx, a->u.field.target, &tv);
		if (st != VRL_OK)
			return st;
		if (tv->type == VRL_OBJECT) {
			vrl_value *c = vrl_object_get(tv, a->u.field.name, a->u.field.name_len);
			*out = c ? vrl_value_ref(c) : vrl_null();
		} else {
			*out = vrl_null();
		}
		vrl_value_unref(tv);
		return VRL_OK;
	}

	case AST_INDEX: {
		vrl_value *tv = NULL, *iv = NULL;
		vrl_status st = eval(ctx, a->u.index.target, &tv);
		if (st != VRL_OK)
			return st;
		st = eval(ctx, a->u.index.index, &iv);
		if (st != VRL_OK) {
			vrl_value_unref(tv);
			return st;
		}
		*out = vrl_null();
		if (iv->type == VRL_INTEGER && tv->type == VRL_ARRAY) {
			int64_t i = iv->u.integer;
			size_t len = vrl_array_len(tv);
			if (i < 0)
				i += (int64_t)len;
			if (i >= 0 && (size_t)i < len) {
				vrl_value_unref(*out);
				*out = vrl_value_ref(vrl_array_get(tv, (size_t)i));
			}
		} else if (iv->type == VRL_BYTES && tv->type == VRL_OBJECT) {
			vrl_value *c = vrl_object_get(tv, iv->u.bytes.data, iv->u.bytes.len);
			if (c) {
				vrl_value_unref(*out);
				*out = vrl_value_ref(c);
			}
		}
		vrl_value_unref(tv);
		vrl_value_unref(iv);
		return VRL_OK;
	}

	case AST_ARRAY: {
		vrl_value *arr = vrl_array_new();
		for (size_t i = 0; i < a->u.array.n; i++) {
			vrl_value *item = NULL;
			vrl_status st = eval(ctx, a->u.array.items[i], &item);
			if (st != VRL_OK) {
				vrl_value_unref(arr);
				return st;
			}
			vrl_array_push(arr, item);
		}
		*out = arr;
		return VRL_OK;
	}

	case AST_OBJECT: {
		vrl_value *obj = vrl_object_new();
		for (size_t i = 0; i < a->u.object.n; i++) {
			vrl_value *k = NULL, *v = NULL;
			vrl_status st = eval(ctx, a->u.object.pairs[i].key, &k);
			if (st != VRL_OK) {
				vrl_value_unref(obj);
				return st;
			}
			if (k->type != VRL_BYTES) {
				vrl_value_unref(k);
				vrl_value_unref(obj);
				ctx_set_error(ctx, vrl_errf("object key must be a string"));
				return VRL_ERR;
			}
			st = eval(ctx, a->u.object.pairs[i].val, &v);
			if (st != VRL_OK) {
				vrl_value_unref(k);
				vrl_value_unref(obj);
				return st;
			}
			vrl_object_set(obj, k->u.bytes.data, k->u.bytes.len, v);
			vrl_value_unref(k);
		}
		*out = obj;
		return VRL_OK;
	}

	case AST_UNARY: {
		vrl_value *v = NULL;
		vrl_status st = eval(ctx, a->u.unary.operand, &v);
		if (st != VRL_OK)
			return st;
		if (a->u.unary.op == UOP_NOT) {
			*out = vrl_boolean(!vrl_value_truthy(v));
		} else { /* UOP_NEG */
			if (v->type == VRL_INTEGER)
				*out = vrl_integer(-v->u.integer);
			else if (v->type == VRL_FLOAT)
				*out = vrl_float(-v->u.flt);
			else {
				vrl_value_unref(v);
				ctx_set_error(ctx, vrl_errf("cannot negate %s", vrl_type_name(v->type)));
				return VRL_ERR;
			}
		}
		vrl_value_unref(v);
		return VRL_OK;
	}

	case AST_BINARY: {
		vrl_binop op = a->u.binary.op;
		/* error coalescing: catch errors from the left side */
		if (op == OP_COALESCE) {
			vrl_value *l = NULL;
			vrl_status st = eval(ctx, a->u.binary.left, &l);
			if (st == VRL_ABORT)
				return st;
			if (st == VRL_ERR) {
				free(ctx->error);
				ctx->error = NULL;
				return eval(ctx, a->u.binary.right, out);
			}
			/* also fall through to right if left is null */
			if (l->type == VRL_NULL) {
				vrl_value_unref(l);
				return eval(ctx, a->u.binary.right, out);
			}
			*out = l;
			return VRL_OK;
		}
		/* logical short-circuit */
		if (op == OP_AND || op == OP_OR) {
			vrl_value *l = NULL;
			vrl_status st = eval(ctx, a->u.binary.left, &l);
			if (st != VRL_OK)
				return st;
			int lt = vrl_value_truthy(l);
			vrl_value_unref(l);
			if (op == OP_AND && !lt) {
				*out = vrl_boolean(0);
				return VRL_OK;
			}
			if (op == OP_OR && lt) {
				*out = vrl_boolean(1);
				return VRL_OK;
			}
			vrl_value *r = NULL;
			st = eval(ctx, a->u.binary.right, &r);
			if (st != VRL_OK)
				return st;
			*out = vrl_boolean(vrl_value_truthy(r));
			vrl_value_unref(r);
			return VRL_OK;
		}

		vrl_value *l = NULL, *r = NULL;
		vrl_status st = eval(ctx, a->u.binary.left, &l);
		if (st != VRL_OK)
			return st;
		st = eval(ctx, a->u.binary.right, &r);
		if (st != VRL_OK) {
			vrl_value_unref(l);
			return st;
		}
		switch (op) {
		case OP_EQ: *out = vrl_boolean(vrl_value_equal(l, r)); st = VRL_OK; break;
		case OP_NE: *out = vrl_boolean(!vrl_value_equal(l, r)); st = VRL_OK; break;
		case OP_LT: case OP_LE: case OP_GT: case OP_GE:
			st = binop_compare(ctx, op, l, r, out); break;
		default:
			st = binop_arith(ctx, op, l, r, out); break;
		}
		vrl_value_unref(l);
		vrl_value_unref(r);
		return st;
	}

	case AST_ASSIGN: {
		vrl_value *v = NULL;
		vrl_status st = eval(ctx, a->u.assign.value, &v);
		if (a->u.assign.err_target) {
			/* value, err = expr : catch recoverable errors */
			if (st == VRL_ABORT)
				return st;
			if (st == VRL_ERR) {
				char *msg = ctx->error ? ctx->error : vrl_errf("error");
				ctx->error = NULL;
				set_path(ctx, a->u.assign.target, vrl_null());
				set_path(ctx, a->u.assign.err_target, vrl_bytes_cstr(msg));
				free(msg);
				*out = vrl_null();
				return VRL_OK;
			}
			*out = vrl_value_ref(v);
			set_path(ctx, a->u.assign.target, v);
			set_path(ctx, a->u.assign.err_target, vrl_null());
			return VRL_OK;
		}
		if (st != VRL_OK)
			return st;
		*out = vrl_value_ref(v);
		return set_path(ctx, a->u.assign.target, v);
	}

	case AST_CALL:
		return eval_call(ctx, a, out);

	case AST_IF: {
		vrl_value *c = NULL;
		vrl_status st = eval(ctx, a->u.iff.cond, &c);
		if (st != VRL_OK)
			return st;
		int t = vrl_value_truthy(c);
		vrl_value_unref(c);
		if (t)
			return eval(ctx, a->u.iff.then_blk, out);
		if (a->u.iff.else_blk)
			return eval(ctx, a->u.iff.else_blk, out);
		*out = vrl_null();
		return VRL_OK;
	}

	case AST_BLOCK:
		return eval_block(ctx, a, out);

	case AST_ABORT: {
		char *msg = NULL;
		if (a->u.abort.message) {
			vrl_value *m = NULL;
			if (eval(ctx, a->u.abort.message, &m) == VRL_OK) {
				msg = vrl_value_to_string(m, NULL);
				vrl_value_unref(m);
			}
		}
		ctx->aborted = 1;
		free(ctx->abort_msg);
		ctx->abort_msg = msg ? msg : vrl_errf("aborted");
		return VRL_ABORT;
	}

	case AST_CLOSURE:
		ctx_set_error(ctx, vrl_errf("closure used outside of a function call"));
		return VRL_ERR;
	}
	ctx_set_error(ctx, vrl_errf("unhandled ast node"));
	return VRL_ERR;
}

vrl_status vrl_eval(vrl_ctx *ctx, vrl_ast *node, vrl_value **out)
{
	return eval(ctx, node, out);
}

vrl_status vrl_invoke_closure(vrl_ctx *ctx, vrl_ast *closure,
			      vrl_value **params, size_t nparams,
			      vrl_value **out, char **err)
{
	if (!closure || closure->kind != AST_CLOSURE) {
		if (err)
			*err = vrl_errf("expected a closure");
		return VRL_ERR;
	}
	size_t np = closure->u.closure.n;
	/* save previous bindings so closures don't leak scope */
	vrl_value **saved = np ? calloc(np, sizeof(vrl_value *)) : NULL;
	for (size_t i = 0; i < np; i++) {
		char *pn = closure->u.closure.params[i];
		vrl_value *prev = var_get(ctx, pn, strlen(pn));
		saved[i] = prev ? vrl_value_ref(prev) : NULL;
		vrl_value *bind = (i < nparams && params[i]) ? vrl_value_ref(params[i]) : vrl_null();
		var_set(ctx, pn, strlen(pn), bind);
	}
	vrl_status st = eval(ctx, closure->u.closure.body, out);
	for (size_t i = 0; i < np; i++) {
		char *pn = closure->u.closure.params[i];
		if (saved[i])
			var_set(ctx, pn, strlen(pn), saved[i]);
		else
			vrl_object_del(ctx->vars, pn, strlen(pn));
	}
	free(saved);
	if (st == VRL_ERR && err) {
		*err = ctx->error;
		ctx->error = NULL;
	}
	return st;
}

vrl_status vrl_exec(vrl_ctx *ctx, vrl_ast *program)
{
	if (!program)
		return VRL_OK;
	vrl_value *result = NULL;
	vrl_status st = eval(ctx, program, &result);
	vrl_value_unref(result);
	return st;
}
