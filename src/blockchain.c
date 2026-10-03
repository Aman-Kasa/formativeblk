#include "blockchain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void blockchain_zero_hash(char out[HASH_HEX_LEN]) {
    memset(out, '0', HASH_HEX_LEN - 1);
    out[HASH_HEX_LEN - 1] = '\0';
}

void blockchain_init(Blockchain *chain) {
    chain->head = NULL;
    chain->tail = NULL;
    chain->length = 0;
}

void blockchain_free(Blockchain *chain) {
    Block *cur = chain->head;
    while (cur) {
        Block *next = cur->next;
        free(cur);
        cur = next;
    }
    chain->head = chain->tail = NULL;
    chain->length = 0;
}

/* Serializes every hashed field (everything except `signature` and `hash`
 * itself) into one buffer in a fixed order, per ARCHITECTURE.md's
 * cryptographic flow. */
static void serialize_for_hash(const Block *b, char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%d|%ld|%s|%s|%s|%s|%s|%s",
              b->index, (long)b->timestamp,
              b->book_id, b->book_title,
              b->member_id, b->member_name,
              b->action, b->previous_hash);
}

static int seal_block(Block *b, EVP_PKEY *pkey) {
    char buf[512];
    serialize_for_hash(b, buf, sizeof(buf));
    crypto_sha256_hex((const unsigned char *)buf, strlen(buf), b->hash);
    if (b->hash[0] == '\0') return 0; /* crypto_sha256_hex signals failure with an empty string */

    if (!crypto_sign_hash(pkey, b->hash, b->signature, sizeof(b->signature), &b->sig_len)) {
        return 0;
    }
    return 1;
}

static void append(Blockchain *chain, Block *b) {
    b->next = NULL;
    if (chain->tail) {
        chain->tail->next = b;
    } else {
        chain->head = b;
    }
    chain->tail = b;
    chain->length++;
}

int blockchain_create_genesis(Blockchain *chain, EVP_PKEY *pkey) {
    if (chain->length != 0) {
        fprintf(stderr, "ERROR: blockchain_create_genesis called on a non-empty chain.\n");
        return 0;
    }

    Block *b = calloc(1, sizeof(Block));
    if (!b) {
        fprintf(stderr, "ERROR: out of memory creating genesis block.\n");
        return 0;
    }
    b->index = 0;
    b->timestamp = time(NULL);
    /* book_id/book_title/member_id/member_name are left as empty strings —
     * genesis carries no lending record. */
    snprintf(b->action, ACTION_LEN, "GENESIS");
    blockchain_zero_hash(b->previous_hash);

    if (!seal_block(b, pkey)) {
        free(b);
        return 0;
    }

    append(chain, b);
    return 1;
}

static const Block *find_last_for_book(const Blockchain *chain, const char *book_id) {
    const Block *result = NULL;
    for (const Block *b = chain->head; b; b = b->next) {
        if (b->index == 0) continue; /* genesis carries no book */
        if (strcmp(b->book_id, book_id) == 0) result = b;
    }
    return result;
}

int blockchain_book_is_borrowed(const Blockchain *chain, const char *book_id) {
    const Block *last = find_last_for_book(chain, book_id);
    return last != NULL && strcmp(last->action, "BORROWED") == 0;
}

LendResult blockchain_borrow(Blockchain *chain, const Registry *reg, EVP_PKEY *pkey,
                              const char *book_id, const char *member_id) {
    const Book *book = registry_find_book(reg, book_id);
    const Member *member = registry_find_member(reg, member_id);
    if (!book || !member) {
        return !book ? LEND_ERR_BOOK_NOT_FOUND : LEND_ERR_MEMBER_NOT_FOUND;
    }
    if (blockchain_book_is_borrowed(chain, book_id)) {
        return LEND_ERR_ALREADY_BORROWED;
    }

    Block *b = calloc(1, sizeof(Block));
    if (!b) return LEND_ERR_CRYPTO;

    b->index = (int)chain->length;
    b->timestamp = time(NULL);
    snprintf(b->book_id, BOOK_ID_LEN, "%s", book->book_id);
    snprintf(b->book_title, BOOK_TITLE_LEN, "%s", book->title);
    snprintf(b->member_id, MEMBER_ID_LEN, "%s", member->member_id);
    snprintf(b->member_name, MEMBER_NAME_LEN, "%s", member->full_name);
    snprintf(b->action, ACTION_LEN, "BORROWED");
    snprintf(b->previous_hash, HASH_HEX_LEN, "%s", chain->tail->hash);

    if (!seal_block(b, pkey)) {
        free(b);
        return LEND_ERR_CRYPTO;
    }

    append(chain, b);
    return LEND_OK;
}

LendResult blockchain_return(Blockchain *chain, const Registry *reg, EVP_PKEY *pkey,
                              const char *book_id) {
    const Book *book = registry_find_book(reg, book_id);
    if (!book) return LEND_ERR_BOOK_NOT_FOUND;

    const Block *last = find_last_for_book(chain, book_id);
    if (!last || strcmp(last->action, "BORROWED") != 0) {
        return LEND_ERR_NOT_BORROWED;
    }

    Block *b = calloc(1, sizeof(Block));
    if (!b) return LEND_ERR_CRYPTO;

    b->index = (int)chain->length;
    b->timestamp = time(NULL);
    snprintf(b->book_id, BOOK_ID_LEN, "%s", last->book_id);
    snprintf(b->book_title, BOOK_TITLE_LEN, "%s", last->book_title);
    snprintf(b->member_id, MEMBER_ID_LEN, "%s", last->member_id);
    snprintf(b->member_name, MEMBER_NAME_LEN, "%s", last->member_name);
    snprintf(b->action, ACTION_LEN, "RETURNED");
    snprintf(b->previous_hash, HASH_HEX_LEN, "%s", chain->tail->hash);

    if (!seal_block(b, pkey)) {
        free(b);
        return LEND_ERR_CRYPTO;
    }

    append(chain, b);
    return LEND_OK;
}

ChainValidation blockchain_validate(const Blockchain *chain, EVP_PKEY *pub_key) {
    ChainValidation result = { .valid = 1, .bad_index = -1, .reason = NULL };

    if (!chain->head) {
        result.valid = 0;
        result.bad_index = -1;
        result.reason = "chain is empty (no genesis block)";
        return result;
    }

    char expected_prev_hash[HASH_HEX_LEN];
    blockchain_zero_hash(expected_prev_hash);
    int expected_index = 0;

    for (const Block *b = chain->head; b; b = b->next) {
        if (b->index != expected_index) {
            result.valid = 0;
            result.bad_index = b->index;
            result.reason = "block index out of sequence";
            return result;
        }
        if (strcmp(b->previous_hash, expected_prev_hash) != 0) {
            result.valid = 0;
            result.bad_index = b->index;
            result.reason = "previous_hash does not match the prior block's hash";
            return result;
        }

        char buf[512];
        serialize_for_hash(b, buf, sizeof(buf));
        char recomputed[HASH_HEX_LEN];
        crypto_sha256_hex((const unsigned char *)buf, strlen(buf), recomputed);
        if (strcmp(recomputed, b->hash) != 0) {
            result.valid = 0;
            result.bad_index = b->index;
            result.reason = "stored hash does not match recomputed hash (block data was modified)";
            return result;
        }

        if (!crypto_verify_hash(pub_key, b->hash, b->signature, b->sig_len)) {
            result.valid = 0;
            result.bad_index = b->index;
            result.reason = "ECDSA signature is invalid for this block's hash";
            return result;
        }

        snprintf(expected_prev_hash, HASH_HEX_LEN, "%s", b->hash);
        expected_index++;
    }

    return result;
}

const char *lend_result_message(LendResult r) {
    switch (r) {
        case LEND_OK: return "OK";
        case LEND_ERR_BOOK_NOT_FOUND:
        case LEND_ERR_MEMBER_NOT_FOUND:
            return "ERROR: Book or Member not found";
        case LEND_ERR_ALREADY_BORROWED:
            return "ERROR: this book is already on loan";
        case LEND_ERR_NOT_BORROWED:
            return "ERROR: this book was never borrowed, or has already been returned";
        case LEND_ERR_CRYPTO:
            return "ERROR: internal cryptographic operation failed";
    }
    return "ERROR: unknown";
}
