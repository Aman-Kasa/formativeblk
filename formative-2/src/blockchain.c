#include "blockchain.h"
#include "config.h"
#include "simclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERIALIZE_BUF_LEN 640
#define SECONDS_PER_DAY 86400L

/* ===================================================================== */
/* Linked-list helpers shared by the chain and the pending pool          */
/* ===================================================================== */

void blocklist_init(BlockList *list) {
    list->head = NULL;
    list->tail = NULL;
    list->length = 0;
}

void blocklist_free(BlockList *list) {
    Block *cur = list->head;
    while (cur) {
        Block *next = cur->next;
        free(cur);
        cur = next;
    }
    blocklist_init(list);
}

void blocklist_append(BlockList *list, Block *b) {
    b->next = NULL;
    if (list->tail) {
        list->tail->next = b;
    } else {
        list->head = b;
    }
    list->tail = b;
    list->length++;
}

Block *blocklist_pop_front(BlockList *list) {
    Block *b = list->head;
    if (!b) return NULL;
    list->head = b->next;
    if (!list->head) list->tail = NULL;
    list->length--;
    b->next = NULL;
    return b;
}

/* ===================================================================== */
/* Hashing and proof of work                                             */
/* ===================================================================== */

void blockchain_zero_hash(char out[HASH_HEX_LEN]) {
    memset(out, '0', HASH_HEX_LEN - 1);
    out[HASH_HEX_LEN - 1] = '\0';
}

/* Serializes every hashed field in a fixed order. The nonce is part of the
 * input, which is what makes proof of work possible: changing the nonce
 * changes the hash, and the miner keeps changing it until the hash meets
 * the difficulty target. token_reward and tx_id are included so a reward
 * cannot be edited after mining without breaking the block's hash. */
static void serialize_for_hash(const Block *b, char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%d|%ld|%s|%s|%s|%s|%s|%s|%d|%lu|%d|%s",
             b->index, (long)b->timestamp,
             b->book_id, b->book_title,
             b->member_id, b->member_name,
             b->action, b->previous_hash,
             b->difficulty, b->nonce,
             b->token_reward, b->tx_id);
}

void block_compute_hash(const Block *b, char out[HASH_HEX_LEN]) {
    char buf[SERIALIZE_BUF_LEN];
    serialize_for_hash(b, buf, sizeof(buf));
    crypto_sha256_hex((const unsigned char *)buf, strlen(buf), out);
}

void block_compute_tx_id(const Block *b, char out[HASH_HEX_LEN]) {
    if (b->token_reward <= 0) {
        out[0] = '\0';
        return;
    }
    /* The reward transaction: SYSTEM pays token_reward to the member, minus
     * the fixed fee, because this book was returned at this time. */
    char buf[SERIALIZE_BUF_LEN];
    snprintf(buf, sizeof(buf), "REWARD|SYSTEM|%s|%s|%d|%d|%ld",
             b->member_id, b->book_id, b->token_reward, TX_FEE, (long)b->timestamp);
    crypto_sha256_hex((const unsigned char *)buf, strlen(buf), out);
}

int hash_meets_difficulty(const char *hash, int difficulty) {
    if (difficulty < MIN_DIFFICULTY || difficulty > MAX_DIFFICULTY) return 0;
    for (int i = 0; i < difficulty; i++) {
        if (hash[i] != '0') return 0;
    }
    return 1;
}

void blockchain_prepare(const Blockchain *chain, Block *b, int difficulty) {
    b->index = (int)chain->length;
    if (chain->tail) {
        snprintf(b->previous_hash, HASH_HEX_LEN, "%s", chain->tail->hash);
    } else {
        blockchain_zero_hash(b->previous_hash);
    }
    b->difficulty = difficulty;
    b->nonce = 0;
    b->hash[0] = '\0';
}

int blockchain_pow(Block *b, unsigned long max_attempts, unsigned long *attempts) {
    *attempts = 0;
    while (*attempts < max_attempts) {
        block_compute_hash(b, b->hash);
        (*attempts)++;
        if (hash_meets_difficulty(b->hash, b->difficulty)) {
            return 1;
        }
        b->nonce++;
    }
    b->hash[0] = '\0';
    return 0;
}

int blockchain_sign_and_append(Blockchain *chain, Block *b, EVP_PKEY *priv_key) {
    /* The signature covers the final, proof-of-work hash, so it also covers
     * every field and the nonce that produced that hash. */
    if (!crypto_sign_hash(priv_key, b->hash, b->signature, sizeof(b->signature), &b->sig_len)) {
        return 0;
    }
    blocklist_append(chain, b);
    return 1;
}

int blockchain_create_genesis(Blockchain *chain, EVP_PKEY *priv_key, int difficulty) {
    if (chain->length != 0) {
        fprintf(stderr, "ERROR: genesis requested on a non-empty chain.\n");
        return 0;
    }
    Block *b = calloc(1, sizeof(Block));
    if (!b) {
        fprintf(stderr, "ERROR: out of memory creating genesis block.\n");
        return 0;
    }
    b->timestamp = time(NULL);
    snprintf(b->action, ACTION_LEN, "GENESIS");
    blockchain_prepare(chain, b, difficulty);

    unsigned long attempts = 0;
    /* Genesis is mined like any other block. The attempt cap is far above the
     * expected 16^4 = 65,536 tries at the maximum difficulty. */
    if (!blockchain_pow(b, 100000000UL, &attempts) ||
        !blockchain_sign_and_append(chain, b, priv_key)) {
        free(b);
        return 0;
    }
    printf("Genesis block mined at difficulty %d after %lu hash attempts.\n", difficulty, attempts);
    return 1;
}

/* ===================================================================== */
/* Loan-state lookups (chain first, then the pending pool)               */
/* ===================================================================== */

/* Latest record for a book across confirmed and pending records. Pending
 * records are newer than anything on the chain, so they are checked last. */
static const Block *latest_for_book(const Blockchain *chain, const PendingPool *pool,
                                    const char *book_id) {
    const Block *latest = NULL;
    for (const Block *b = chain->head; b; b = b->next) {
        if (b->index != 0 && strcmp(b->book_id, book_id) == 0) latest = b;
    }
    for (const Block *b = pool->head; b; b = b->next) {
        if (strcmp(b->book_id, book_id) == 0) latest = b;
    }
    return latest;
}

/* Latest BORROWED record for a book: the start of its current loan. */
static const Block *latest_borrow_for_book(const Blockchain *chain, const PendingPool *pool,
                                           const char *book_id) {
    const Block *latest = NULL;
    for (const Block *b = chain->head; b; b = b->next) {
        if (b->index != 0 && strcmp(b->book_id, book_id) == 0 &&
            strcmp(b->action, "BORROWED") == 0) latest = b;
    }
    for (const Block *b = pool->head; b; b = b->next) {
        if (strcmp(b->book_id, book_id) == 0 && strcmp(b->action, "BORROWED") == 0) latest = b;
    }
    return latest;
}

static int action_means_on_loan(const char *action) {
    /* An OVERDUE book is still out: overdue is a status, not a return. */
    return strcmp(action, "BORROWED") == 0 || strcmp(action, "OVERDUE") == 0;
}

int lending_book_on_loan(const Blockchain *chain, const PendingPool *pool, const char *book_id) {
    const Block *latest = latest_for_book(chain, pool, book_id);
    return latest != NULL && action_means_on_loan(latest->action);
}

/* ===================================================================== */
/* Lending events -> pending pool                                        */
/* ===================================================================== */

static Block *new_pending(const char *book_id, const char *title,
                          const char *member_id, const char *member_name,
                          const char *action, int reward) {
    Block *b = calloc(1, sizeof(Block));
    if (!b) return NULL;
    b->index = -1;               /* assigned when mined */
    b->timestamp = sim_now();
    snprintf(b->book_id, BOOK_ID_LEN, "%s", book_id);
    snprintf(b->book_title, BOOK_TITLE_LEN, "%s", title);
    snprintf(b->member_id, MEMBER_ID_LEN, "%s", member_id);
    snprintf(b->member_name, MEMBER_NAME_LEN, "%s", member_name);
    snprintf(b->action, ACTION_LEN, "%s", action);
    b->token_reward = reward;
    block_compute_tx_id(b, b->tx_id);
    return b;
}

static int tx_id_in_use(const Blockchain *chain, const PendingPool *pool, const char *tx_id) {
    for (const Block *b = chain->head; b; b = b->next) {
        if (b->tx_id[0] && strcmp(b->tx_id, tx_id) == 0) return 1;
    }
    for (const Block *b = pool->head; b; b = b->next) {
        if (b->tx_id[0] && strcmp(b->tx_id, tx_id) == 0) return 1;
    }
    return 0;
}

LendResult lending_borrow(const Blockchain *chain, PendingPool *pool, const Registry *reg,
                          const char *book_id, const char *member_id) {
    const Book *book = registry_find_book(reg, book_id);
    const Member *member = registry_find_member(reg, member_id);
    if (!book) return LEND_ERR_BOOK_NOT_FOUND;
    if (!member) return LEND_ERR_MEMBER_NOT_FOUND;
    if (lending_book_on_loan(chain, pool, book_id)) return LEND_ERR_ALREADY_BORROWED;

    Block *b = new_pending(book->book_id, book->title, member->member_id, member->full_name,
                           "BORROWED", REWARD_NONE);
    if (!b) return LEND_ERR_NO_MEMORY;
    blocklist_append(pool, b);
    return LEND_OK;
}

LendResult lending_return(const Blockchain *chain, PendingPool *pool, const Registry *reg,
                          const char *book_id, const Block **queued) {
    if (!registry_find_book(reg, book_id)) return LEND_ERR_BOOK_NOT_FOUND;
    if (!lending_book_on_loan(chain, pool, book_id)) return LEND_ERR_NOT_BORROWED;

    const Block *loan = latest_borrow_for_book(chain, pool, book_id);
    if (!loan) return LEND_ERR_NOT_BORROWED;

    time_t now = sim_now();
    int late = (now - loan->timestamp) > (time_t)LOAN_PERIOD_DAYS * SECONDS_PER_DAY;
    int reward = late ? REWARD_LATE : REWARD_ON_TIME;

    Block *b = new_pending(loan->book_id, loan->book_title, loan->member_id, loan->member_name,
                           "RETURNED", reward);
    if (!b) return LEND_ERR_NO_MEMORY;
    /* A tx_id must identify one transaction. The same member returning the
     * same book twice within one second (only possible from a script) would
     * otherwise produce an identical reward transaction, so the later event
     * is stamped one second later until its tx_id is unique. */
    while (tx_id_in_use(chain, pool, b->tx_id)) {
        b->timestamp++;
        block_compute_tx_id(b, b->tx_id);
    }
    blocklist_append(pool, b);
    if (queued) *queued = b;
    return LEND_OK;
}

int lending_check_overdue(const Blockchain *chain, PendingPool *pool, const Registry *reg) {
    int queued = 0;
    time_t now = sim_now();
    for (size_t i = 0; i < reg->book_count; i++) {
        const char *book_id = reg->books[i].book_id;
        const Block *latest = latest_for_book(chain, pool, book_id);
        /* Only a book whose latest record is BORROWED can newly become
         * overdue; one already flagged OVERDUE is not flagged twice. */
        if (!latest || strcmp(latest->action, "BORROWED") != 0) continue;
        if ((now - latest->timestamp) <= (time_t)LOAN_PERIOD_DAYS * SECONDS_PER_DAY) continue;

        Block *b = new_pending(latest->book_id, latest->book_title, latest->member_id,
                               latest->member_name, "OVERDUE", REWARD_NONE);
        if (!b) break;
        blocklist_append(pool, b);
        queued++;
    }
    return queued;
}

/* ===================================================================== */
/* Validation                                                            */
/* ===================================================================== */

static ChainValidation fail(int index, const char *reason) {
    ChainValidation v = { .valid = 0, .bad_index = index, .reason = reason };
    return v;
}

ChainValidation blockchain_validate(const Blockchain *chain, EVP_PKEY *pub_key) {
    if (!chain->head) return fail(-1, "chain is empty (no genesis block)");

    char expected_prev[HASH_HEX_LEN];
    blockchain_zero_hash(expected_prev);
    int expected_index = 0;

    for (const Block *b = chain->head; b; b = b->next) {
        if (b->index != expected_index) {
            return fail(b->index, "block index out of sequence");
        }
        if (strcmp(b->previous_hash, expected_prev) != 0) {
            return fail(b->index, "previous_hash does not match the prior block's hash");
        }

        char recomputed[HASH_HEX_LEN];
        block_compute_hash(b, recomputed);
        if (strcmp(recomputed, b->hash) != 0) {
            return fail(b->index, "stored hash does not match recomputed hash (block data was modified)");
        }
        if (!hash_meets_difficulty(b->hash, b->difficulty)) {
            return fail(b->index, "hash does not meet the proof-of-work difficulty target");
        }

        char expected_tx[HASH_HEX_LEN];
        block_compute_tx_id(b, expected_tx);
        if (strcmp(expected_tx, b->tx_id) != 0) {
            return fail(b->index, "tx_id does not match the block's reward transaction");
        }

        if (!crypto_verify_hash(pub_key, b->hash, b->signature, b->sig_len)) {
            return fail(b->index, "ECDSA signature is invalid for this block's hash");
        }

        snprintf(expected_prev, HASH_HEX_LEN, "%s", b->hash);
        expected_index++;
    }

    ChainValidation ok = { .valid = 1, .bad_index = -1, .reason = NULL };
    return ok;
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
        case LEND_ERR_NO_MEMORY:
            return "ERROR: out of memory";
    }
    return "ERROR: unknown";
}
