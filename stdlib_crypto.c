#define _GNU_SOURCE
#include "stdlib_internal.h"

#ifdef AVRL_WITH_OPENSSL

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#define ARG(name, idx) vrl_arg(a, name, idx)

/* ================================================================== */
/* helpers                                                            */
/* ================================================================== */

static char *hex_encode(const unsigned char *in, size_t n)
{
	static const char *hex = "0123456789abcdef";
	char *out = malloc(n * 2 + 1);
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = hex[in[i] >> 4];
		out[i * 2 + 1] = hex[in[i] & 0xf];
	}
	out[n * 2] = '\0';
	return out;
}

static char *b64_encode(const unsigned char *in, size_t n)
{
	static const char *alpha =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	avrl_buf b;
	avrl_buf_init(&b);
	size_t i = 0;
	for (; i + 3 <= n; i += 3) {
		unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) |
			     (unsigned)in[i + 2];
		avrl_buf_addc(&b, alpha[(v >> 18) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 12) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 6) & 63]);
		avrl_buf_addc(&b, alpha[v & 63]);
	}
	size_t rem = n - i;
	if (rem == 1) {
		unsigned v = (unsigned)in[i] << 16;
		avrl_buf_addc(&b, alpha[(v >> 18) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 12) & 63]);
		avrl_buf_addc(&b, '=');
		avrl_buf_addc(&b, '=');
	} else if (rem == 2) {
		unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
		avrl_buf_addc(&b, alpha[(v >> 18) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 12) & 63]);
		avrl_buf_addc(&b, alpha[(v >> 6) & 63]);
		avrl_buf_addc(&b, '=');
	}
	return b.s ? (b.s[b.len] = '\0', b.s) : (char *)calloc(1, 1);
}

static int digest_hex(const EVP_MD *(*mdfn)(void), const char *data, size_t len,
		      vrl_value **out, char **err)
{
	unsigned char dig[EVP_MAX_MD_SIZE];
	unsigned diglen = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx) {
		*err = vrl_errf("openssl: out of memory");
		return 0;
	}
	if (EVP_DigestInit_ex(ctx, mdfn(), NULL) != 1 ||
	    EVP_DigestUpdate(ctx, data, len) != 1 ||
	    EVP_DigestFinal_ex(ctx, dig, &diglen) != 1) {
		EVP_MD_CTX_free(ctx);
		*err = vrl_errf("openssl: digest failed");
		return 0;
	}
	EVP_MD_CTX_free(ctx);
	char *hex = hex_encode(dig, diglen);
	*out = vrl_bytes_take(hex, diglen * 2);
	return 1;
}

/* ================================================================== */
/* md5 / sha1 / sha2 / sha3                                           */
/* ================================================================== */

static vrl_status fn_md5(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s;
	size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err))
		return VRL_ERR;
	if (!digest_hex(EVP_md5, s, n, out, err))
		return VRL_ERR;
	return VRL_OK;
}

static vrl_status fn_sha1(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s;
	size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err))
		return VRL_ERR;
	if (!digest_hex(EVP_sha1, s, n, out, err))
		return VRL_ERR;
	return VRL_OK;
}

static const EVP_MD *sha2_variant(const char *v)
{
	if (!v || !strcmp(v, "SHA-512/256"))
		return EVP_sha512_256();
	if (!strcmp(v, "SHA-512/224"))
		return EVP_sha512_224();
	if (!strcmp(v, "SHA-224"))
		return EVP_sha224();
	if (!strcmp(v, "SHA-256"))
		return EVP_sha256();
	if (!strcmp(v, "SHA-384"))
		return EVP_sha384();
	if (!strcmp(v, "SHA-512"))
		return EVP_sha512();
	return NULL;
}

static vrl_status fn_sha2(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s;
	size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err))
		return VRL_ERR;
	vrl_value *vv = ARG("variant", 1);
	const char *variant = (vv && vv->type == VRL_BYTES) ? vv->u.bytes.data : "SHA-512/256";
	const EVP_MD *md = sha2_variant(variant);
	if (!md) {
		*err = vrl_errf("sha2: unknown variant '%s'", variant);
		return VRL_ERR;
	}
	unsigned char dig[EVP_MAX_MD_SIZE];
	unsigned diglen = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx || EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
	    EVP_DigestUpdate(ctx, s, n) != 1 ||
	    EVP_DigestFinal_ex(ctx, dig, &diglen) != 1) {
		if (ctx)
			EVP_MD_CTX_free(ctx);
		*err = vrl_errf("sha2: digest failed");
		return VRL_ERR;
	}
	EVP_MD_CTX_free(ctx);
	char *hex = hex_encode(dig, diglen);
	*out = vrl_bytes_take(hex, diglen * 2);
	return VRL_OK;
}

static const EVP_MD *sha3_variant(const char *v)
{
	if (!v || !strcmp(v, "SHA3-512"))
		return EVP_sha3_512();
	if (!strcmp(v, "SHA3-224"))
		return EVP_sha3_224();
	if (!strcmp(v, "SHA3-256"))
		return EVP_sha3_256();
	if (!strcmp(v, "SHA3-384"))
		return EVP_sha3_384();
	return NULL;
}

static vrl_status fn_sha3(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *s;
	size_t n;
	if (!avrl_arg_str(a, "value", 0, &s, &n, err))
		return VRL_ERR;
	vrl_value *vv = ARG("variant", 1);
	const char *variant = (vv && vv->type == VRL_BYTES) ? vv->u.bytes.data : "SHA3-512";
	const EVP_MD *md = sha3_variant(variant);
	if (!md) {
		*err = vrl_errf("sha3: unknown variant '%s'", variant);
		return VRL_ERR;
	}
	unsigned char dig[EVP_MAX_MD_SIZE];
	unsigned diglen = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx || EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
	    EVP_DigestUpdate(ctx, s, n) != 1 ||
	    EVP_DigestFinal_ex(ctx, dig, &diglen) != 1) {
		if (ctx)
			EVP_MD_CTX_free(ctx);
		*err = vrl_errf("sha3: digest failed");
		return VRL_ERR;
	}
	EVP_MD_CTX_free(ctx);
	char *hex = hex_encode(dig, diglen);
	*out = vrl_bytes_take(hex, diglen * 2);
	return VRL_OK;
}

/* ================================================================== */
/* hmac                                                               */
/* ================================================================== */

static const EVP_MD *hmac_md(const char *algo)
{
	if (!algo || !strcmp(algo, "SHA-256") || !strcmp(algo, "SHA256"))
		return EVP_sha256();
	if (!strcmp(algo, "SHA1") || !strcmp(algo, "SHA-1"))
		return EVP_sha1();
	if (!strcmp(algo, "SHA-224") || !strcmp(algo, "SHA224"))
		return EVP_sha224();
	if (!strcmp(algo, "SHA-384") || !strcmp(algo, "SHA384"))
		return EVP_sha384();
	if (!strcmp(algo, "SHA-512") || !strcmp(algo, "SHA512"))
		return EVP_sha512();
	if (!strcmp(algo, "MD5"))
		return EVP_md5();
	return NULL;
}

static vrl_status fn_hmac(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *val, *key;
	size_t vlen, klen;
	if (!avrl_arg_str(a, "value", 0, &val, &vlen, err))
		return VRL_ERR;
	if (!avrl_arg_str(a, "key", 1, &key, &klen, err))
		return VRL_ERR;
	vrl_value *av = ARG("algorithm", 2);
	const char *algo = (av && av->type == VRL_BYTES) ? av->u.bytes.data : "SHA-256";
	const EVP_MD *md = hmac_md(algo);
	if (!md) {
		*err = vrl_errf("hmac: unknown algorithm '%s'", algo);
		return VRL_ERR;
	}
	unsigned char dig[EVP_MAX_MD_SIZE];
	unsigned diglen = 0;
	if (!HMAC(md, key, (int)klen, (const unsigned char *)val, vlen, dig, &diglen)) {
		*err = vrl_errf("hmac: failed");
		return VRL_ERR;
	}
	*out = vrl_bytes((const char *)dig, diglen);
	return VRL_OK;
}

/* ================================================================== */
/* encrypt / decrypt (AES + ChaCha20-Poly1305)                        */
/* ================================================================== */

typedef struct {
	const char *name;
	const EVP_CIPHER *(*cipher)(void);
	int key_len;
	int iv_len;
	int is_aead;
	int is_cbc;
} cipher_spec;

static const cipher_spec CIPHERS[] = {
    {"AES-256-CFB", EVP_aes_256_cfb128, 32, 16, 0, 0},
    {"AES-192-CFB", EVP_aes_192_cfb128, 24, 16, 0, 0},
    {"AES-128-CFB", EVP_aes_128_cfb128, 16, 16, 0, 0},
    {"AES-256-OFB", EVP_aes_256_ofb, 32, 16, 0, 0},
    {"AES-192-OFB", EVP_aes_192_ofb, 24, 16, 0, 0},
    {"AES-128-OFB", EVP_aes_128_ofb, 16, 16, 0, 0},
    {"AES-256-CTR", EVP_aes_256_ctr, 32, 16, 0, 0},
    {"AES-192-CTR", EVP_aes_192_ctr, 24, 16, 0, 0},
    {"AES-128-CTR", EVP_aes_128_ctr, 16, 16, 0, 0},
    {"AES-256-CTR-BE", EVP_aes_256_ctr, 32, 16, 0, 0},
    {"AES-192-CTR-BE", EVP_aes_192_ctr, 24, 16, 0, 0},
    {"AES-128-CTR-BE", EVP_aes_128_ctr, 16, 16, 0, 0},
    {"AES-256-CBC-PKCS7", EVP_aes_256_cbc, 32, 16, 0, 1},
    {"AES-192-CBC-PKCS7", EVP_aes_192_cbc, 24, 16, 0, 1},
    {"AES-128-CBC-PKCS7", EVP_aes_128_cbc, 16, 16, 0, 1},
    {"CHACHA20-POLY1305", EVP_chacha20_poly1305, 32, 12, 1, 0},
    {NULL, NULL, 0, 0, 0, 0},
};

static const cipher_spec *find_cipher(const char *name)
{
	for (int i = 0; CIPHERS[i].name; i++) {
		if (!strcasecmp(CIPHERS[i].name, name))
			return &CIPHERS[i];
	}
	return NULL;
}

static vrl_status cipher_crypt(int do_encrypt, vrl_call_args *a, vrl_value **out, char **err)
{
	const char *data, *algo, *key, *iv;
	size_t dlen, alen, klen, ivlen;
	const char *data_name = do_encrypt ? "plaintext" : "ciphertext";
	/* Prefer the VRL name; fall back to positional index 0. */
	if (!avrl_arg_str(a, data_name, 0, &data, &dlen, err))
		return VRL_ERR;
	if (!avrl_arg_str(a, "algorithm", 1, &algo, &alen, err))
		return VRL_ERR;
	if (!avrl_arg_str(a, "key", 2, &key, &klen, err))
		return VRL_ERR;
	if (!avrl_arg_str(a, "iv", 3, &iv, &ivlen, err))
		return VRL_ERR;

	const cipher_spec *spec = find_cipher(algo);
	if (!spec) {
		*err = vrl_errf("%s: unsupported algorithm '%s'",
				do_encrypt ? "encrypt" : "decrypt", algo);
		return VRL_ERR;
	}
	if ((int)klen != spec->key_len) {
		*err = vrl_errf("%s: key length %zu does not match required %d",
				do_encrypt ? "encrypt" : "decrypt", klen, spec->key_len);
		return VRL_ERR;
	}
	if ((int)ivlen != spec->iv_len) {
		*err = vrl_errf("%s: iv length %zu does not match required %d",
				do_encrypt ? "encrypt" : "decrypt", ivlen, spec->iv_len);
		return VRL_ERR;
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		*err = vrl_errf("openssl: out of memory");
		return VRL_ERR;
	}

	size_t out_cap = dlen + EVP_MAX_BLOCK_LENGTH + (spec->is_aead ? 16 : 0);
	unsigned char *outbuf = malloc(out_cap);
	int outlen = 0, tmplen = 0;

	int ok = 1;
	if (do_encrypt) {
		ok = EVP_EncryptInit_ex(ctx, spec->cipher(), NULL,
					(const unsigned char *)key,
					(const unsigned char *)iv) == 1;
		if (ok && !spec->is_cbc)
			EVP_CIPHER_CTX_set_padding(ctx, 0);
		if (ok)
			ok = EVP_EncryptUpdate(ctx, outbuf, &outlen,
					       (const unsigned char *)data, (int)dlen) == 1;
		if (ok)
			ok = EVP_EncryptFinal_ex(ctx, outbuf + outlen, &tmplen) == 1;
		outlen += tmplen;
		if (ok && spec->is_aead) {
			ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
						 outbuf + outlen) == 1;
			outlen += 16;
		}
	} else {
		size_t ct_len = dlen;
		const unsigned char *ct = (const unsigned char *)data;
		unsigned char tag[16];
		if (spec->is_aead) {
			if (dlen < 16) {
				free(outbuf);
				EVP_CIPHER_CTX_free(ctx);
				*err = vrl_errf("decrypt: ciphertext too short for AEAD tag");
				return VRL_ERR;
			}
			ct_len = dlen - 16;
			memcpy(tag, data + ct_len, 16);
		}
		ok = EVP_DecryptInit_ex(ctx, spec->cipher(), NULL,
					(const unsigned char *)key,
					(const unsigned char *)iv) == 1;
		if (ok && !spec->is_cbc)
			EVP_CIPHER_CTX_set_padding(ctx, 0);
		if (ok && spec->is_aead)
			ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, tag) == 1;
		if (ok)
			ok = EVP_DecryptUpdate(ctx, outbuf, &outlen, ct, (int)ct_len) == 1;
		if (ok)
			ok = EVP_DecryptFinal_ex(ctx, outbuf + outlen, &tmplen) == 1;
		outlen += tmplen;
	}

	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		free(outbuf);
		*err = vrl_errf("%s: cipher operation failed", do_encrypt ? "encrypt" : "decrypt");
		return VRL_ERR;
	}
	*out = vrl_bytes_take((char *)outbuf, (size_t)outlen);
	return VRL_OK;
}

static vrl_status fn_encrypt(vrl_call_args *a, vrl_value **out, char **err)
{
	return cipher_crypt(1, a, out, err);
}

static vrl_status fn_decrypt(vrl_call_args *a, vrl_value **out, char **err)
{
	return cipher_crypt(0, a, out, err);
}

/* ================================================================== */
/* community_id (https://github.com/corelight/community-id-spec)      */
/* ================================================================== */

static int ip_to_bytes(const char *s, unsigned char *buf, size_t *len, char **err)
{
	if (inet_pton(AF_INET, s, buf) == 1) {
		*len = 4;
		return 1;
	}
	if (inet_pton(AF_INET6, s, buf) == 1) {
		*len = 16;
		return 1;
	}
	*err = vrl_errf("community_id: invalid IP '%s'", s);
	return 0;
}

static int addr_lt(const unsigned char *a, size_t alen, uint16_t aport,
		   const unsigned char *b, size_t blen, uint16_t bport)
{
	if (alen != blen)
		return alen < blen;
	int cmp = memcmp(a, b, alen);
	if (cmp < 0)
		return 1;
	if (cmp > 0)
		return 0;
	return aport < bport;
}

static vrl_status fn_community_id(vrl_call_args *a, vrl_value **out, char **err)
{
	const char *sip, *dip;
	size_t siplen, diplen;
	if (!avrl_arg_str(a, "source_ip", 0, &sip, &siplen, err))
		return VRL_ERR;
	if (!avrl_arg_str(a, "destination_ip", 1, &dip, &diplen, err))
		return VRL_ERR;
	vrl_value *pv = ARG("protocol", 2);
	if (!pv || pv->type != VRL_INTEGER) {
		*err = vrl_errf("community_id: protocol must be an integer");
		return VRL_ERR;
	}
	if (pv->u.integer < 0 || pv->u.integer > 255) {
		*err = vrl_errf("community_id: protocol must be between 0 and 255");
		return VRL_ERR;
	}
	uint8_t proto = (uint8_t)pv->u.integer;

	vrl_value *spv = ARG("source_port", 3);
	vrl_value *dpv = ARG("destination_port", 4);
	vrl_value *seedv = ARG("seed", 5);
	int have_ports = (spv && spv->type == VRL_INTEGER) && (dpv && dpv->type == VRL_INTEGER);
	uint16_t sport = 0, dport = 0;
	if (have_ports) {
		if (spv->u.integer < 0 || spv->u.integer > 65535 ||
		    dpv->u.integer < 0 || dpv->u.integer > 65535) {
			*err = vrl_errf("community_id: ports must be between 0 and 65535");
			return VRL_ERR;
		}
		sport = (uint16_t)spv->u.integer;
		dport = (uint16_t)dpv->u.integer;
	} else if (proto == 6 || proto == 17 || proto == 132) {
		*err = vrl_errf("community_id: src port and dst port should be set when "
				"protocol is tcp/udp/sctp");
		return VRL_ERR;
	}

	uint16_t seed = 0;
	if (seedv && seedv->type == VRL_INTEGER) {
		if (seedv->u.integer < 0 || seedv->u.integer > 65535) {
			*err = vrl_errf("community_id: seed must be between 0 and 65535");
			return VRL_ERR;
		}
		seed = (uint16_t)seedv->u.integer;
	}

	unsigned char sbuf[16], dbuf[16];
	size_t slen = 0, dlen = 0;
	if (!ip_to_bytes(sip, sbuf, &slen, err) || !ip_to_bytes(dip, dbuf, &dlen, err))
		return VRL_ERR;
	if (slen != dlen) {
		*err = vrl_errf("community_id: source and destination IP versions differ");
		return VRL_ERR;
	}

	/* Canonicalize endpoints for bidirectional flows. */
	const unsigned char *a1 = sbuf, *a2 = dbuf;
	uint16_t p1 = sport, p2 = dport;
	if (!addr_lt(sbuf, slen, sport, dbuf, dlen, dport)) {
		a1 = dbuf;
		a2 = sbuf;
		p1 = dport;
		p2 = sport;
	}

	unsigned char input[2 + 16 + 16 + 1 + 1 + 2 + 2];
	size_t off = 0;
	uint16_t seed_be = htons(seed);
	memcpy(input + off, &seed_be, 2);
	off += 2;
	memcpy(input + off, a1, slen);
	off += slen;
	memcpy(input + off, a2, dlen);
	off += dlen;
	input[off++] = proto;
	input[off++] = 0;
	/* Ports / ICMP type+code when provided. Portless protocols (e.g. RSVP) omit. */
	if (have_ports) {
		uint16_t p1be = htons(p1), p2be = htons(p2);
		memcpy(input + off, &p1be, 2);
		off += 2;
		memcpy(input + off, &p2be, 2);
		off += 2;
	}

	unsigned char dig[EVP_MAX_MD_SIZE];
	unsigned diglen = 0;
	EVP_MD_CTX *hctx = EVP_MD_CTX_new();
	if (!hctx || EVP_DigestInit_ex(hctx, EVP_sha1(), NULL) != 1 ||
	    EVP_DigestUpdate(hctx, input, off) != 1 ||
	    EVP_DigestFinal_ex(hctx, dig, &diglen) != 1) {
		if (hctx)
			EVP_MD_CTX_free(hctx);
		*err = vrl_errf("community_id: sha1 failed");
		return VRL_ERR;
	}
	EVP_MD_CTX_free(hctx);
	char *b64 = b64_encode(dig, diglen);
	size_t blen = strlen(b64);
	char *res = malloc(2 + blen + 1);
	res[0] = '1';
	res[1] = ':';
	memcpy(res + 2, b64, blen + 1);
	free(b64);
	*out = vrl_bytes_take(res, 2 + blen);
	return VRL_OK;
}

/* ================================================================== */
/* encrypt_ip / decrypt_ip (ipcrypt-deterministic + ipcrypt-pfx)      */
/* ================================================================== */

static int ip_to_16(const char *s, unsigned char out[16], char **err)
{
	unsigned char tmp[16];
	if (inet_pton(AF_INET, s, tmp) == 1) {
		memset(out, 0, 10);
		out[10] = 0xff;
		out[11] = 0xff;
		memcpy(out + 12, tmp, 4);
		return 1;
	}
	if (inet_pton(AF_INET6, s, out) == 1)
		return 1;
	*err = vrl_errf("invalid IP address '%s'", s);
	return 0;
}

static int is_ipv4_mapped(const unsigned char b[16])
{
	static const unsigned char pref[12] = {
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
	return memcmp(b, pref, 12) == 0;
}

static void bytes16_to_ip(const unsigned char b[16], char *out, size_t outsz)
{
	if (is_ipv4_mapped(b)) {
		inet_ntop(AF_INET, b + 12, out, (socklen_t)outsz);
		return;
	}
	inet_ntop(AF_INET6, b, out, (socklen_t)outsz);
}

static int aes128_ecb_block(const unsigned char key[16], const unsigned char in[16],
			    unsigned char out[16], int encrypt)
{
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return 0;
	int ok = 1, outlen = 0, tmplen = 0;
	if (encrypt)
		ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) == 1;
	else
		ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) == 1;
	if (ok)
		EVP_CIPHER_CTX_set_padding(ctx, 0);
	if (ok) {
		if (encrypt)
			ok = EVP_EncryptUpdate(ctx, out, &outlen, in, 16) == 1 &&
			     EVP_EncryptFinal_ex(ctx, out + outlen, &tmplen) == 1;
		else
			ok = EVP_DecryptUpdate(ctx, out, &outlen, in, 16) == 1 &&
			     EVP_DecryptFinal_ex(ctx, out + outlen, &tmplen) == 1;
	}
	EVP_CIPHER_CTX_free(ctx);
	return ok;
}

static int get_bit(const unsigned char data[16], int position)
{
	int byte_index = 15 - (position / 8);
	int bit_index = position % 8;
	return (data[byte_index] >> bit_index) & 1;
}

static void set_bit(unsigned char data[16], int position, int value)
{
	int byte_index = 15 - (position / 8);
	int bit_index = position % 8;
	if (value)
		data[byte_index] |= (unsigned char)(1u << bit_index);
	else
		data[byte_index] &= (unsigned char)~(1u << bit_index);
}

static void pad_prefix(unsigned char out[16], int prefix_len_bits)
{
	memset(out, 0, 16);
	if (prefix_len_bits == 0) {
		out[15] = 0x01;
	} else if (prefix_len_bits == 96) {
		out[3] = 0x01;
		out[14] = 0xff;
		out[15] = 0xff;
	}
}

static void shift_left_one_bit(unsigned char data[16])
{
	unsigned char result[16];
	int carry = 0;
	for (int i = 15; i >= 0; i--) {
		result[i] = (unsigned char)(((data[i] << 1) | carry) & 0xff);
		carry = (data[i] >> 7) & 1;
	}
	memcpy(data, result, 16);
}

static int ipcrypt_deterministic(const unsigned char key[16], const unsigned char in[16],
				 unsigned char out[16], int encrypt)
{
	return aes128_ecb_block(key, in, out, encrypt);
}

static int ipcrypt_pfx(const unsigned char key[32], const unsigned char in[16],
		       unsigned char out[16], int encrypt)
{
	const unsigned char *K1 = key;
	const unsigned char *K2 = key + 16;
	if (memcmp(K1, K2, 16) == 0)
		return 0; /* K1 and K2 must differ */

	unsigned char result[16];
	memset(result, 0, 16);
	int prefix_start;

	if (is_ipv4_mapped(in)) {
		prefix_start = 96;
		result[10] = 0xff;
		result[11] = 0xff;
	} else {
		prefix_start = 0;
	}

	unsigned char padded[16];
	pad_prefix(padded, prefix_start);

	for (int prefix_len_bits = prefix_start; prefix_len_bits < 128; prefix_len_bits++) {
		unsigned char e1[16], e2[16], e[16];
		if (!aes128_ecb_block(K1, padded, e1, 1) ||
		    !aes128_ecb_block(K2, padded, e2, 1))
			return 0;
		for (int i = 0; i < 16; i++)
			e[i] = (unsigned char)(e1[i] ^ e2[i]);
		int cipher_bit = get_bit(e, 0);
		int bit_pos = 127 - prefix_len_bits;
		int bit;
		if (encrypt) {
			int original_bit = get_bit(in, bit_pos);
			set_bit(result, bit_pos, cipher_bit ^ original_bit);
			bit = original_bit;
		} else {
			int encrypted_bit = get_bit(in, bit_pos);
			int original_bit = cipher_bit ^ encrypted_bit;
			set_bit(result, bit_pos, original_bit);
			bit = original_bit;
		}
		shift_left_one_bit(padded);
		set_bit(padded, 0, bit);
	}
	memcpy(out, result, 16);
	return 1;
}

static vrl_status ipcrypt_fn(int encrypt, vrl_call_args *a, vrl_value **out, char **err)
{
	const char *ip, *key, *mode;
	size_t iplen, klen, mlen;
	if (!avrl_arg_str(a, "ip", 0, &ip, &iplen, err))
		return VRL_ERR;
	if (!avrl_arg_str(a, "key", 1, &key, &klen, err))
		return VRL_ERR;
	if (!avrl_arg_str(a, "mode", 2, &mode, &mlen, err))
		return VRL_ERR;

	unsigned char in16[16], out16[16];
	if (!ip_to_16(ip, in16, err))
		return VRL_ERR;

	char ipstr[INET6_ADDRSTRLEN];
	if (!strcasecmp(mode, "aes128")) {
		if (klen != 16) {
			*err = vrl_errf("%s_ip: aes128 key must be 16 bytes",
					encrypt ? "encrypt" : "decrypt");
			return VRL_ERR;
		}
		if (!ipcrypt_deterministic((const unsigned char *)key, in16, out16, encrypt)) {
			*err = vrl_errf("%s_ip: AES failed", encrypt ? "encrypt" : "decrypt");
			return VRL_ERR;
		}
	} else if (!strcasecmp(mode, "pfx")) {
		if (klen != 32) {
			*err = vrl_errf("%s_ip: pfx key must be 32 bytes",
					encrypt ? "encrypt" : "decrypt");
			return VRL_ERR;
		}
		if (!ipcrypt_pfx((const unsigned char *)key, in16, out16, encrypt)) {
			*err = vrl_errf("%s_ip: pfx failed (check key halves differ)",
					encrypt ? "encrypt" : "decrypt");
			return VRL_ERR;
		}
	} else {
		*err = vrl_errf("%s_ip: unsupported mode '%s' (use aes128 or pfx)",
				encrypt ? "encrypt" : "decrypt", mode);
		return VRL_ERR;
	}

	bytes16_to_ip(out16, ipstr, sizeof(ipstr));
	*out = vrl_bytes_cstr(ipstr);
	return VRL_OK;
}

static vrl_status fn_encrypt_ip(vrl_call_args *a, vrl_value **out, char **err)
{
	return ipcrypt_fn(1, a, out, err);
}

static vrl_status fn_decrypt_ip(vrl_call_args *a, vrl_value **out, char **err)
{
	return ipcrypt_fn(0, a, out, err);
}

/* ================================================================== */
/* registration                                                       */
/* ================================================================== */

void vrl_reg_crypto(void)
{
	vrl_register("md5", fn_md5);
	vrl_register("sha1", fn_sha1);
	vrl_register("sha2", fn_sha2);
	vrl_register("sha3", fn_sha3);
	vrl_register("hmac", fn_hmac);
	vrl_register("encrypt", fn_encrypt);
	vrl_register("decrypt", fn_decrypt);
	vrl_register("community_id", fn_community_id);
	vrl_register("encrypt_ip", fn_encrypt_ip);
	vrl_register("decrypt_ip", fn_decrypt_ip);
}

#endif /* AVRL_WITH_OPENSSL */
