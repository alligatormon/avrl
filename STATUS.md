# avrl — implementation status

`avrl` is a from-scratch C implementation of **VRL (Vector Remap Language)** for the
alligator monitoring agent. It parses (multi-line) logs into structured events and/or
metrics. It is designed to be embedded/linked into alligator the same way `amtail`
(the mtail implementation) is.

> This file is the source of truth for **what is implemented, what is not, and why/when.**
> Update it as work progresses so future sessions can resume quickly.

- **Intended vendor path in alligator:** `src/external/avrl/` (submodule) + glue in `src/vrl/`
- **Language reference:** https://vrl.dev/functions
- **Status:** Phase 2 complete — standalone engine compiles, 54 self-tests pass, UBSan-clean.
  **All pure-C / no-new-dependency VRL functions are implemented (170/216).** Only
  heavy-dependency (📦) and host-integration (🔌) functions remain. Not yet wired into alligator.

---

## 1. Design decisions (locked in)

| Decision | Choice | Rationale |
|---|---|---|
| Language | Full VRL reimplementation in C | User requirement; link like mtail, no Rust/FFI |
| Packaging | New standalone lib `libavrl.a` (mirrors amtail layout) | Clean separation; can be a submodule |
| Output | Both transformed **event** and **metrics** | User requirement |
| Dependencies | **Core = libc only**; PCRE + jansson for regex/json | alligator's "light, no deps" mandate — both already alligator deps |
| Execution model | **Tree-walking interpreter** (not bytecode VM) | Correctness-first; matches VRL semantics closely; easy to extend |
| Value model | Refcounted tagged union, **move-on-insert** | Simple, predictable ownership |
| Errors | Runtime fallibility (`VRL_OK/ERR/ABORT`) | No static type/fallibility checker (yet) |
| Multiline | Dedicated record assembler (Vector-style) | The capability amtail fundamentally lacks |

**Key intentional divergence from upstream VRL:** upstream VRL is *statically* typed and
does compile-time fallibility checking. avrl does **runtime** fallibility instead (an
unhandled fallible call surfaces as a runtime error/abort rather than a compile error).
This keeps the implementation tractable in C.

---

## 2. Architecture / pipeline

```
vrl_compile()
  → vrl_lex()          lexer.c    source text → token stream (newline-aware)
  → vrl_parse()        parser.c   tokens → AST (Pratt / recursive descent)
vrl_exec()             interp.c   tree-walking evaluation against the `.` event
     ├─ stdlib.c       function registry (FNV hash) + core built-ins + module dispatch
     │   ├─ stdlib_type.c        type asserts, is_*, to_regex, mod, assert(_eq)
     │   ├─ stdlib_string.c      case conv, truncate, find, redact, sieve, replace_with, …
     │   ├─ stdlib_collection.c  array/enumerate/object ops, match_datadog_query
     │   ├─ stdlib_codec.c       base16/64, percent, mime_q, punycode, encode_csv/kv/logfmt
     │   ├─ stdlib_number.c      format_*, convert, syslog, system, haversine, crc/xxhash/seahash, IP
     │   ├─ stdlib_random.c      random_*, uuid_v4/v7, uuid_from_friendly_id
     │   ├─ stdlib_path.c        get/set/remove (del/exists live in interp.c)
     │   ├─ stdlib_parse.c       parse_* (kv, logfmt, url, syslog, grok, csv, aws, …)
     │   └─ stdlib_util.c        shared arg/buffer helpers
     ├─ value.c        refcounted value model
     ├─ json.c         jansson ⇄ value bridge
     └─ pcre_wrap.c    PCRE compile/exec wrapper (uses alligator regex_match struct)
multiline.c            physical lines → assembled logical records (fed to interp)
```

### File map

| File | Purpose | Status |
|---|---|---|
| `log.h` | per-subsystem debug levels (`avrl_log_level`) | done |
| `value.{h,c}` | value model: null/bool/int/float/bytes/timestamp/regex/array/object | done |
| `pcre_wrap.{h,c}` | PCRE wrapper (`avrl_regex_compile/free/exec`) | done |
| `lexer.{h,c}` | tokenizer; `r''`/`s''`/`t''` literals, `\u{}`, newline separators | done |
| `ast.h` | AST node definitions | done |
| `parser.{h,c}` | parser + RFC3339 timestamp parsing | done |
| `interp.{h,c}` | interpreter, ctx, lvalue/path resolution, operators, closures, `del`/`exists` | done |
| `stdlib.c` + `stdlib_*.c` | function registry + built-ins (split by category) | all pure-C funcs done (see §5) |
| `stdlib_internal.h` / `stdlib_util.c` | shared arg/buffer helpers + module registration | done |
| `json.{h,c}` | `parse_json`/`encode_json` conversion | done |
| `multiline.{h,c}` | multi-line record assembler | done |
| `vrl.{h,c}` | public API (`vrl_compile`, `vrl_event_from_message`, `vrl_run_once`) | done |
| `test.c` | CLI + self-test harness | done |
| `CMakeLists.txt` | build (static lib + `avrl` test binary) | done |
| **alligator glue** (`src/vrl/…`) | handler, push, del, metric export | **NOT STARTED** |

Total: ~8,000 LOC.

---

## 3. Language features

### Implemented
- Literals: integer, float (with `_` separators, exponents), string (`"..."` with escapes
  incl. `\u{}`), raw string (`s'...'`), regex (`r'...'`), timestamp (`t'...'`), `true`/`false`/`null`.
- Arrays `[...]`, objects `{ "k": v }`.
- Paths / event target: `.`, `.a.b`, `.a[0]`, `."quoted key"`, variable paths `x.y`.
- Assignment: `path = expr`, nested auto-vivification (`.a.b.c = 1` creates intermediates),
  compound targets `value, err = fallible()`.
- Operators: `+ - * / %`, `== != < <= > >=`, `&& ||` (short-circuit), unary `! -`,
  string concat via `+`, error-coalescing `??` (catches error **and** null on left).
- Control flow: `if / else if / else` (block-valued), blocks `{ ... }` (value = last expr).
- Function calls: positional + named args (`f(x, format: "...")`), fallible marker `f!()`.
- Closures: `map_values(x) -> |k, v| { ... }` (used by iteration functions).
- `abort` (with optional message) — drops the event.
- Comments `# ...`.
- **Newline-aware statement separation** (newlines separate statements at bracket depth 0,
  but are ignored inside `()`/`[]` and after operators for line continuation).

### Not implemented (language-level)
| Feature | Why / When |
|---|---|
| Static type checker & compile-time fallibility | Big effort; runtime fallibility used instead. Possible future phase. |
| `abort` with structured error object | Minor; currently message string only. |
| Metadata paths `%foo` (Vector event metadata) | Needs event-metadata model; add with alligator glue. |
| `if` used as an expression returning a value in all positions | Works as block value; edge cases untested. |

---

## 4. Multiline assembler (`multiline.{h,c}`)

The feature that motivated the whole project. Modes (mirrors Vector's `multiline`):

| Mode | Behavior | Status |
|---|---|---|
| `halt_before` | a line matching pattern starts a new record | done + tested |
| `halt_with` | a line matching pattern ends the current record (inclusive) | done |
| `continue_through` | group while lines match; first non-match ends record | done |
| `continue_past` | a matching line means the next line continues the record | done (≈ continue_through) |

Not yet: idle **timeout**-based flush (Vector's `timeout_ms`) — needs a timer from the
host (alligator event loop). `vrl_multiline_flush()` exists for EOF/manual flush.

---

## 5. Stdlib function coverage

> **Full per-function status is in
> [`FUNCTIONS_STATUS.md`](FUNCTIONS_STATUS.md), checked against
> https://vector.dev/docs/reference/vrl/functions/ .** Summary below.

avrl now implements **170 / 216** functions (~79%). **Every function that can be done
in pure C with only the existing deps (PCRE, jansson) is implemented.** The remaining 46
all require either a heavy external dependency (📦, 34) or alligator host integration
(🔌, 12) — see `FUNCTIONS_STATUS.md` for the per-function table.

### Fully implemented categories (✅)
Array (5/5), Coerce (5/5), Convert (6/6), Debug (3/3), Enumerate (16/16), Path (5/5),
Map (1/1), Number (7/7), Object (6/6), Random (7/7), Timestamp (2/2), Checksum (2/2).

### Highlights added in Phase 2
```
Path        : del exists get set remove              (del/exists are path-AST)
Type        : array bool float int object timestamp string(now a real assertion)
              is_empty is_nullish is_json to_regex tag_types_externally type_def
String      : snakecase camelcase pascalcase kebabcase screamingsnakecase
              truncate find contains_all match_any strlen basename dirname
              split_path strip_ansi_escape_codes shannon_entropy redact sieve
              replace_with parse_float
Collection  : append pop chunks zip compact flatten unique includes for_each
              map_keys match_array tally tally_value unflatten
              to_entries from_entries object_from_array unnest match_datadog_query
Codec       : encode/decode base16 base64 percent, decode_mime_q,
              encode/decode punycode, encode_csv encode_key_value encode_logfmt
Number      : format_int format_number mod from/to_unix_timestamp
              to_syslog_level/severity/facility/facility_code
System      : get_env_var get_hostname get_timezone_name
Map/Hash    : haversine crc xxhash(XXH64) seahash
IP          : ip_aton ip_ntoa ip_pton ip_ntop ip_to_ipv6 ipv6_to_ipv4
              ip_cidr_contains ip_subnet is_ipv4 is_ipv6
Random      : random_bool/int/float/bytes uuid_v4 uuid_v7 uuid_from_friendly_id
Parse       : parse_key_value parse_logfmt parse_query_string parse_url parse_int
              parse_bytes parse_duration parse_csv parse_tokens parse_regex_all
              parse_common_log parse_apache_log parse_nginx_log parse_glog parse_klog
              parse_syslog parse_linux_authorization parse_cef parse_ruby_hash
              parse_influxdb parse_grok parse_groks parse_aws_{alb,vpc_flow,cloudwatch}_*
```

### Known simplifications (⚠️ — see FUNCTIONS_STATUS.md)
- `match_datadog_query`: supports AND/OR/NOT, parens, `field:value`, `@attr`, ranges,
  comparisons and wildcards; **no** `field:(a OR b)` grouping or facet/tag-array nuance.
- `unnest`: takes `(object, field)` rather than a bare path expression.
- `type_def`: runtime approximation (upstream is compile-time).
- `get_timezone_name`: uses `TZ`/`tzname` (no full IANA database).
- `parse_grok`/`parse_groks`: use a built-in common-pattern set (not the full grok DB).

### Notable pure-C implementation notes
- `crc` = CRC-32/IEEE (decimal string); `xxhash` = XXH64; `seahash` = reference algorithm.
  All three verified against upstream test vectors in `test.c`.
- `decode_punycode`/`encode_punycode` implement RFC 3492 per label.
- RNG (`random_*`, uuid) uses xorshift128+ lazily seeded from `/dev/urandom` (non-crypto).

### Deliberately deferred — would add HEAVY dependencies (📦, violates "light, no deps")
| Function(s) | Dependency | Decision |
|---|---|---|
| `md5` `sha1` `sha2` `sha3` `hmac` `encrypt` `decrypt`, `community_id`, `decrypt_ip`/`encrypt_ip` | OpenSSL / libcrypto | **Opt-in only** (`AVRL_WITH_OPENSSL`). md5/sha could be pure-C opt-in. |
| `encode/decode` gzip/zlib/zstd/lz4/snappy | zlib / zstd / lz4 / snappy | **Opt-in only.** |
| `encode_charset`/`decode_charset` | iconv | Opt-in. |
| `encode_proto` `parse_proto` `parse_dnstap` | protobuf | Opt-in. |
| `parse_xml` `parse_yaml` `parse_cbor` `parse_etld` `parse_user_agent` | libxml/libyaml/CBOR/PSL/UA-DB | Opt-in. |
| `validate_json_schema` | JSON-schema lib | Opt-in. |

### Deferred — need alligator host integration (🔌)
`find_enrichment_table_records`, `get_enrichment_table_record`, `get_secret`,
`set_secret`, `remove_secret`, `set_semantic_meaning`, `aggregate_vector_metrics`,
`find_vector_metrics`, `get_vector_metric`, `dns_lookup`, `http_request`, `reverse_dns`
— implement together with the alligator glue (§8).

---

## 6. Public API (embedding)

```c
#include "vrl.h"

vrl_stdlib_init();                                   // once at startup
vrl_program *p = vrl_compile(src, len, ll);          // at config push; check p->err
vrl_ctx *ctx = vrl_ctx_new(ll);                      // per stream (reusable, hot path)

// per record:
vrl_ctx_reset(ctx);
vrl_ctx_set_event(ctx, vrl_event_from_message(line, len, "file"));
vrl_status st = vrl_exec(ctx, p->root);              // ctx->event now transformed
// st: VRL_OK | VRL_ERR | VRL_ABORT (abort => drop event)
```

One-shot helper for tests: `vrl_run_once(prog, event, &out_event, &err, ll)`.

Multiline: `vrl_multiline_new(pattern, mode, ll, &err)` → feed lines with
`vrl_multiline_feed(...)` → assembled records delivered to a callback → run each through
the interpreter.

---

## 7. Build & test

```sh
cd /Users/g.kashintsev/devel/myrepo/avrl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/avrl                       # run self-tests (54 cases)
./build/avrl -e 'PROGRAM' -m 'MSG' # run a program against {"message": MSG}, print event JSON
./build/avrl -f script.vrl -m 'MSG'
```

Manual build (macOS/homebrew paths) — note the split stdlib modules:
```sh
gcc -std=gnu11 -I. -I../alligator/src \
  -I/opt/homebrew/Cellar/pcre/8.45/include -I/opt/homebrew/Cellar/jansson/2.14.1/include \
  value.c pcre_wrap.c lexer.c parser.c interp.c \
  stdlib.c stdlib_util.c stdlib_type.c stdlib_string.c stdlib_collection.c \
  stdlib_codec.c stdlib_number.c stdlib_random.c stdlib_path.c stdlib_parse.c \
  json.c multiline.c vrl.c test.c \
  -L/opt/homebrew/Cellar/pcre/8.45/lib -L/opt/homebrew/Cellar/jansson/2.14.1/lib \
  -lpcre -ljansson -lm -o avrl
```

### Verification done
- ✅ Compiles clean with `-Wall -Wextra` (gcc/clang) and via CMake.
- ✅ 54/54 self-tests pass (`test.c`): the original 14 plus a Planned-C batch covering
  type asserts, case conversions, collections, codecs (base64/16/punycode round-trips),
  number/convert/IP, checksums (`crc`/`xxhash`/`seahash` vs upstream vectors), path
  get/set/remove, and parsers (kv/query/int/duration/csv/grok/datadog-query).
- ✅ UBSan clean (`-fsanitize=undefined -fno-sanitize-recover`) across the full suite plus
  ad-hoc exercise of every new function.
- ⚠️ ASan/LeakSanitizer could not run in the sandbox (`sanitizer_malloc_mac.inc` init check
  blocked by the environment). **TODO: run ASan+LSan outside sandbox** to confirm no leaks.

---

## 8. Roadmap / next steps (in order)

1. **Memory validation** — run ASan + LeakSanitizer outside the sandbox; fix any leaks
   (refcount audit of error paths in `interp.c` / `stdlib*.c`).
2. ✅ **`exists` / `del`** — done (path-AST special-case in `eval_call`).
3. ✅ **Stdlib breadth (pure-C)** — done: all Planned-C functions implemented (§5).
4. **Alligator glue** (`src/vrl/`), mirroring `src/amtail/`:
   - `vrl_parser_push()` registering aggregate handler key `"vrl"`.
   - `vrl_push(json_t*)` compiling programs at config push (store in `ac->vrl` hash).
   - `vrl_handler(char*, size_t, context_arg*)`: multiline-assemble → build event → run
     program → export metrics via `metric_add()` and/or forward transformed event.
   - Per-stream state in `context_arg` (ctx, tail buffer, multiline state).
   - Config: `vrl { name; program|script; multiline {...} }` + `aggregate { vrl file://... name=... }`.
   - API hook in `api_v1.c` for `PUT/DELETE "vrl"`.
   - `src/CMakeLists.txt` + `.gitmodules` submodule entry `src/external/avrl`.
5. **Multiline timeout flush** — wire to alligator's libuv timer.
6. **Metric emission from VRL** — decide mapping (e.g. a `metric` object convention or
   dedicated functions) so a VRL program can emit counters/gauges/histograms like amtail.
7. (Optional) heavy-dep functions behind CMake options (`AVRL_WITH_OPENSSL`, `AVRL_WITH_ZLIB`,
   `AVRL_WITH_MAXMINDDB`).

---

## 9. Session log (stages completed)

- **Stage 0** — Analyzed amtail (lexer→parser→AST→bytecode VM + PCRE) and alligator's
  amtail integration; concluded VRL needs a new front-end + runtime, reusing only infra
  patterns and the metric-export glue approach.
- **Stage 1** — Value model (`value.{h,c}`) + PCRE wrapper (`pcre_wrap.{h,c}`).
- **Stage 2** — Lexer (`lexer.{h,c}`).
- **Stage 3** — AST + parser (`ast.h`, `parser.{h,c}`), incl. RFC3339 timestamps.
- **Stage 4** — Interpreter (`interp.{h,c}`): ctx, path get/set + auto-vivify, operators,
  fallibility, closures.
- **Stage 5** — Stdlib (`stdlib.c`, 45 functions) + JSON bridge (`json.{h,c}`).
- **Stage 6** — Multiline assembler (`multiline.{h,c}`).
- **Stage 7** — Public API (`vrl.{h,c}`), CLI/self-tests (`test.c`), CMake; fixed
  newline-as-separator bug; all tests green; UBSan clean.
- **Stage 8** — **Implemented all "Planned-C" (pure-C) functions** (coverage 42 → 170).
  - Refactored the registry: `vrl_register()` + per-module `vrl_reg_*()` init; split stdlib
    into `stdlib_{type,string,collection,codec,number,random,path,parse}.c` + `stdlib_util.c`.
  - Added `del`/`exists` as path-AST builtins in `interp.c` (read-only path resolver +
    in-place deletion/array shift).
  - New categories fully covered: Array, Coerce, Convert, Debug, Enumerate, Path, Map,
    Number, Object, Random, Checksum. Broad String/Codec/IP/Parse/System/Type coverage.
  - Verified `crc`/`xxhash`/`seahash`/punycode against known vectors; 54/54 tests, UBSan clean.
  - Updated `FUNCTIONS_STATUS.md` (per-function) and this file.
- **Next** — see §8 (ASan/LSan, **alligator glue**, multiline timeout, metric emission,
  then optional 📦 heavy-dep functions behind CMake flags).
