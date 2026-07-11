#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
	const char *src;
	size_t len;
	size_t pos;
	uint32_t line;
	uint32_t col;
	int depth;      /* () and [] nesting; newlines suppressed while >0 */
	vrl_token_stream *ts;
	avrl_log_level ll;
} lexer;

static void ts_push(vrl_token_stream *ts, vrl_token tok)
{
	if (ts->len >= ts->cap) {
		size_t cap = ts->cap ? ts->cap * 2 : 64;
		vrl_token *t = realloc(ts->toks, cap * sizeof(*t));
		if (!t)
			return;
		ts->toks = t;
		ts->cap = cap;
	}
	ts->toks[ts->len++] = tok;
}

static int lx_peek(lexer *lx)
{
	return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1;
}

static int lx_peek2(lexer *lx)
{
	return lx->pos + 1 < lx->len ? (unsigned char)lx->src[lx->pos + 1] : -1;
}

static int lx_adv(lexer *lx)
{
	if (lx->pos >= lx->len)
		return -1;
	int c = (unsigned char)lx->src[lx->pos++];
	if (c == '\n') {
		lx->line++;
		lx->col = 1;
	} else {
		lx->col++;
	}
	return c;
}

static void lx_error(lexer *lx, const char *msg)
{
	if (lx->ts->err)
		return;
	char buf[256];
	snprintf(buf, sizeof(buf), "line %u: %s", lx->line, msg);
	lx->ts->err = strdup(buf);
	lx->ts->err_line = lx->line;
}

/* Skip inline whitespace (not newlines) and comments. Returns 1 if a newline
 * boundary was crossed (so the caller can emit a separator token). */
static int skip_inline_ws(lexer *lx)
{
	int saw_newline = 0;
	for (;;) {
		int c = lx_peek(lx);
		if (c == ' ' || c == '\t' || c == '\r') {
			lx_adv(lx);
		} else if (c == '#') {
			while (lx_peek(lx) != '\n' && lx_peek(lx) != -1)
				lx_adv(lx);
		} else if (c == '\n') {
			saw_newline = 1;
			lx_adv(lx);
		} else {
			break;
		}
	}
	return saw_newline;
}

static int is_ident_start(int c)
{
	return c == '_' || isalpha(c);
}

static int is_ident_char(int c)
{
	return c == '_' || isalnum(c);
}

/* dynamic char buffer */
typedef struct {
	char *s;
	size_t len, cap;
} cbuf;

static void cbuf_push(cbuf *b, char c)
{
	if (b->len + 2 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 32;
		char *ns = realloc(b->s, cap);
		if (!ns)
			return;
		b->s = ns;
		b->cap = cap;
	}
	b->s[b->len++] = c;
	b->s[b->len] = '\0';
}

static void cbuf_push_utf8(cbuf *b, unsigned long cp)
{
	if (cp < 0x80) {
		cbuf_push(b, (char)cp);
	} else if (cp < 0x800) {
		cbuf_push(b, (char)(0xC0 | (cp >> 6)));
		cbuf_push(b, (char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		cbuf_push(b, (char)(0xE0 | (cp >> 12)));
		cbuf_push(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		cbuf_push(b, (char)(0x80 | (cp & 0x3F)));
	} else {
		cbuf_push(b, (char)(0xF0 | (cp >> 18)));
		cbuf_push(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
		cbuf_push(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		cbuf_push(b, (char)(0x80 | (cp & 0x3F)));
	}
}

/* scan a double-quoted string with escapes; opening quote already consumed */
static int scan_escaped_string(lexer *lx, cbuf *out)
{
	for (;;) {
		int c = lx_adv(lx);
		if (c == -1) {
			lx_error(lx, "unterminated string literal");
			return -1;
		}
		if (c == '"')
			return 0;
		if (c == '\\') {
			int e = lx_adv(lx);
			switch (e) {
			case 'n': cbuf_push(out, '\n'); break;
			case 't': cbuf_push(out, '\t'); break;
			case 'r': cbuf_push(out, '\r'); break;
			case '0': cbuf_push(out, '\0'); break;
			case '\\': cbuf_push(out, '\\'); break;
			case '"': cbuf_push(out, '"'); break;
			case '\'': cbuf_push(out, '\''); break;
			case 'u': {
				/* \u{XXXX} */
				if (lx_peek(lx) == '{') {
					lx_adv(lx);
					unsigned long cp = 0;
					int n = 0;
					while (isxdigit(lx_peek(lx))) {
						int d = lx_adv(lx);
						cp = cp * 16 + (isdigit(d) ? d - '0' : (tolower(d) - 'a' + 10));
						n++;
					}
					if (lx_peek(lx) == '}')
						lx_adv(lx);
					if (n == 0) {
						lx_error(lx, "invalid \\u escape");
						return -1;
					}
					cbuf_push_utf8(out, cp);
				} else {
					lx_error(lx, "invalid \\u escape (expected '{')");
					return -1;
				}
				break;
			}
			default:
				lx_error(lx, "invalid escape sequence");
				return -1;
			}
		} else {
			cbuf_push(out, (char)c);
		}
	}
}

/* scan a raw single-quoted string; opening quote already consumed.
 * In raw strings, '' is an escaped single quote. */
static int scan_raw_string(lexer *lx, cbuf *out, char quote)
{
	for (;;) {
		int c = lx_adv(lx);
		if (c == -1) {
			lx_error(lx, "unterminated raw string literal");
			return -1;
		}
		if (c == quote) {
			if (lx_peek(lx) == quote) {
				lx_adv(lx);
				cbuf_push(out, (char)quote);
				continue;
			}
			return 0;
		}
		cbuf_push(out, (char)c);
	}
}

static void emit_simple(lexer *lx, vrl_token_type type, const char *start,
			uint32_t line, uint32_t col)
{
	vrl_token tok = {0};
	tok.type = type;
	tok.start = start;
	tok.len = (size_t)(lx->src + lx->pos - start);
	tok.line = line;
	tok.col = col;
	ts_push(lx->ts, tok);
}

static void scan_number(lexer *lx, const char *start, uint32_t line, uint32_t col)
{
	cbuf b = {0};
	int is_float = 0;
	while (isdigit(lx_peek(lx)) || lx_peek(lx) == '_') {
		int c = lx_adv(lx);
		if (c != '_')
			cbuf_push(&b, (char)c);
	}
	if (lx_peek(lx) == '.' && isdigit(lx_peek2(lx))) {
		is_float = 1;
		cbuf_push(&b, (char)lx_adv(lx)); /* . */
		while (isdigit(lx_peek(lx)) || lx_peek(lx) == '_') {
			int c = lx_adv(lx);
			if (c != '_')
				cbuf_push(&b, (char)c);
		}
	}
	if (lx_peek(lx) == 'e' || lx_peek(lx) == 'E') {
		is_float = 1;
		cbuf_push(&b, (char)lx_adv(lx));
		if (lx_peek(lx) == '+' || lx_peek(lx) == '-')
			cbuf_push(&b, (char)lx_adv(lx));
		while (isdigit(lx_peek(lx)))
			cbuf_push(&b, (char)lx_adv(lx));
	}
	vrl_token tok = {0};
	tok.start = start;
	tok.len = (size_t)(lx->src + lx->pos - start);
	tok.line = line;
	tok.col = col;
	if (is_float) {
		tok.type = TK_FLOAT;
		tok.fval = strtod(b.s ? b.s : "0", NULL);
	} else {
		tok.type = TK_INT;
		tok.ival = (int64_t)strtoll(b.s ? b.s : "0", NULL, 10);
	}
	ts_push(lx->ts, tok);
	free(b.s);
}

static void scan_prefixed_literal(lexer *lx, char prefix, const char *start,
				  uint32_t line, uint32_t col)
{
	/* prefix already peeked; consume prefix + quote */
	lx_adv(lx); /* prefix char */
	int quote = lx_adv(lx); /* ' or " */
	cbuf b = {0};
	int rc;
	if (prefix == 'r' || prefix == 's' || prefix == 't') {
		/* r/s/t use raw scanning (no backslash escapes), except regex keeps
		 * backslashes verbatim which raw scanning already does. */
		rc = scan_raw_string(lx, &b, (char)quote);
	} else {
		rc = -1;
	}
	if (rc != 0) {
		free(b.s);
		vrl_token tok = {0};
		tok.type = TK_ERROR;
		tok.start = start;
		tok.line = line;
		tok.col = col;
		ts_push(lx->ts, tok);
		return;
	}
	vrl_token tok = {0};
	tok.start = start;
	tok.len = (size_t)(lx->src + lx->pos - start);
	tok.line = line;
	tok.col = col;
	tok.text = b.s ? b.s : strdup("");
	tok.text_len = b.len;
	if (prefix == 'r')
		tok.type = TK_REGEX;
	else if (prefix == 't')
		tok.type = TK_TIMESTAMP;
	else
		tok.type = TK_STRING; /* s'' raw string */
	ts_push(lx->ts, tok);
}

static void scan_ident_or_keyword(lexer *lx, const char *start, uint32_t line, uint32_t col)
{
	while (is_ident_char(lx_peek(lx)))
		lx_adv(lx);
	size_t len = (size_t)(lx->src + lx->pos - start);
	vrl_token tok = {0};
	tok.start = start;
	tok.len = len;
	tok.line = line;
	tok.col = col;
	if (len == 4 && !memcmp(start, "true", 4))
		tok.type = TK_TRUE;
	else if (len == 5 && !memcmp(start, "false", 5))
		tok.type = TK_FALSE;
	else if (len == 4 && !memcmp(start, "null", 4))
		tok.type = TK_NULL;
	else if (len == 2 && !memcmp(start, "if", 2))
		tok.type = TK_IF;
	else if (len == 4 && !memcmp(start, "else", 4))
		tok.type = TK_ELSE;
	else if (len == 5 && !memcmp(start, "abort", 5))
		tok.type = TK_ABORT;
	else
		tok.type = TK_IDENT;
	ts_push(lx->ts, tok);
}

vrl_token_stream *vrl_lex(const char *src, size_t src_len, avrl_log_level ll)
{
	vrl_token_stream *ts = calloc(1, sizeof(*ts));
	if (!ts)
		return NULL;
	lexer lx = {.src = src, .len = src_len, .pos = 0, .line = 1, .col = 1, .ts = ts, .ll = ll};

	while (!ts->err) {
		int saw_nl = skip_inline_ws(&lx);
		int c = lx_peek(&lx);
		if (c == -1)
			break;
		/* emit a statement separator for newlines at bracket depth 0,
		 * but never as the first token or right after another separator */
		if (saw_nl && lx.depth == 0 && ts->len > 0 &&
		    ts->toks[ts->len - 1].type != TK_NEWLINE) {
			vrl_token nl = {0};
			nl.type = TK_NEWLINE;
			nl.line = lx.line;
			nl.start = lx.src + lx.pos;
			ts_push(ts, nl);
		}
		const char *start = lx.src + lx.pos;
		uint32_t line = lx.line, col = lx.col;

		/* prefixed literals: r'...' s'...' t'...' */
		if ((c == 'r' || c == 's' || c == 't') &&
		    (lx_peek2(&lx) == '\'' || lx_peek2(&lx) == '"')) {
			scan_prefixed_literal(&lx, (char)c, start, line, col);
			continue;
		}

		if (isdigit(c)) {
			scan_number(&lx, start, line, col);
			continue;
		}
		if (is_ident_start(c)) {
			scan_ident_or_keyword(&lx, start, line, col);
			continue;
		}
		if (c == '"') {
			lx_adv(&lx);
			cbuf b = {0};
			if (scan_escaped_string(&lx, &b) != 0) {
				free(b.s);
				break;
			}
			vrl_token tok = {0};
			tok.type = TK_STRING;
			tok.start = start;
			tok.len = (size_t)(lx.src + lx.pos - start);
			tok.line = line;
			tok.col = col;
			tok.text = b.s ? b.s : strdup("");
			tok.text_len = b.len;
			ts_push(ts, tok);
			continue;
		}

		lx_adv(&lx); /* consume the punctuation char */
		switch (c) {
		case '.': emit_simple(&lx, TK_DOT, start, line, col); break;
		case '[': lx.depth++; emit_simple(&lx, TK_LBRACKET, start, line, col); break;
		case ']': if (lx.depth > 0) lx.depth--; emit_simple(&lx, TK_RBRACKET, start, line, col); break;
		case '{': emit_simple(&lx, TK_LBRACE, start, line, col); break;
		case '}': emit_simple(&lx, TK_RBRACE, start, line, col); break;
		case '(': lx.depth++; emit_simple(&lx, TK_LPAREN, start, line, col); break;
		case ')': if (lx.depth > 0) lx.depth--; emit_simple(&lx, TK_RPAREN, start, line, col); break;
		case ',': emit_simple(&lx, TK_COMMA, start, line, col); break;
		case ':': emit_simple(&lx, TK_COLON, start, line, col); break;
		case ';': emit_simple(&lx, TK_SEMICOLON, start, line, col); break;
		case '+': emit_simple(&lx, TK_PLUS, start, line, col); break;
		case '-':
			if (lx_peek(&lx) == '>') { lx_adv(&lx); emit_simple(&lx, TK_ARROW, start, line, col); }
			else emit_simple(&lx, TK_MINUS, start, line, col);
			break;
		case '*': emit_simple(&lx, TK_STAR, start, line, col); break;
		case '/': emit_simple(&lx, TK_SLASH, start, line, col); break;
		case '%': emit_simple(&lx, TK_PERCENT, start, line, col); break;
		case '=':
			if (lx_peek(&lx) == '=') { lx_adv(&lx); emit_simple(&lx, TK_EQ, start, line, col); }
			else emit_simple(&lx, TK_ASSIGN, start, line, col);
			break;
		case '!':
			if (lx_peek(&lx) == '=') { lx_adv(&lx); emit_simple(&lx, TK_NE, start, line, col); }
			else emit_simple(&lx, TK_BANG, start, line, col);
			break;
		case '<':
			if (lx_peek(&lx) == '=') { lx_adv(&lx); emit_simple(&lx, TK_LE, start, line, col); }
			else emit_simple(&lx, TK_LT, start, line, col);
			break;
		case '>':
			if (lx_peek(&lx) == '=') { lx_adv(&lx); emit_simple(&lx, TK_GE, start, line, col); }
			else emit_simple(&lx, TK_GT, start, line, col);
			break;
		case '&':
			if (lx_peek(&lx) == '&') { lx_adv(&lx); emit_simple(&lx, TK_AND, start, line, col); }
			else { lx_error(&lx, "unexpected '&' (did you mean '&&'?)"); }
			break;
		case '|':
			if (lx_peek(&lx) == '|') { lx_adv(&lx); emit_simple(&lx, TK_OR, start, line, col); }
			else emit_simple(&lx, TK_PIPE, start, line, col);
			break;
		case '?':
			if (lx_peek(&lx) == '?') { lx_adv(&lx); emit_simple(&lx, TK_COALESCE, start, line, col); }
			else emit_simple(&lx, TK_QUESTION, start, line, col);
			break;
		default: {
			char msg[64];
			snprintf(msg, sizeof(msg), "unexpected character '%c'", c);
			lx_error(&lx, msg);
			break;
		}
		}
	}

	vrl_token eof = {0};
	eof.type = TK_EOF;
	eof.line = lx.line;
	eof.start = lx.src + lx.pos;
	ts_push(ts, eof);
	return ts;
}

void vrl_token_stream_free(vrl_token_stream *ts)
{
	if (!ts)
		return;
	for (size_t i = 0; i < ts->len; i++)
		free(ts->toks[i].text);
	free(ts->toks);
	free(ts->err);
	free(ts);
}

const char *vrl_token_type_name(vrl_token_type t)
{
	switch (t) {
	case TK_EOF: return "EOF";
	case TK_ERROR: return "ERROR";
	case TK_NEWLINE: return "NEWLINE";
	case TK_INT: return "INT";
	case TK_FLOAT: return "FLOAT";
	case TK_STRING: return "STRING";
	case TK_REGEX: return "REGEX";
	case TK_TIMESTAMP: return "TIMESTAMP";
	case TK_TRUE: return "true";
	case TK_FALSE: return "false";
	case TK_NULL: return "null";
	case TK_IDENT: return "IDENT";
	case TK_DOT: return ".";
	case TK_LBRACKET: return "[";
	case TK_RBRACKET: return "]";
	case TK_LBRACE: return "{";
	case TK_RBRACE: return "}";
	case TK_LPAREN: return "(";
	case TK_RPAREN: return ")";
	case TK_COMMA: return ",";
	case TK_COLON: return ":";
	case TK_SEMICOLON: return ";";
	case TK_ASSIGN: return "=";
	case TK_PLUS: return "+";
	case TK_MINUS: return "-";
	case TK_STAR: return "*";
	case TK_SLASH: return "/";
	case TK_PERCENT: return "%";
	case TK_EQ: return "==";
	case TK_NE: return "!=";
	case TK_LT: return "<";
	case TK_LE: return "<=";
	case TK_GT: return ">";
	case TK_GE: return ">=";
	case TK_AND: return "&&";
	case TK_OR: return "||";
	case TK_BANG: return "!";
	case TK_COALESCE: return "??";
	case TK_QUESTION: return "?";
	case TK_PIPE: return "|";
	case TK_ARROW: return "->";
	case TK_IF: return "if";
	case TK_ELSE: return "else";
	case TK_ABORT: return "abort";
	}
	return "?";
}
