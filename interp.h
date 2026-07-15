#pragma once
#include "ast.h"
#include "value.h"
#include "log.h"

typedef enum {
	VRL_OK = 0,   /* produced a value */
	VRL_ERR,      /* recoverable error (catchable by ?? or `, err =`) */
	VRL_ABORT,    /* abort / unhandled fatal: stop program */
} vrl_status;

typedef struct vrl_ctx {
	vrl_value *event;   /* current `.` (owned) */
	vrl_value *vars;    /* object of variables (owned) */
	char *error;        /* last recoverable error message (owned) */
	int aborted;
	char *abort_msg;    /* owned */
	avrl_log_level ll;
} vrl_ctx;

typedef struct vrl_call_args {
	vrl_ctx *ctx;
	vrl_value **args;   /* evaluated argument values (borrowed) */
	char **names;       /* parallel names; NULL entry = positional */
	size_t n;
	vrl_ast *closure;   /* closure AST or NULL */
} vrl_call_args;

/* stdlib function: returns status; on VRL_OK sets *out (owned),
 * on VRL_ERR sets *err (owned message). */
typedef vrl_status (*vrl_fn)(vrl_call_args *a, vrl_value **out, char **err);

vrl_ctx *vrl_ctx_new(avrl_log_level ll);
void vrl_ctx_free(vrl_ctx *ctx);
void vrl_ctx_set_event(vrl_ctx *ctx, vrl_value *event /* owned */);
void vrl_ctx_reset(vrl_ctx *ctx); /* clear vars/error between events */

/* Execute a parsed program (AST_BLOCK) against ctx->event. */
vrl_status vrl_exec(vrl_ctx *ctx, vrl_ast *program);

/* Evaluate a single expression (used by stdlib closures / tests). */
vrl_status vrl_eval(vrl_ctx *ctx, vrl_ast *node, vrl_value **out);

/* --- helpers for stdlib functions --- */
vrl_value *vrl_arg(vrl_call_args *a, const char *name, int idx); /* borrowed or NULL */
vrl_status vrl_invoke_closure(vrl_ctx *ctx, vrl_ast *closure,
			      vrl_value **params, size_t nparams,
			      vrl_value **out, char **err);
char *vrl_errf(const char *fmt, ...); /* allocate a formatted error string */

/* stdlib registry (implemented in stdlib.c) */
void vrl_stdlib_init(void);
vrl_fn vrl_stdlib_lookup(const char *name, size_t len);
/* Register (or override) a builtin. Name must be a static/long-lived string. */
void vrl_register(const char *name, vrl_fn fn);
