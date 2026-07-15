# avrl — VRL function coverage

Per-function implementation status, checked against the official reference:
**https://vector.dev/docs/reference/vrl/functions/**.

**Summary: 170 / 216 implemented (~79%).**
**All pure-C / no-new-dependency functions (formerly "Planned-C") are now done.**
The remaining 46 are gated on heavy external dependencies (📦, 34) or on
alligator host integration (🔌, 12).

## Legend

| Tag | Meaning |
|-----|---------|
| ✅ **Done** | Implemented (libc + existing deps: PCRE, jansson) and tested |
| ⚠️ **Partial** | Implemented but a documented subset / simplified semantics |
| 🔌 **Host** | Needs alligator host integration (secrets, enrichment tables, vector metrics, DNS/HTTP, event metadata) |
| 📦 **Dep** | Needs a **heavy external dependency** — deferred / opt-in to preserve alligator's "light, no deps" rule |

Dependency notes for 📦: crypto → OpenSSL; gzip/zlib → zlib; zstd/lz4/snappy → their libs;
charset → iconv; proto/dnstap → protobuf; xml → libxml/expat; yaml → libyaml;
user_agent → UA regex DB; geoip enrichment → libmaxminddb; json schema → schema lib;
cbor → CBOR lib; etld → public suffix list.

Source layout for the stdlib (split by category):
`stdlib.c` (core), `stdlib_type.c`, `stdlib_string.c`, `stdlib_collection.c`,
`stdlib_codec.c`, `stdlib_number.c` (number/convert/system/map/checksum/ip),
`stdlib_random.c`, `stdlib_path.c`, `stdlib_parse.c`. `del`/`exists` are handled
in `interp.c` (they take a path *expression*, not a value).

---

## Array functions (5/5) ✅

| Function | Status | Notes |
|---|---|---|
| append | ✅ Done | |
| chunks | ✅ Done | array or string |
| pop | ✅ Done | returns array without last element |
| push | ✅ Done | |
| zip | ✅ Done | two arrays or transpose of array-of-arrays |

## Codec functions (13/29)

| Function | Status | Notes |
|---|---|---|
| decode_base16 | ✅ Done | hex |
| decode_base64 | ✅ Done | standard + url_safe alphabets |
| decode_charset | 📦 Dep | iconv |
| decode_gzip | 📦 Dep | zlib |
| decode_lz4 | 📦 Dep | liblz4 |
| decode_mime_q | ✅ Done | Q and B encoded-words |
| decode_percent | ✅ Done | |
| decode_punycode | ✅ Done | RFC 3492, per-label |
| decode_snappy | 📦 Dep | snappy |
| decode_zlib | 📦 Dep | zlib |
| decode_zstd | 📦 Dep | zstd |
| encode_base16 | ✅ Done | |
| encode_base64 | ✅ Done | charset + padding options |
| encode_charset | 📦 Dep | iconv |
| encode_csv | ✅ Done | RFC4180 quoting |
| encode_gzip | 📦 Dep | zlib |
| encode_json | ✅ Done | via jansson |
| encode_key_value | ✅ Done | |
| encode_logfmt | ✅ Done | |
| encode_lz4 | 📦 Dep | |
| encode_percent | ✅ Done | |
| encode_proto | 📦 Dep | protobuf |
| encode_punycode | ✅ Done | RFC 3492 |
| encode_snappy | 📦 Dep | |
| encode_zlib | 📦 Dep | |
| encode_zstd | 📦 Dep | |

## Coerce functions (5/5) ✅

| Function | Status | Notes |
|---|---|---|
| to_bool | ✅ Done | |
| to_float | ✅ Done | |
| to_int | ✅ Done | |
| to_regex | ✅ Done | compiles pattern → regex value |
| to_string | ✅ Done | |

## Convert functions (6/6) ✅

| Function | Status | Notes |
|---|---|---|
| from_unix_timestamp | ✅ Done | seconds/milliseconds/nanoseconds |
| to_syslog_facility | ✅ Done | code → name |
| to_syslog_facility_code | ✅ Done | name → code |
| to_syslog_level | ✅ Done | severity → level name |
| to_syslog_severity | ✅ Done | level name → severity |
| to_unix_timestamp | ✅ Done | seconds/milliseconds/nanoseconds |

## Debug functions (3/3) ✅

| Function | Status | Notes |
|---|---|---|
| assert | ✅ Done | fails as recoverable error |
| assert_eq | ✅ Done | |
| log | ✅ Done | writes to stderr |

## Enrichment functions (0/2)

| Function | Status | Notes |
|---|---|---|
| find_enrichment_table_records | 🔌 Host | needs enrichment-table infra |
| get_enrichment_table_record | 🔌 Host / 📦 | + libmaxminddb for GeoIP tables |

## Enumerate functions (16/16) ✅

| Function | Status | Notes |
|---|---|---|
| compact | ✅ Done | null/string/object/array/recursive options |
| filter | ✅ Done | closure-based |
| flatten | ✅ Done | object (dotted keys) + array |
| for_each | ✅ Done | closure-based |
| includes | ✅ Done | |
| keys | ✅ Done | |
| length | ✅ Done | |
| map_keys | ✅ Done | recursive option |
| map_values | ✅ Done | closure-based |
| match_array | ✅ Done | any / all |
| strlen | ✅ Done | UTF-8 char count |
| tally | ✅ Done | |
| tally_value | ✅ Done | |
| unflatten | ✅ Done | dotted keys → nested |
| unique | ✅ Done | |
| values | ✅ Done | |

## Event functions (0/4)

| Function | Status | Notes |
|---|---|---|
| get_secret | 🔌 Host | |
| remove_secret | 🔌 Host | |
| set_secret | 🔌 Host | |
| set_semantic_meaning | 🔌 Host | needs event metadata model |

## Path functions (5/5) ✅

| Function | Status | Notes |
|---|---|---|
| del | ✅ Done | path-AST, special-cased in `interp.c` |
| exists | ✅ Done | path-AST |
| get | ✅ Done | dynamic path array |
| remove | ✅ Done | dynamic path array |
| set | ✅ Done | dynamic path array |

## Cryptography functions (1/8)

| Function | Status | Notes |
|---|---|---|
| decrypt | 📦 Dep | OpenSSL |
| encrypt | 📦 Dep | OpenSSL |
| hmac | 📦 Dep | OpenSSL |
| md5 | 📦 Dep | (small pure-C impl possible; opt-in) |
| seahash | ✅ Done | reference algorithm, matches upstream vectors |
| sha1 | 📦 Dep | (or pure-C; opt-in) |
| sha2 | 📦 Dep | |
| sha3 | 📦 Dep | |

## IP functions (10/12)

| Function | Status | Notes |
|---|---|---|
| decrypt_ip | 📦 Dep | crypto |
| encrypt_ip | 📦 Dep | crypto |
| ip_aton | ✅ Done | |
| ip_cidr_contains | ✅ Done | v4 + v6 |
| ip_ntoa | ✅ Done | |
| ip_ntop | ✅ Done | |
| ip_pton | ✅ Done | |
| ip_subnet | ✅ Done | prefix or netmask |
| ip_to_ipv6 | ✅ Done | v4-mapped |
| ipv6_to_ipv4 | ✅ Done | |
| is_ipv4 | ✅ Done | |
| is_ipv6 | ✅ Done | |

## Map functions (1/1) ✅

| Function | Status | Notes |
|---|---|---|
| haversine | ✅ Done | metric/imperial |

## Metrics functions (0/3)

| Function | Status | Notes |
|---|---|---|
| aggregate_vector_metrics | 🔌 Host | |
| find_vector_metrics | 🔌 Host | |
| get_vector_metric | 🔌 Host | |

## Number functions (7/7) ✅

| Function | Status | Notes |
|---|---|---|
| abs | ✅ Done | |
| ceil | ✅ Done | (precision arg TODO) |
| floor | ✅ Done | (precision arg TODO) |
| format_int | ✅ Done | base 2–36 |
| format_number | ✅ Done | scale + grouping/decimal separators |
| mod | ✅ Done | function form (also `%` operator) |
| round | ✅ Done | (precision arg TODO) |

## Object functions (6/6) ✅

| Function | Status | Notes |
|---|---|---|
| from_entries | ✅ Done | [k,v] pairs or {key,value} |
| match_datadog_query | ⚠️ Partial | AND/OR/NOT, parens, field:val, @attr, ranges, comparisons, wildcards; no `field:(a OR b)` grouping |
| merge | ✅ Done | (no `deep` option yet) |
| object_from_array | ✅ Done | (values,keys) or pairs |
| to_entries | ✅ Done | |
| unnest | ⚠️ Partial | takes (object, field) rather than a bare path |

## Parse functions (28/35)

| Function | Status | Notes |
|---|---|---|
| parse_apache_log | ✅ Done | common / combined / error |
| parse_aws_alb_log | ✅ Done | |
| parse_aws_cloudwatch_log_subscription_message | ✅ Done | JSON message |
| parse_aws_vpc_flow_log | ✅ Done | v2 default fields |
| parse_bytes | ✅ Done | binary/decimal units |
| parse_cbor | 📦 Dep | CBOR lib |
| parse_cef | ✅ Done | header + extensions |
| parse_common_log | ✅ Done | |
| parse_csv | ✅ Done | single record, quoted fields |
| parse_dnstap | 📦 Dep | protobuf/framestream |
| parse_duration | ✅ Done | compound durations |
| parse_etld | 📦 Dep | public suffix list |
| parse_glog | ✅ Done | |
| parse_grok | ✅ Done | built-in pattern set → PCRE named groups |
| parse_groks | ✅ Done | first matching pattern |
| parse_influxdb | ✅ Done | line protocol |
| parse_int | ✅ Done | base + auto-detect |
| parse_json | ✅ Done | via jansson |
| parse_key_value | ✅ Done | quoted values |
| parse_klog | ✅ Done | |
| parse_linux_authorization | ✅ Done | syslog-based |
| parse_logfmt | ✅ Done | |
| parse_nginx_log | ✅ Done | combined / error |
| parse_proto | 📦 Dep | protobuf |
| parse_query_string | ✅ Done | repeated keys → arrays |
| parse_regex | ✅ Done | numbered + named captures |
| parse_regex_all | ✅ Done | |
| parse_ruby_hash | ✅ Done | strings/symbols/numbers/nil/bool/nested |
| parse_syslog | ✅ Done | RFC3164 + RFC5424 |
| parse_timestamp | ✅ Done | strptime + RFC3339 |
| parse_tokens | ✅ Done | quotes + [brackets] |
| parse_url | ✅ Done | scheme/user/pass/host/port/path/query/fragment |
| parse_user_agent | 📦 Dep | UA regex DB |
| parse_xml | 📦 Dep | libxml/expat |
| parse_yaml | 📦 Dep | libyaml |

## Random functions (7/7) ✅

| Function | Status | Notes |
|---|---|---|
| random_bool | ✅ Done | |
| random_bytes | ✅ Done | |
| random_float | ✅ Done | |
| random_int | ✅ Done | |
| uuid_from_friendly_id | ✅ Done | base62 → uuid |
| uuid_v4 | ✅ Done | |
| uuid_v7 | ✅ Done | time-ordered |

## String functions (29/30)

| Function | Status | Notes |
|---|---|---|
| basename | ✅ Done | |
| camelcase | ✅ Done | |
| community_id | 📦 Dep | needs SHA1 |
| contains | ✅ Done | |
| contains_all | ✅ Done | case_sensitive option |
| dirname | ✅ Done | |
| downcase | ✅ Done | |
| ends_with | ✅ Done | |
| find | ✅ Done | string or regex, byte index |
| join | ✅ Done | |
| kebabcase | ✅ Done | |
| match | ✅ Done | |
| match_any | ✅ Done | |
| parse_float | ✅ Done | (listed under String in docs) |
| pascalcase | ✅ Done | |
| redact | ✅ Done | regex/pattern filters, recursive |
| replace | ✅ Done | literal + regex |
| replace_with | ✅ Done | closure-based, match object |
| screamingsnakecase | ✅ Done | |
| shannon_entropy | ✅ Done | |
| sieve | ✅ Done | keep chars matching filter regex |
| slice | ✅ Done | |
| snakecase | ✅ Done | |
| split | ✅ Done | literal + regex + limit |
| split_path | ✅ Done | non-standard extension (splits on `/`) |
| starts_with | ✅ Done | |
| strip_ansi_escape_codes | ✅ Done | CSI + OSC |
| strip_whitespace | ✅ Done | |
| truncate | ✅ Done | UTF-8, optional ellipsis |
| upcase | ✅ Done | |

## System functions (3/6)

| Function | Status | Notes |
|---|---|---|
| dns_lookup | 🔌 Host | resolver / event loop |
| get_env_var | ✅ Done | getenv |
| get_hostname | ✅ Done | gethostname |
| get_timezone_name | ⚠️ Partial | TZ env / tzname (no full IANA db) |
| http_request | 🔌 Host | needs HTTP client / async |
| reverse_dns | 🔌 Host | resolver |

## Timestamp functions (2/2) ✅

| Function | Status | Notes |
|---|---|---|
| format_timestamp | ✅ Done | strftime |
| now | ✅ Done | |

## Type functions (21/22)

| Function | Status | Notes |
|---|---|---|
| array | ✅ Done | type assertion |
| bool | ✅ Done | type assertion |
| float | ✅ Done | type assertion |
| int | ✅ Done | type assertion |
| is_array | ✅ Done | |
| is_boolean | ✅ Done | |
| is_empty | ✅ Done | |
| is_float | ✅ Done | |
| is_integer | ✅ Done | |
| is_json | ✅ Done | optional variant |
| is_null | ✅ Done | |
| is_nullish | ✅ Done | null / "" / "-" / whitespace |
| is_object | ✅ Done | |
| is_regex | ✅ Done | |
| is_string | ✅ Done | |
| is_timestamp | ✅ Done | |
| object | ✅ Done | type assertion |
| string | ✅ Done | **fixed**: now a type assertion (errors if not a string) |
| tag_types_externally | ✅ Done | |
| timestamp | ✅ Done | type assertion |
| type_def | ⚠️ Partial | runtime approximation (`{"type": ...}`); upstream is compile-time |
| validate_json_schema | 📦 Dep | JSON-schema lib |

## Checksum functions (2/2) ✅

| Function | Status | Notes |
|---|---|---|
| crc | ✅ Done | CRC-32/IEEE, decimal string |
| xxhash | ✅ Done | XXH64, matches upstream vectors |

---

## avrl non-standard extensions (not in VRL)

| Function | Notes |
|---|---|
| `to_timestamp` | Convenience coercer added during bring-up. Upstream uses `parse_timestamp` / `from_unix_timestamp`. |
| `split_path` | Splits a filesystem-style path on `/`. Not an upstream VRL function. |

---

## Category rollup

| Category | Done / Total |
|---|---|
| Array | 5/5 |
| Codec | 13/29 |
| Coerce | 5/5 |
| Convert | 6/6 |
| Debug | 3/3 |
| Enrichment | 0/2 |
| Enumerate | 16/16 |
| Event | 0/4 |
| Path | 5/5 |
| Cryptography | 1/8 |
| IP | 10/12 |
| Map | 1/1 |
| Metrics | 0/3 |
| Number | 7/7 |
| Object | 6/6 |
| Parse | 28/35 |
| Random | 7/7 |
| String | 29/30 |
| System | 3/6 |
| Timestamp | 2/2 |
| Type | 21/22 |
| Checksum | 2/2 |
| **Total** | **170/216** |

## Remaining work (46 functions)

**📦 Heavy dependency (34)** — opt-in behind CMake flags to keep the core light:
- Crypto (7): `decrypt`, `encrypt`, `hmac`, `md5`, `sha1`, `sha2`, `sha3` → OpenSSL (md5/sha could be pure-C opt-in)
- Compression (10): `decode_gzip`/`zlib`/`zstd`/`lz4`/`snappy`, `encode_*` → zlib/zstd/lz4/snappy
- Charset (2): `decode_charset`, `encode_charset` → iconv
- Proto (3): `encode_proto`, `parse_proto`, `parse_dnstap` → protobuf
- Parsers (4): `parse_cbor`, `parse_etld`, `parse_user_agent`, `parse_xml`, `parse_yaml` → CBOR/PSL/UA-DB/libxml/libyaml
- IP crypto (2): `decrypt_ip`, `encrypt_ip`
- Misc (2): `community_id` (SHA1), `validate_json_schema`

**🔌 Host integration (12)** — implement together with the alligator glue:
- Enrichment (2): `find_enrichment_table_records`, `get_enrichment_table_record`
- Event/secrets (4): `get_secret`, `set_secret`, `remove_secret`, `set_semantic_meaning`
- Metrics (3): `aggregate_vector_metrics`, `find_vector_metrics`, `get_vector_metric`
- System (3): `dns_lookup`, `http_request`, `reverse_dns`
