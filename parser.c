#define _GNU_SOURCE
#include "parser.h"
#include "lexer.h"
#include "pcre_wrap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct {
	vrl_token *toks;
	size_t n;
	size_t pos;
	char *err;
	uint32_t err_line;
	avrl_log_level ll;
} parser;

/* ------------------------------------------------------------------ */
/* ast helpers                                                         */
/* ------------------------------------------------------------------ */

static vrl_ast *node_new(vrl_ast_kind kind, uint32_t line)
{
	vrl_ast *a = calloc(1, sizeof(*a));
	if (a) {
		a->kind = kind;
		a->line = line;
	}
	return a;
}

void vrl_ast_free(vrl_ast *a)
{
	if (!a)
		return;
	switch (a->kind) {
	case AST_LITERAL:
		vrl_value_unref(a->u.literal);
		break;
	case AST_ARRAY:
		for (size_t i = 0; i < a->u.array.n; i++)
			vrl_ast_free(a->u.array.items[i]);
		free(a->u.array.items);
		break;
	case AST_OBJECT:
		for (size_t i = 0; i < a->u.object.n; i++) {
			vrl_ast_free(a->u.object.pairs[i].key);
			vrl_ast_free(a->u.object.pairs[i].val);
		}
		free(a->u.object.pairs);
		break;
	case AST_IDENT:
		free(a->u.ident.name);
		break;
	case AST_FIELD:
		vrl_ast_free(a->u.field.target);
		free(a->u.field.name);
		break;
	case AST_INDEX:
		vrl_ast_free(a->u.index.target);
		vrl_ast_free(a->u.index.index);
		break;
	case AST_ASSIGN:
		vrl_ast_free(a->u.assign.target);
		vrl_ast_free(a->u.assign.err_target);
		vrl_ast_free(a->u.assign.value);
		break;
	case AST_UNARY:
		vrl_ast_free(a->u.unary.operand);
		break;
	case AST_BINARY:
		vrl_ast_free(a->u.binary.left);
		vrl_ast_free(a->u.binary.right);
		break;
	case AST_CALL:
		free(a->u.call.name);
		for (size_t i = 0; i < a->u.call.n; i++) {
			vrl_ast_free(a->u.call.args[i]);
			if (a->u.call.arg_names)
				free(a->u.call.arg_names[i]);
		}
		free(a->u.call.args);
		free(a->u.call.arg_names);
		vrl_ast_free(a->u.call.closure);
		break;
	case AST_IF:
		vrl_ast_free(a->u.iff.cond);
		vrl_ast_free(a->u.iff.then_blk);
		vrl_ast_free(a->u.iff.else_blk);
		break;
	case AST_BLOCK:
		for (size_t i = 0; i < a->u.block.n; i++)
			vrl_ast_free(a->u.block.stmts[i]);
		free(a->u.block.stmts);
		break;
	case AST_ABORT:
		vrl_ast_free(a->u.abort.message);
		break;
	case AST_CLOSURE:
		for (size_t i = 0; i < a->u.closure.n; i++)
			free(a->u.closure.params[i]);
		free(a->u.closure.params);
		vrl_ast_free(a->u.closure.body);
		break;
	case AST_EVENT_ROOT:
		break;
	}
	free(a);
}

/* ------------------------------------------------------------------ */
/* parser primitives                                                   */
/* ------------------------------------------------------------------ */

static vrl_token *cur(parser *p) { return &p->toks[p->pos]; }
static vrl_token *peekn(parser *p, size_t k)
{
	size_t i = p->pos + k;
	if (i >= p->n)
		i = p->n - 1; /* EOF token */
	return &p->toks[i];
}
static int at(parser *p, vrl_token_type t) { return cur(p)->type == t; }

static void perror_at(parser *p, uint32_t line, const char *msg)
{
	if (p->err)
		return;
	char buf[256];
	snprintf(buf, sizeof(buf), "line %u: %s", line, msg);
	p->err = strdup(buf);
	p->err_line = line;
}

static void perr(parser *p, const char *msg)
{
	perror_at(p, cur(p)->line, msg);
}

static vrl_token *advance(parser *p)
{
	vrl_token *t = cur(p);
	if (p->pos + 1 < p->n)
		p->pos++;
	return t;
}

static int expect(parser *p, vrl_token_type t, const char *msg)
{
	if (at(p, t)) {
		advance(p);
		return 1;
	}
	perr(p, msg);
	return 0;
}

static void skip_newlines(parser *p)
{
	while (at(p, TK_NEWLINE))
		advance(p);
}

static void skip_sep(parser *p)
{
	while (at(p, TK_NEWLINE) || at(p, TK_SEMICOLON))
		advance(p);
}

/* index of the k-th token from pos, skipping newlines */
static vrl_token *peek_skip_nl(parser *p, size_t k)
{
	size_t i = p->pos;
	size_t seen = 0;
	while (i < p->n) {
		if (p->toks[i].type != TK_NEWLINE) {
			if (seen == k)
				return &p->toks[i];
			seen++;
		}
		i++;
	}
	return &p->toks[p->n - 1];
}

/* forward decls */
static vrl_ast *parse_statement(parser *p);
static vrl_ast *parse_expr(parser *p);
static vrl_ast *parse_binary(parser *p, int min_prec);
static vrl_ast *parse_unary(parser *p);
static vrl_ast *parse_postfix(parser *p);
static vrl_ast *parse_primary(parser *p);
static vrl_ast *parse_block(parser *p);
static vrl_ast *parse_if(parser *p);

/* ------------------------------------------------------------------ */
/* timestamp parsing                                                   */
/* ------------------------------------------------------------------ */

int vrl_parse_timestamp_str(const char *s, size_t len, double *out)
{
	char buf[64];
	if (len >= sizeof(buf))
		return -1;
	memcpy(buf, s, len);
	buf[len] = '\0';

	struct tm tmv;
	memset(&tmv, 0, sizeof(tmv));
	double frac = 0.0;
	const char *fmts[] = {
		"%Y-%m-%dT%H:%M:%S",
		"%Y-%m-%d %H:%M:%S",
		"%Y-%m-%dT%H:%M:%SZ",
		"%Y-%m-%d",
		NULL,
	};
	for (int i = 0; fmts[i]; i++) {
		memset(&tmv, 0, sizeof(tmv));
		char *end = strptime(buf, fmts[i], &tmv);
		if (end) {
			/* optional fractional seconds and Z / offset ignored (assume UTC) */
			if (*end == '.') {
				frac = strtod(end, NULL);
				/* frac like .123 -> parsed as 0.123 by strtod on ".123" */
			}
#if defined(__APPLE__) || defined(__linux__)
			time_t t = timegm(&tmv);
#else
			time_t t = mktime(&tmv);
#endif
			*out = (double)t + frac;
			return 0;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* precedence                                                          */
/* ------------------------------------------------------------------ */

static int binop_info(vrl_token_type t, vrl_binop *op)
{
	switch (t) {
	case TK_COALESCE: *op = OP_COALESCE; return 1;
	case TK_OR:       *op = OP_OR;       return 2;
	case TK_AND:      *op = OP_AND;      return 3;
	case TK_EQ:       *op = OP_EQ;       return 4;
	case TK_NE:       *op = OP_NE;       return 4;
	case TK_LT:       *op = OP_LT;       return 5;
	case TK_LE:       *op = OP_LE;       return 5;
	case TK_GT:       *op = OP_GT;       return 5;
	case TK_GE:       *op = OP_GE;       return 5;
	case TK_PLUS:     *op = OP_ADD;      return 6;
	case TK_MINUS:    *op = OP_SUB;      return 6;
	case TK_STAR:     *op = OP_MUL;      return 7;
	case TK_SLASH:    *op = OP_DIV;      return 7;
	case TK_PERCENT:  *op = OP_MOD;      return 7;
	default: return 0;
	}
}

/* ------------------------------------------------------------------ */
/* expressions                                                         */
/* ------------------------------------------------------------------ */

static vrl_ast *parse_expr(parser *p)
{
	return parse_binary(p, 1);
}

static vrl_ast *parse_binary(parser *p, int min_prec)
{
	vrl_ast *left = parse_unary(p);
	if (!left)
		return NULL;
	for (;;) {
		vrl_binop op;
		int prec = binop_info(cur(p)->type, &op);
		if (prec == 0 || prec < min_prec)
			break;
		uint32_t line = cur(p)->line;
		advance(p);
		skip_newlines(p); /* allow line continuation after an operator */
		vrl_ast *right = parse_binary(p, prec + 1);
		if (!right) {
			vrl_ast_free(left);
			return NULL;
		}
		vrl_ast *n = node_new(AST_BINARY, line);
		n->u.binary.op = op;
		n->u.binary.left = left;
		n->u.binary.right = right;
		left = n;
	}
	return left;
}

static vrl_ast *parse_unary(parser *p)
{
	if (at(p, TK_BANG) || at(p, TK_MINUS)) {
		uint32_t line = cur(p)->line;
		vrl_unop op = at(p, TK_BANG) ? UOP_NOT : UOP_NEG;
		advance(p);
		skip_newlines(p);
		vrl_ast *operand = parse_unary(p);
		if (!operand)
			return NULL;
		vrl_ast *n = node_new(AST_UNARY, line);
		n->u.unary.op = op;
		n->u.unary.operand = operand;
		return n;
	}
	return parse_postfix(p);
}

static vrl_ast *parse_closure(parser *p)
{
	/* '->' already consumed. expect | params | { body } */
	uint32_t line = cur(p)->line;
	if (!expect(p, TK_PIPE, "expected '|' to start closure params"))
		return NULL;
	char **params = NULL;
	size_t n = 0, cap = 0;
	while (!at(p, TK_PIPE) && !at(p, TK_EOF)) {
		if (!at(p, TK_IDENT)) {
			perr(p, "expected closure parameter name");
			goto fail;
		}
		if (n >= cap) {
			cap = cap ? cap * 2 : 4;
			params = realloc(params, cap * sizeof(*params));
		}
		params[n++] = strndup(cur(p)->start, cur(p)->len);
		advance(p);
		if (at(p, TK_COMMA))
			advance(p);
		else
			break;
	}
	if (!expect(p, TK_PIPE, "expected '|' to close closure params"))
		goto fail;
	vrl_ast *body = parse_block(p);
	if (!body)
		goto fail;
	vrl_ast *n_ast = node_new(AST_CLOSURE, line);
	n_ast->u.closure.params = params;
	n_ast->u.closure.n = n;
	n_ast->u.closure.body = body;
	return n_ast;
fail:
	for (size_t i = 0; i < n; i++)
		free(params[i]);
	free(params);
	return NULL;
}

static vrl_ast *parse_postfix(parser *p)
{
	vrl_ast *node = parse_primary(p);
	if (!node)
		return NULL;
	for (;;) {
		if (at(p, TK_DOT)) {
			uint32_t line = cur(p)->line;
			advance(p);
			if (at(p, TK_IDENT) || at(p, TK_STRING)) {
				vrl_ast *f = node_new(AST_FIELD, line);
				f->u.field.target = node;
				if (at(p, TK_STRING)) {
					f->u.field.name = strndup(cur(p)->text, cur(p)->text_len);
					f->u.field.name_len = cur(p)->text_len;
				} else {
					f->u.field.name = strndup(cur(p)->start, cur(p)->len);
					f->u.field.name_len = cur(p)->len;
				}
				advance(p);
				node = f;
			} else {
				perr(p, "expected field name after '.'");
				vrl_ast_free(node);
				return NULL;
			}
		} else if (at(p, TK_LBRACKET)) {
			uint32_t line = cur(p)->line;
			advance(p);
			vrl_ast *idx = parse_expr(p);
			if (!idx || !expect(p, TK_RBRACKET, "expected ']'")) {
				vrl_ast_free(idx);
				vrl_ast_free(node);
				return NULL;
			}
			vrl_ast *ix = node_new(AST_INDEX, line);
			ix->u.index.target = node;
			ix->u.index.index = idx;
			node = ix;
		} else if (at(p, TK_ARROW)) {
			advance(p);
			if (node->kind != AST_CALL || node->u.call.closure) {
				perr(p, "closure '->' can only follow a function call");
				vrl_ast_free(node);
				return NULL;
			}
			vrl_ast *cl = parse_closure(p);
			if (!cl) {
				vrl_ast_free(node);
				return NULL;
			}
			node->u.call.closure = cl;
		} else {
			break;
		}
	}
	return node;
}

static vrl_ast *parse_array(parser *p)
{
	uint32_t line = cur(p)->line;
	advance(p); /* [ */
	vrl_ast *n = node_new(AST_ARRAY, line);
	size_t cap = 0;
	while (!at(p, TK_RBRACKET) && !at(p, TK_EOF)) {
		vrl_ast *item = parse_expr(p);
		if (!item) {
			vrl_ast_free(n);
			return NULL;
		}
		if (n->u.array.n >= cap) {
			cap = cap ? cap * 2 : 4;
			n->u.array.items = realloc(n->u.array.items, cap * sizeof(vrl_ast *));
		}
		n->u.array.items[n->u.array.n++] = item;
		if (at(p, TK_COMMA))
			advance(p);
		else
			break;
	}
	if (!expect(p, TK_RBRACKET, "expected ']' to close array")) {
		vrl_ast_free(n);
		return NULL;
	}
	return n;
}

static int object_ahead(parser *p)
{
	/* cur is '{' ; look past any newlines */
	vrl_token *t1 = peek_skip_nl(p, 1);
	if (t1->type == TK_RBRACE)
		return 1; /* {} empty object */
	if (t1->type == TK_STRING && peek_skip_nl(p, 2)->type == TK_COLON)
		return 1;
	return 0;
}

static vrl_ast *parse_object(parser *p)
{
	uint32_t line = cur(p)->line;
	advance(p); /* { */
	vrl_ast *n = node_new(AST_OBJECT, line);
	size_t cap = 0;
	skip_newlines(p);
	while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
		vrl_ast *key = parse_expr(p);
		skip_newlines(p);
		if (!key || !expect(p, TK_COLON, "expected ':' in object literal")) {
			vrl_ast_free(key);
			vrl_ast_free(n);
			return NULL;
		}
		skip_newlines(p);
		vrl_ast *val = parse_expr(p);
		if (!val) {
			vrl_ast_free(key);
			vrl_ast_free(n);
			return NULL;
		}
		if (n->u.object.n >= cap) {
			cap = cap ? cap * 2 : 4;
			n->u.object.pairs = realloc(n->u.object.pairs, cap * sizeof(vrl_ast_pair));
		}
		n->u.object.pairs[n->u.object.n].key = key;
		n->u.object.pairs[n->u.object.n].val = val;
		n->u.object.n++;
		skip_newlines(p);
		if (at(p, TK_COMMA)) {
			advance(p);
			skip_newlines(p);
		} else {
			break;
		}
	}
	if (!expect(p, TK_RBRACE, "expected '}' to close object")) {
		vrl_ast_free(n);
		return NULL;
	}
	return n;
}

static vrl_ast *parse_call(parser *p, char *name, size_t name_len, int fallible, uint32_t line)
{
	advance(p); /* ( */
	vrl_ast *n = node_new(AST_CALL, line);
	n->u.call.name = name;
	n->u.call.name_len = name_len;
	n->u.call.fallible = fallible;
	size_t cap = 0;
	while (!at(p, TK_RPAREN) && !at(p, TK_EOF)) {
		char *argname = NULL;
		/* named arg: IDENT ':' expr */
		if (at(p, TK_IDENT) && peekn(p, 1)->type == TK_COLON) {
			argname = strndup(cur(p)->start, cur(p)->len);
			advance(p); /* ident */
			advance(p); /* : */
		}
		vrl_ast *arg = parse_expr(p);
		if (!arg) {
			free(argname);
			vrl_ast_free(n);
			return NULL;
		}
		if (n->u.call.n >= cap) {
			cap = cap ? cap * 2 : 4;
			n->u.call.args = realloc(n->u.call.args, cap * sizeof(vrl_ast *));
			n->u.call.arg_names = realloc(n->u.call.arg_names, cap * sizeof(char *));
		}
		n->u.call.args[n->u.call.n] = arg;
		n->u.call.arg_names[n->u.call.n] = argname;
		n->u.call.n++;
		if (at(p, TK_COMMA))
			advance(p);
		else
			break;
	}
	if (!expect(p, TK_RPAREN, "expected ')' to close call")) {
		vrl_ast_free(n);
		return NULL;
	}
	return n;
}

static vrl_ast *parse_primary(parser *p)
{
	vrl_token *t = cur(p);
	uint32_t line = t->line;
	switch (t->type) {
	case TK_INT: {
		advance(p);
		vrl_ast *n = node_new(AST_LITERAL, line);
		n->u.literal = vrl_integer(t->ival);
		return n;
	}
	case TK_FLOAT: {
		advance(p);
		vrl_ast *n = node_new(AST_LITERAL, line);
		n->u.literal = vrl_float(t->fval);
		return n;
	}
	case TK_STRING: {
		advance(p);
		vrl_ast *n = node_new(AST_LITERAL, line);
		n->u.literal = vrl_bytes(t->text, t->text_len);
		return n;
	}
	case TK_TRUE:
	case TK_FALSE: {
		advance(p);
		vrl_ast *n = node_new(AST_LITERAL, line);
		n->u.literal = vrl_boolean(t->type == TK_TRUE);
		return n;
	}
	case TK_NULL: {
		advance(p);
		vrl_ast *n = node_new(AST_LITERAL, line);
		n->u.literal = vrl_null();
		return n;
	}
	case TK_REGEX: {
		advance(p);
		regex_match *re = avrl_regex_compile(t->text);
		if (!re) {
			perror_at(p, line, "invalid regex literal");
			return NULL;
		}
		vrl_ast *n = node_new(AST_LITERAL, line);
		n->u.literal = vrl_regex_take(re);
		return n;
	}
	case TK_TIMESTAMP: {
		advance(p);
		double ts;
		if (vrl_parse_timestamp_str(t->text, t->text_len, &ts) != 0) {
			perror_at(p, line, "invalid timestamp literal");
			return NULL;
		}
		vrl_ast *n = node_new(AST_LITERAL, line);
		n->u.literal = vrl_timestamp(ts);
		return n;
	}
	case TK_IDENT: {
		char *name = strndup(t->start, t->len);
		size_t name_len = t->len;
		advance(p);
		int fallible = 0;
		if (at(p, TK_BANG) && peekn(p, 1)->type == TK_LPAREN) {
			fallible = 1;
			advance(p); /* ! */
		}
		if (at(p, TK_LPAREN))
			return parse_call(p, name, name_len, fallible, line);
		if (fallible) {
			perror_at(p, line, "'!' is only valid on function calls");
			free(name);
			return NULL;
		}
		vrl_ast *n = node_new(AST_IDENT, line);
		n->u.ident.name = name;
		n->u.ident.name_len = name_len;
		return n;
	}
	case TK_DOT: {
		advance(p);
		if (at(p, TK_IDENT) || at(p, TK_STRING)) {
			vrl_ast *root = node_new(AST_EVENT_ROOT, line);
			vrl_ast *f = node_new(AST_FIELD, line);
			f->u.field.target = root;
			if (at(p, TK_STRING)) {
				f->u.field.name = strndup(cur(p)->text, cur(p)->text_len);
				f->u.field.name_len = cur(p)->text_len;
			} else {
				f->u.field.name = strndup(cur(p)->start, cur(p)->len);
				f->u.field.name_len = cur(p)->len;
			}
			advance(p);
			return f;
		}
		if (at(p, TK_LBRACKET)) {
			advance(p);
			vrl_ast *idx = parse_expr(p);
			if (!idx || !expect(p, TK_RBRACKET, "expected ']'")) {
				vrl_ast_free(idx);
				return NULL;
			}
			vrl_ast *root = node_new(AST_EVENT_ROOT, line);
			vrl_ast *ix = node_new(AST_INDEX, line);
			ix->u.index.target = root;
			ix->u.index.index = idx;
			return ix;
		}
		return node_new(AST_EVENT_ROOT, line);
	}
	case TK_LPAREN: {
		advance(p);
		vrl_ast *e = parse_expr(p);
		if (!e || !expect(p, TK_RPAREN, "expected ')'")) {
			vrl_ast_free(e);
			return NULL;
		}
		return e;
	}
	case TK_LBRACKET:
		return parse_array(p);
	case TK_LBRACE:
		if (object_ahead(p))
			return parse_object(p);
		return parse_block(p);
	case TK_IF:
		return parse_if(p);
	case TK_ABORT: {
		advance(p);
		vrl_ast *n = node_new(AST_ABORT, line);
		/* optional message expression */
		if (!at(p, TK_SEMICOLON) && !at(p, TK_RBRACE) &&
		    !at(p, TK_NEWLINE) && !at(p, TK_EOF))
			n->u.abort.message = parse_expr(p);
		return n;
	}
	default:
		perr(p, "unexpected token in expression");
		return NULL;
	}
}

static vrl_ast *parse_block(parser *p)
{
	uint32_t line = cur(p)->line;
	if (!expect(p, TK_LBRACE, "expected '{'"))
		return NULL;
	vrl_ast *n = node_new(AST_BLOCK, line);
	size_t cap = 0;
	skip_sep(p);
	while (!at(p, TK_RBRACE) && !at(p, TK_EOF) && !p->err) {
		vrl_ast *stmt = parse_statement(p);
		if (!stmt) {
			vrl_ast_free(n);
			return NULL;
		}
		if (n->u.block.n >= cap) {
			cap = cap ? cap * 2 : 8;
			n->u.block.stmts = realloc(n->u.block.stmts, cap * sizeof(vrl_ast *));
		}
		n->u.block.stmts[n->u.block.n++] = stmt;
		skip_sep(p);
	}
	if (!expect(p, TK_RBRACE, "expected '}' to close block")) {
		vrl_ast_free(n);
		return NULL;
	}
	return n;
}

static vrl_ast *parse_if(parser *p)
{
	uint32_t line = cur(p)->line;
	advance(p); /* if */
	vrl_ast *cond = parse_expr(p);
	if (!cond)
		return NULL;
	vrl_ast *then_blk = parse_block(p);
	if (!then_blk) {
		vrl_ast_free(cond);
		return NULL;
	}
	vrl_ast *else_blk = NULL;
	/* tolerate a newline before 'else' */
	size_t save = p->pos;
	skip_newlines(p);
	if (!at(p, TK_ELSE))
		p->pos = save; /* the newline is a statement separator, keep it */
	if (at(p, TK_ELSE)) {
		advance(p);
		skip_newlines(p);
		if (at(p, TK_IF))
			else_blk = parse_if(p);
		else
			else_blk = parse_block(p);
		if (!else_blk) {
			vrl_ast_free(cond);
			vrl_ast_free(then_blk);
			return NULL;
		}
	}
	vrl_ast *n = node_new(AST_IF, line);
	n->u.iff.cond = cond;
	n->u.iff.then_blk = then_blk;
	n->u.iff.else_blk = else_blk;
	return n;
}

static int is_assignable(vrl_ast *a)
{
	return a && (a->kind == AST_IDENT || a->kind == AST_FIELD ||
		     a->kind == AST_INDEX || a->kind == AST_EVENT_ROOT);
}

static vrl_ast *parse_statement(parser *p)
{
	vrl_ast *left = parse_expr(p);
	if (!left)
		return NULL;

	if (at(p, TK_ASSIGN)) {
		if (!is_assignable(left)) {
			perr(p, "left-hand side of assignment is not assignable");
			vrl_ast_free(left);
			return NULL;
		}
		uint32_t line = cur(p)->line;
		advance(p);
		skip_newlines(p);
		vrl_ast *value = parse_statement(p); /* right-assoc; allows a = b = c */
		if (!value) {
			vrl_ast_free(left);
			return NULL;
		}
		vrl_ast *n = node_new(AST_ASSIGN, line);
		n->u.assign.target = left;
		n->u.assign.value = value;
		return n;
	}

	if (at(p, TK_COMMA)) {
		/* two-target assignment: value, err = fallible() */
		uint32_t line = cur(p)->line;
		advance(p);
		vrl_ast *err_target = parse_expr(p);
		if (!err_target) {
			vrl_ast_free(left);
			return NULL;
		}
		if (!is_assignable(left) || !is_assignable(err_target)) {
			perr(p, "assignment targets must be paths or variables");
			vrl_ast_free(left);
			vrl_ast_free(err_target);
			return NULL;
		}
		if (!expect(p, TK_ASSIGN, "expected '=' in 'value, err =' assignment")) {
			vrl_ast_free(left);
			vrl_ast_free(err_target);
			return NULL;
		}
		skip_newlines(p);
		vrl_ast *value = parse_statement(p);
		if (!value) {
			vrl_ast_free(left);
			vrl_ast_free(err_target);
			return NULL;
		}
		vrl_ast *n = node_new(AST_ASSIGN, line);
		n->u.assign.target = left;
		n->u.assign.err_target = err_target;
		n->u.assign.value = value;
		return n;
	}

	return left;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

vrl_program *vrl_parse(const char *src, size_t src_len, avrl_log_level ll)
{
	vrl_program *prog = calloc(1, sizeof(*prog));
	if (!prog)
		return NULL;

	vrl_token_stream *ts = vrl_lex(src, src_len, ll);
	if (!ts) {
		prog->err = strdup("lexer allocation failure");
		return prog;
	}
	if (ts->err) {
		prog->err = strdup(ts->err);
		prog->err_line = ts->err_line;
		vrl_token_stream_free(ts);
		return prog;
	}

	parser p = {.toks = ts->toks, .n = ts->len, .pos = 0, .ll = ll};

	vrl_ast *root = node_new(AST_BLOCK, 1);
	size_t cap = 0;
	skip_sep(&p);
	while (!at(&p, TK_EOF) && !p.err) {
		vrl_ast *stmt = parse_statement(&p);
		if (!stmt)
			break;
		if (root->u.block.n >= cap) {
			cap = cap ? cap * 2 : 16;
			root->u.block.stmts = realloc(root->u.block.stmts, cap * sizeof(vrl_ast *));
		}
		root->u.block.stmts[root->u.block.n++] = stmt;
		skip_sep(&p);
	}

	if (p.err) {
		prog->err = strdup(p.err);
		prog->err_line = p.err_line;
		free(p.err);
		vrl_ast_free(root);
		vrl_token_stream_free(ts);
		return prog;
	}

	prog->root = root;
	vrl_token_stream_free(ts);
	return prog;
}

void vrl_program_free(vrl_program *p)
{
	if (!p)
		return;
	vrl_ast_free(p->root);
	free(p->err);
	free(p);
}
