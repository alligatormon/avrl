#define _GNU_SOURCE
#include "vrl.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static avrl_log_level LL;

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(sz + 1);
	size_t rd = fread(buf, 1, sz, f);
	buf[rd] = '\0';
	fclose(f);
	if (len)
		*len = rd;
	return buf;
}

static int run_program_on_message(const char *prog_src, const char *msg)
{
	vrl_program *p = vrl_compile(prog_src, strlen(prog_src), LL);
	if (p->err) {
		fprintf(stderr, "compile error: %s\n", p->err);
		vrl_program_free(p);
		return 1;
	}
	vrl_value *ev = vrl_event_from_message(msg, strlen(msg), NULL);
	vrl_value *out = NULL;
	char *err = NULL;
	vrl_status st = vrl_run_once(p, ev, &out, &err, LL);
	if (st == VRL_OK) {
		char *j = vrl_json_encode(out);
		printf("%s\n", j);
		free(j);
		vrl_value_unref(out);
	} else {
		fprintf(stderr, "%s: %s\n", st == VRL_ABORT ? "abort" : "error",
			err ? err : "(no message)");
	}
	free(err);
	vrl_program_free(p);
	return st == VRL_OK ? 0 : 1;
}

/* ---------------- self tests ---------------- */

static int tests_run = 0, tests_failed = 0;

static void check_json(const char *name, const char *prog, const char *msg, const char *expect)
{
	tests_run++;
	vrl_program *p = vrl_compile(prog, strlen(prog), LL);
	if (p->err) {
		printf("FAIL %-28s compile error: %s\n", name, p->err);
		tests_failed++;
		vrl_program_free(p);
		return;
	}
	vrl_value *ev = vrl_event_from_message(msg, strlen(msg), NULL);
	vrl_value *out = NULL;
	char *err = NULL;
	vrl_status st = vrl_run_once(p, ev, &out, &err, LL);
	if (st != VRL_OK) {
		printf("FAIL %-28s runtime %s: %s\n", name, st == VRL_ABORT ? "abort" : "error", err ? err : "");
		tests_failed++;
		free(err);
		vrl_program_free(p);
		return;
	}
	char *j = vrl_json_encode(out);
	if (strcmp(j, expect) != 0) {
		printf("FAIL %-28s\n     got:    %s\n     expect: %s\n", name, j, expect);
		tests_failed++;
	} else {
		printf("ok   %-28s %s\n", name, j);
	}
	free(j);
	vrl_value_unref(out);
	vrl_program_free(p);
}

static void ml_collect(void *ud, const char *rec, size_t len)
{
	char ***acc = ud;
	size_t n = 0;
	while ((*acc)[n]) n++;
	*acc = realloc(*acc, (n + 2) * sizeof(char *));
	(*acc)[n] = strndup(rec, len);
	(*acc)[n + 1] = NULL;
}

static void test_multiline(void)
{
	tests_run++;
	char **acc = calloc(1, sizeof(char *));
	char *err = NULL;
	/* records start with a bracketed timestamp; continuation lines indented */
	vrl_multiline *ml = vrl_multiline_new("^\\[", VRL_ML_HALT_BEFORE, LL, &err);
	const char *lines[] = {
		"[2021-01-01] start",
		"    at foo()",
		"    at bar()",
		"[2021-01-02] next",
		"    at baz()",
		NULL,
	};
	for (int i = 0; lines[i]; i++)
		vrl_multiline_feed(ml, lines[i], strlen(lines[i]), ml_collect, &acc);
	vrl_multiline_flush(ml, ml_collect, &acc);
	vrl_multiline_free(ml);

	int n = 0;
	while (acc[n]) n++;
	int ok = (n == 2) &&
		 strcmp(acc[0], "[2021-01-01] start\n    at foo()\n    at bar()") == 0 &&
		 strcmp(acc[1], "[2021-01-02] next\n    at baz()") == 0;
	if (ok)
		printf("ok   %-28s 2 records assembled\n", "multiline.halt_before");
	else {
		printf("FAIL %-28s got %d records\n", "multiline.halt_before", n);
		for (int i = 0; i < n; i++)
			printf("     [%d] %s\n", i, acc[i]);
		tests_failed++;
	}
	for (int i = 0; i < n; i++)
		free(acc[i]);
	free(acc);
}

static int self_tests(void)
{
	printf("=== avrl self tests ===\n");

	check_json("assign.field", ".foo = 1", "x", "{\"message\":\"x\",\"foo\":1}");
	check_json("arith.int", ".n = 2 + 3 * 4", "x", "{\"message\":\"x\",\"n\":14}");
	check_json("string.concat", ".s = \"a\" + \"b\"", "x", "{\"message\":\"x\",\"s\":\"ab\"}");
	check_json("upcase", ".u = upcase(.message)", "hi", "{\"message\":\"hi\",\"u\":\"HI\"}");
	check_json("parse_json.bang",
		   ". = parse_json!(.message)",
		   "{\"a\":1,\"b\":\"two\"}",
		   "{\"a\":1,\"b\":\"two\"}");
	check_json("nested.path", ".a.b.c = true", "x", "{\"message\":\"x\",\"a\":{\"b\":{\"c\":true}}}");
	check_json("if.else",
		   "if .message == \"yes\" { .ok = true } else { .ok = false }",
		   "yes", "{\"message\":\"yes\",\"ok\":true}");
	check_json("coalesce",
		   ".n = to_int(.message) ?? -1",
		   "notanumber", "{\"message\":\"notanumber\",\"n\":-1}");
	check_json("err.assign",
		   ".v, .err = to_int(.message)",
		   "42", "{\"message\":\"42\",\"v\":42,\"err\":null}");
	check_json("split",
		   ".parts = split(.message, \",\")",
		   "a,b,c", "{\"message\":\"a,b,c\",\"parts\":[\"a\",\"b\",\"c\"]}");
	check_json("parse_regex",
		   ".m = parse_regex!(.message, r'(?P<num>\\d+)')",
		   "id=123", "{\"message\":\"id=123\",\"m\":{\"0\":\"123\",\"1\":\"123\",\"num\":\"123\"}}");
	check_json("del.via.assign.replace",
		   ".message = replace(.message, \"foo\", \"bar\")",
		   "foo baz foo", "{\"message\":\"bar baz bar\"}");
	check_json("map_values",
		   ". = map_values({\"a\": 1, \"b\": 2}) -> |v| { v + 10 }",
		   "x", "{\"a\":11,\"b\":12}");

	test_multiline();

	printf("=== %d run, %d failed ===\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
	vrl_stdlib_init();

	if (argc >= 3 && !strcmp(argv[1], "-e")) {
		const char *msg = (argc >= 5 && !strcmp(argv[3], "-m")) ? argv[4] : "";
		return run_program_on_message(argv[2], msg);
	}
	if (argc >= 3 && !strcmp(argv[1], "-f")) {
		size_t len;
		char *src = read_file(argv[2], &len);
		if (!src) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
		const char *msg = (argc >= 5 && !strcmp(argv[3], "-m")) ? argv[4] : "";
		int rc = run_program_on_message(src, msg);
		free(src);
		return rc;
	}

	return self_tests();
}
