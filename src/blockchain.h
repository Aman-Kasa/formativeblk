#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include <time.h>
#include <stddef.h>

#include "registry.h"
#include "crypto.h"

#define ACTION_LEN 10

typedef struct Block {
    int index;
    time_t timestamp;
    char book_id[BOOK_ID_LEN];
    char book_title[BOOK_TITLE_LEN];
    char member_id[MEMBER_ID_LEN];
    char member_name[MEMBER_NAME_LEN];
    char action[ACTION_LEN];
    char previous_hash[HASH_HEX_LEN];
    unsigned char signature[MAX_SIGNATURE_LEN];
    char hash[HASH_HEX_LEN];

    /* In-memory only: ECDSA (P-256) DER signatures are variable-length, but
     * the assignment's Block struct has no length field alongside
     * `signature`. Tracked here rather than persisted or hashed — see
     * ARCHITECTURE.md "Data structures" for why this doesn't change the
     * spec-defined on-disk block shape. */
    size_t sig_len;

    struct Block *next;
} Block;

typedef struct {
    Block *head;
    Block *tail;
    size_t length;
} Blockchain;

typedef enum {
    LEND_OK = 0,
    LEND_ERR_BOOK_NOT_FOUND,
    LEND_ERR_MEMBER_NOT_FOUND,
    LEND_ERR_ALREADY_BORROWED,
    LEND_ERR_NOT_BORROWED,
    LEND_ERR_CRYPTO,
} LendResult;

typedef struct {
    int valid;              /* 1 = chain intact, 0 = compromised */
    int bad_index;           /* index of the first problem block, -1 if valid */
    const char *reason;      /* static, human-readable reason */
} ChainValidation;

void blockchain_init(Blockchain *chain);
void blockchain_free(Blockchain *chain);

/* Creates and appends the genesis block (index 0, previous_hash = 64 zeros).
 * Must only be called on an empty chain. Returns 1 on success, 0 on failure. */
int blockchain_create_genesis(Blockchain *chain, EVP_PKEY *pkey);

/* Validates book_id/member_id against `reg`, checks loan state, and on
 * success creates, hashes, signs and appends a BORROWED block. */
LendResult blockchain_borrow(Blockchain *chain, const Registry *reg, EVP_PKEY *pkey,
                              const char *book_id, const char *member_id);

/* Finds the book's most recent BORROWED entry and, if found, creates,
 * hashes, signs and appends a RETURNED block. */
LendResult blockchain_return(Blockchain *chain, const Registry *reg, EVP_PKEY *pkey,
                              const char *book_id);

/* True if the book's most recent lending record on the chain is BORROWED. */
int blockchain_book_is_borrowed(const Blockchain *chain, const char *book_id);

/* Walks the full chain verifying: genesis correctness, per-block hash
 * recomputation, previous_hash linkage, signature validity (against
 * `pub_key`), and index/order consistency. Stops at, and reports, the first
 * problem found. */
ChainValidation blockchain_validate(const Blockchain *chain, EVP_PKEY *pub_key);

/* Fills a HASH_HEX_LEN buffer with 64 ASCII '0' characters — the required
 * genesis previous_hash, and the reference value validation compares against. */
void blockchain_zero_hash(char out[HASH_HEX_LEN]);

const char *lend_result_message(LendResult r);

#endif
