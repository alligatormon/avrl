#pragma once
#include <stddef.h>
#include <stdint.h>
#include "log.h"

typedef enum {
	TK_EOF = 0,
	TK_ERROR,
	TK_NEWLINE,   /* statement separator (depth 0 only) */

	/* literals */
	TK_INT,
	TK_FLOAT,
	TK_STRING,      /* "..." with escapes, or s'...' raw */
	TK_REGEX,       /* r'...' or r"..." (flags stored in text after NUL sep) */
	TK_TIMESTAMP,   /* t'...' */
	TK_TRUE,
	TK_FALSE,
	TK_NULL,

	TK_IDENT,

	/* punctuation */
	TK_DOT,         /* . */
	TK_LBRACKET,    /* [ */
	TK_RBRACKET,    /* ] */
	TK_LBRACE,      /* { */
	TK_RBRACE,      /* } */
	TK_LPAREN,      /* ( */
	TK_RPAREN,      /* ) */
	TK_COMMA,       /* , */
	TK_COLON,       /* : */
	TK_SEMICOLON,   /* ; */

	/* operators */
	TK_ASSIGN,      /* = */
	TK_PLUS,        /* + */
	TK_MINUS,       /* - */
	TK_STAR,        /* * */
	TK_SLASH,       /* / */
	TK_PERCENT,     /* % */
	TK_EQ,          /* == */
	TK_NE,          /* != */
	TK_LT,          /* < */
	TK_LE,          /* <= */
	TK_GT,          /* > */
	TK_GE,          /* >= */
	TK_AND,         /* && */
	TK_OR,          /* || */
	TK_BANG,        /* ! (negation, or fallible-call marker before '(') */
	TK_COALESCE,    /* ?? */
	TK_QUESTION,    /* ? */
	TK_PIPE,        /* | (closure param delimiter) */
	TK_ARROW,       /* -> (closure) */

	/* keywords */
	TK_IF,
	TK_ELSE,
	TK_ABORT,
} vrl_token_type;

typedef struct vrl_token {
	vrl_token_type type;
	const char *start;  /* pointer into source (not owned) */
	size_t len;
	/* decoded payload for string/regex/timestamp literals (owned, NUL-term) */
	char *text;
	size_t text_len;
	int64_t ival;
	double  fval;
	uint32_t line;
	uint32_t col;
} vrl_token;

typedef struct vrl_token_stream {
	vrl_token *toks;
	size_t len;
	size_t cap;
	char *err;      /* NULL on success (owned) */
	uint32_t err_line;
} vrl_token_stream;

vrl_token_stream *vrl_lex(const char *src, size_t src_len, avrl_log_level ll);
void vrl_token_stream_free(vrl_token_stream *ts);
const char *vrl_token_type_name(vrl_token_type t);
