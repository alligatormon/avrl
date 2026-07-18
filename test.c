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

	/* ---- Planned-C batch ---- */

	/* type / coerce / debug */
	check_json("type.int", ".v = int(3)", "x", "{\"message\":\"x\",\"v\":3}");
	check_json("type.mod", ".m = mod(10, 3)", "x", "{\"message\":\"x\",\"m\":1}");
	check_json("type.is_nullish", ".n = is_nullish(\"-\")", "x", "{\"message\":\"x\",\"n\":true}");
	check_json("type.is_empty", ".n = is_empty([])", "x", "{\"message\":\"x\",\"n\":true}");

	/* string */
	check_json("str.snakecase", ".s = snakecase(\"helloWorld\")", "x", "{\"message\":\"x\",\"s\":\"hello_world\"}");
	check_json("str.camelcase", ".s = camelcase(\"hello_world\")", "x", "{\"message\":\"x\",\"s\":\"helloWorld\"}");
	check_json("str.truncate", ".s = truncate(\"hello\", 3, true)", "x", "{\"message\":\"x\",\"s\":\"hel...\"}");
	check_json("str.parse_float", ".f = parse_float!(\"3.14\")", "x", "{\"message\":\"x\",\"f\":3.14}");
	check_json("str.contains_all", ".b = contains_all(\"hello world\", [\"hello\",\"world\"])", "x", "{\"message\":\"x\",\"b\":true}");

	/* collection */
	check_json("coll.append", ".a = append([1,2],[3])", "x", "{\"message\":\"x\",\"a\":[1,2,3]}");
	check_json("coll.unique", ".a = unique([1,1,2,2,3])", "x", "{\"message\":\"x\",\"a\":[1,2,3]}");
	check_json("coll.includes", ".b = includes([1,2,3], 2)", "x", "{\"message\":\"x\",\"b\":true}");
	check_json("coll.flatten", ".o = flatten({\"a\":{\"b\":1},\"c\":2})", "x", "{\"message\":\"x\",\"o\":{\"a.b\":1,\"c\":2}}");
	check_json("coll.chunks", ".a = chunks([1,2,3], 2)", "x", "{\"message\":\"x\",\"a\":[[1,2],[3]]}");
	check_json("coll.to_entries", ".o = to_entries({\"a\":1})", "x", "{\"message\":\"x\",\"o\":[{\"key\":\"a\",\"value\":1}]}");

	/* codec */
	check_json("codec.b64", ".s = encode_base64(\"hello\")", "x", "{\"message\":\"x\",\"s\":\"aGVsbG8=\"}");
	check_json("codec.b64.round", ".s = decode_base64!(encode_base64(.message))", "hello", "{\"message\":\"hello\",\"s\":\"hello\"}");
	check_json("codec.b16", ".s = encode_base16(\"AB\")", "x", "{\"message\":\"x\",\"s\":\"4142\"}");
	check_json("codec.puny", ".s = encode_punycode(\"münchen.de\")", "x", "{\"message\":\"x\",\"s\":\"xn--mnchen-3ya.de\"}");
	check_json("codec.puny.round", ".s = decode_punycode!(\"xn--mnchen-3ya.de\")", "x", "{\"message\":\"x\",\"s\":\"münchen.de\"}");

	/* number / convert / system / checksum / ip */
	check_json("num.format_int", ".s = format_int(255, 16)", "x", "{\"message\":\"x\",\"s\":\"ff\"}");
	check_json("num.unix.round", ".n = to_unix_timestamp(from_unix_timestamp(1000))", "x", "{\"message\":\"x\",\"n\":1000}");
	check_json("num.syslog_level", ".s = to_syslog_level(6)", "x", "{\"message\":\"x\",\"s\":\"informational\"}");
	check_json("num.crc", ".s = crc(\"hello\")", "x", "{\"message\":\"x\",\"s\":\"907060870\"}");
	check_json("num.seahash", ".n = seahash(\"to be or not to be\")", "x", "{\"message\":\"x\",\"n\":1988685042348123509}");
	check_json("num.xxhash", ".n = xxhash(\"hello\")", "x", "{\"message\":\"x\",\"n\":2794345569481354659}");
	check_json("num.ip", ".s = ip_ntoa(ip_aton!(\"1.2.3.4\"))", "x", "{\"message\":\"x\",\"s\":\"1.2.3.4\"}");
	check_json("num.is_ipv4", ".b = is_ipv4(\"1.2.3.4\")", "x", "{\"message\":\"x\",\"b\":true}");
	check_json("num.cidr", ".b = ip_cidr_contains!(\"192.168.0.0/16\",\"192.168.1.1\")", "x", "{\"message\":\"x\",\"b\":true}");

	/* path */
	check_json("path.get", ".v = get!({\"a\":{\"b\":5}}, [\"a\",\"b\"])", "x", "{\"message\":\"x\",\"v\":5}");
	check_json("path.set", ". = set!({}, [\"a\",\"b\"], 1)", "x", "{\"a\":{\"b\":1}}");
	check_json("path.remove", ". = remove!({\"a\":1,\"b\":2}, [\"a\"])", "x", "{\"b\":2}");
	check_json("path.exists", ".e = exists(.message)", "x", "{\"message\":\"x\",\"e\":true}");

	/* parse */
	check_json("parse.kv", ".o = parse_key_value!(\"a=1 b=2\")", "x", "{\"message\":\"x\",\"o\":{\"a\":\"1\",\"b\":\"2\"}}");
	check_json("parse.query", ".o = parse_query_string(\"a=1&b=2\")", "x", "{\"message\":\"x\",\"o\":{\"a\":\"1\",\"b\":\"2\"}}");
	check_json("parse.int", ".n = parse_int!(\"ff\", 16)", "x", "{\"message\":\"x\",\"n\":255}");
	check_json("parse.duration", ".d = parse_duration!(\"1h\", \"m\")", "x", "{\"message\":\"x\",\"d\":60.0}");
	check_json("parse.csv", ".a = parse_csv!(\"a,\\\"b,c\\\",d\")", "x", "{\"message\":\"x\",\"a\":[\"a\",\"b,c\",\"d\"]}");
	check_json("parse.grok", ".o = parse_grok!(\"hello 123\", \"%{WORD:word} %{INT:num}\")", "x",
		   "{\"message\":\"x\",\"o\":{\"num\":\"123\",\"word\":\"hello\"}}");

	/* object query */
	check_json("obj.datadog_query",
		   ".b = match_datadog_query({\"status\":\"error\",\"level\":5}, \"status:error AND level:>3\")",
		   "x", "{\"message\":\"x\",\"b\":true}");

#ifdef AVRL_WITH_OPENSSL
	/* crypto (OpenSSL) */
	check_json("crypto.md5", ".s = md5(\"foo\")", "x",
		   "{\"message\":\"x\",\"s\":\"acbd18db4cc2f85cedef654fccc4a4d8\"}");
	check_json("crypto.sha1", ".s = sha1(\"foo\")", "x",
		   "{\"message\":\"x\",\"s\":\"0beec7b5ea3f0fdbc95d0dd47f3c5bc275da8a33\"}");
	check_json("crypto.sha2", ".s = sha2(\"foobar\")", "x",
		   "{\"message\":\"x\",\"s\":\"d014c752bc2be868e16330f47e0c316a5967bcbc9c286a457761d7055b9214ce\"}");
	check_json("crypto.sha3", ".s = sha3(\"foo\", variant: \"SHA3-224\")", "x",
		   "{\"message\":\"x\",\"s\":\"f4f6779e153c391bbd29c95e72b0708e39d9166c7cea51d1f10ef58a\"}");
	check_json("crypto.hmac",
		   ".s = encode_base64(hmac(\"Hello there\", \"super-secret-key\"))", "x",
		   "{\"message\":\"x\",\"s\":\"eLGE8YMviv85NPXgISRUZxstBNSU47JQdcXkUWcClmI=\"}");
	check_json("crypto.encrypt.round",
		   "iv = \"0123456789012345\"\n"
		   "key = \"01234567890123456789012345678912\"\n"
		   ".s = decrypt!(encrypt!(\"data\", \"AES-256-CFB\", key: key, iv: iv), \"AES-256-CFB\", key: key, iv: iv)",
		   "x", "{\"message\":\"x\",\"s\":\"data\"}");
	check_json("crypto.community_id",
		   ".s = community_id!(source_ip: \"1.2.3.4\", destination_ip: \"5.6.7.8\", "
		   "source_port: 1122, destination_port: 3344, protocol: 6)",
		   "x", "{\"message\":\"x\",\"s\":\"1:wCb3OG7yAFWelaUydu0D+125CLM=\"}");
	check_json("crypto.encrypt_ip",
		   ".s = encrypt_ip!(\"192.168.1.1\", \"sixteen byte key\", \"aes128\")",
		   "x", "{\"message\":\"x\",\"s\":\"72b9:a747:f2e9:72af:76ca:5866:6dcf:c3b0\"}");
	check_json("crypto.decrypt_ip",
		   ".s = decrypt_ip!(\"72b9:a747:f2e9:72af:76ca:5866:6dcf:c3b0\", \"sixteen byte key\", \"aes128\")",
		   "x", "{\"message\":\"x\",\"s\":\"192.168.1.1\"}");
	check_json("crypto.ipcrypt.pfx",
		   ".s = decrypt_ip!(encrypt_ip!(\"192.168.1.1\", \"thirty-two bytes key for pfx use\", \"pfx\"), "
		   "\"thirty-two bytes key for pfx use\", \"pfx\")",
		   "x", "{\"message\":\"x\",\"s\":\"192.168.1.1\"}");
#endif

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
