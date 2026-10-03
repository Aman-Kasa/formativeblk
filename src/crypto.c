#include "crypto.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <openssl/pem.h>
#include <openssl/err.h>

static void print_openssl_errors(const char *context) {
    fprintf(stderr, "ERROR: %s (OpenSSL: %s)\n", context, ERR_error_string(ERR_get_error(), NULL));
}

void crypto_sha256_hex(const unsigned char *data, size_t len, char out_hex[HASH_HEX_LEN]) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    /* EVP_Digest is the one-shot EVP API — no deprecated SHA256_Init/Update/Final calls. */
    if (!EVP_Digest(data, len, digest, &digest_len, EVP_sha256(), NULL)) {
        print_openssl_errors("SHA-256 hashing failed");
        out_hex[0] = '\0';
        return;
    }

    crypto_bytes_to_hex(digest, digest_len, out_hex, HASH_HEX_LEN);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static EVP_PKEY *generate_and_save_keypair(const char *priv_path, const char *pub_path) {
    EVP_PKEY *pkey = EVP_EC_gen("P-256");
    if (!pkey) {
        print_openssl_errors("ECDSA (P-256) key generation failed");
        return NULL;
    }

    FILE *priv_f = fopen(priv_path, "w");
    if (!priv_f) {
        fprintf(stderr, "ERROR: could not open '%s' to write the private key.\n", priv_path);
        EVP_PKEY_free(pkey);
        return NULL;
    }
    /* Restrict the private key file to the owner before writing key material into it. */
    chmod(priv_path, S_IRUSR | S_IWUSR);
    if (!PEM_write_PrivateKey(priv_f, pkey, NULL, NULL, 0, NULL, NULL)) {
        print_openssl_errors("writing private key failed");
        fclose(priv_f);
        EVP_PKEY_free(pkey);
        return NULL;
    }
    fclose(priv_f);

    FILE *pub_f = fopen(pub_path, "w");
    if (!pub_f) {
        fprintf(stderr, "ERROR: could not open '%s' to write the public key.\n", pub_path);
        EVP_PKEY_free(pkey);
        return NULL;
    }
    if (!PEM_write_PUBKEY(pub_f, pkey)) {
        print_openssl_errors("writing public key failed");
        fclose(pub_f);
        EVP_PKEY_free(pkey);
        return NULL;
    }
    fclose(pub_f);

    return pkey;
}

EVP_PKEY *crypto_load_or_create_keypair(const char *priv_path, const char *pub_path) {
    if (file_exists(priv_path) && file_exists(pub_path)) {
        FILE *f = fopen(priv_path, "r");
        if (!f) {
            fprintf(stderr, "ERROR: could not open existing private key '%s'.\n", priv_path);
            return NULL;
        }
        EVP_PKEY *pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL);
        fclose(f);
        if (!pkey) {
            print_openssl_errors("could not parse existing private key");
            return NULL;
        }
        return pkey;
    }

    return generate_and_save_keypair(priv_path, pub_path);
}

EVP_PKEY *crypto_load_public_key(const char *pub_path) {
    FILE *f = fopen(pub_path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: could not open public key '%s'.\n", pub_path);
        return NULL;
    }
    EVP_PKEY *pkey = PEM_read_PUBKEY(f, NULL, NULL, NULL);
    fclose(f);
    if (!pkey) {
        print_openssl_errors("could not parse public key");
    }
    return pkey;
}

int crypto_sign_hash(EVP_PKEY *pkey, const char *hash_hex,
                      unsigned char *sig_out, size_t sig_out_cap, size_t *sig_len_out) {
    int ok = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        print_openssl_errors("EVP_MD_CTX_new failed");
        return 0;
    }

    size_t needed_len = 0;
    if (EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey) != 1) {
        print_openssl_errors("EVP_DigestSignInit failed");
        goto done;
    }
    /* First call with a NULL buffer reports how many bytes the signature needs. */
    if (EVP_DigestSign(ctx, NULL, &needed_len, (const unsigned char *)hash_hex, strlen(hash_hex)) != 1) {
        print_openssl_errors("EVP_DigestSign (size query) failed");
        goto done;
    }
    if (needed_len > sig_out_cap) {
        fprintf(stderr, "ERROR: signature (%zu bytes) does not fit in the %zu-byte block signature field.\n",
                needed_len, sig_out_cap);
        goto done;
    }

    size_t sig_len = sig_out_cap;
    if (EVP_DigestSign(ctx, sig_out, &sig_len, (const unsigned char *)hash_hex, strlen(hash_hex)) != 1) {
        print_openssl_errors("EVP_DigestSign failed");
        goto done;
    }

    *sig_len_out = sig_len;
    ok = 1;

done:
    EVP_MD_CTX_free(ctx);
    return ok;
}

int crypto_verify_hash(EVP_PKEY *pkey, const char *hash_hex,
                        const unsigned char *sig, size_t sig_len) {
    int valid = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        print_openssl_errors("EVP_MD_CTX_new failed");
        return 0;
    }

    if (EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey) != 1) {
        print_openssl_errors("EVP_DigestVerifyInit failed");
        goto done;
    }

    /* EVP_DigestVerify returns 1 = valid signature, 0 = signature mismatch,
     * <0 = an unrelated error. Both non-1 cases are treated as "not valid" —
     * verification never fails open. */
    int rc = EVP_DigestVerify(ctx, sig, sig_len, (const unsigned char *)hash_hex, strlen(hash_hex));
    valid = (rc == 1);

done:
    EVP_MD_CTX_free(ctx);
    return valid;
}

void crypto_bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex, size_t out_cap) {
    static const char digits[] = "0123456789abcdef";
    size_t needed = len * 2 + 1;
    if (out_cap < needed) {
        /* Truncate safely rather than overrun — callers size buffers from
         * known constants (HASH_HEX_LEN, MAX_SIGNATURE_LEN*2+1) so this path
         * is a defensive backstop, not an expected case. */
        if (out_cap > 0) out_hex[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2]     = digits[(bytes[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = digits[bytes[i] & 0xF];
    }
    out_hex[len * 2] = '\0';
}

size_t crypto_hex_to_bytes(const char *hex, unsigned char *out, size_t out_cap) {
    size_t hex_len = strlen(hex);
    size_t byte_len = hex_len / 2;
    if (hex_len % 2 != 0 || byte_len > out_cap) {
        return 0;
    }
    for (size_t i = 0; i < byte_len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            return 0;
        }
        out[i] = (unsigned char)byte;
    }
    return byte_len;
}
