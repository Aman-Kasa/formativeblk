#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <openssl/evp.h>

#define HASH_HEX_LEN 65        /* 64 hex chars + NUL, matches Block.hash */
#define MAX_SIGNATURE_LEN 72   /* matches Block.signature buffer size    */

/* Computes SHA-256 over `data` (length `len`) and writes it as a lowercase
 * hex string (64 chars + NUL) into `out_hex`. This is what fills a Block's
 * `hash` and `previous_hash` fields. */
void crypto_sha256_hex(const unsigned char *data, size_t len, char out_hex[HASH_HEX_LEN]);

/* Loads the ECDSA (P-256) keypair from priv_path/pub_path if both exist;
 * otherwise generates a new keypair and writes it to those paths (PEM,
 * private key file permissions restricted to the owner). Returns an
 * EVP_PKEY* the caller must release with EVP_PKEY_free, or NULL on failure. */
EVP_PKEY *crypto_load_or_create_keypair(const char *priv_path, const char *pub_path);

/* Loads only the public key from pub_path (used to verify without ever
 * touching the private key). Returns NULL on failure. */
EVP_PKEY *crypto_load_public_key(const char *pub_path);

/* Signs `hash_hex` (the block's already-computed SHA-256 hex string, treated
 * as the message) with ECDSA using `pkey`. Writes the DER signature into
 * sig_out (capacity sig_out_cap) and its length into *sig_len_out.
 * Returns 1 on success, 0 on failure. */
int crypto_sign_hash(EVP_PKEY *pkey, const char *hash_hex,
                      unsigned char *sig_out, size_t sig_out_cap, size_t *sig_len_out);

/* Verifies that `sig` (length sig_len) is a valid ECDSA signature of
 * `hash_hex` under `pkey`. Returns 1 if valid, 0 if not (including on any
 * internal error — a signature is never treated as valid by default). */
int crypto_verify_hash(EVP_PKEY *pkey, const char *hash_hex,
                        const unsigned char *sig, size_t sig_len);

/* Hex <-> bytes helpers used to store the signature/hash fields as text in
 * the persistence file. Both are bounds-checked and never overrun the
 * caller-supplied buffer. */
void crypto_bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex, size_t out_cap);
size_t crypto_hex_to_bytes(const char *hex, unsigned char *out, size_t out_cap);

#endif
