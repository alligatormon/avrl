#pragma once
#include <stdint.h>
#include <stddef.h>
#include "value.h"

typedef enum {
	AST_LITERAL,     /* constant vrl_value */
	AST_ARRAY,       /* [ e, e, ... ] */
	AST_OBJECT,      /* { "k": e, ... } */
	AST_EVENT_ROOT,  /* . (whole event) */
	AST_IDENT,       /* variable reference */
	AST_FIELD,       /* target.name */
	AST_INDEX,       /* target[index] */
	AST_ASSIGN,      /* target = value ; or target, err = value */
	AST_UNARY,
	AST_BINARY,
	AST_CALL,        /* fn(args...) with optional ! and closure */
	AST_IF,          /* if cond { } else { } */
	AST_BLOCK,       /* { stmt; stmt; expr } */
	AST_ABORT,       /* abort [msg] */
	AST_CLOSURE,     /* -> |a, b| { body } */
} vrl_ast_kind;

typedef enum {
	OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
	OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
	OP_AND, OP_OR, OP_COALESCE,
} vrl_binop;

typedef enum {
	UOP_NOT, UOP_NEG,
} vrl_unop;

typedef struct vrl_ast vrl_ast;

typedef struct vrl_ast_pair {
	vrl_ast *key;   /* expression evaluating to a string */
	vrl_ast *val;
} vrl_ast_pair;

struct vrl_ast {
	vrl_ast_kind kind;
	uint32_t line;
	union {
		vrl_value *literal;
		struct { vrl_ast **items; size_t n; } array;
		struct { vrl_ast_pair *pairs; size_t n; } object;
		struct { char *name; size_t name_len; } ident;
		struct { vrl_ast *target; char *name; size_t name_len; } field;
		struct { vrl_ast *target; vrl_ast *index; } index;
		struct { vrl_ast *target; vrl_ast *err_target; vrl_ast *value; } assign;
		struct { vrl_unop op; vrl_ast *operand; } unary;
		struct { vrl_binop op; vrl_ast *left; vrl_ast *right; } binary;
		struct {
			char *name;
			size_t name_len;
			vrl_ast **args;
			char **arg_names;      /* parallel; NULL entry = positional */
			size_t n;
			int fallible;          /* set by trailing ! */
			vrl_ast *closure;      /* AST_CLOSURE or NULL */
		} call;
		struct { vrl_ast *cond; vrl_ast *then_blk; vrl_ast *else_blk; } iff;
		struct { vrl_ast **stmts; size_t n; } block;
		struct { vrl_ast *message; } abort;
		struct { char **params; size_t n; vrl_ast *body; } closure;
	} u;
};

void vrl_ast_free(vrl_ast *a);
