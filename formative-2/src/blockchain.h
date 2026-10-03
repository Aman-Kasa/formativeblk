#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include "block.h"

/* The confirmed chain and the pending pool share one list type but are kept
 * as distinct names so every function signature says which one it expects. */
typedef BlockList Blockchain;
typedef BlockList PendingPool;

typedef enum {
    LEND_OK = 0,
    LEND_ERR_BOOK_NOT_FOUND,
    LEND_ERR_MEMBER_NOT_FOUND,
    LEND_ERR_ALREADY_BORROWED,
    LEND_ERR_NOT_BORROWED,
    LEND_ERR_NO_MEMORY,
} LendResult;

typedef struct {
    int valid;               /* 1 = chain intact, 0 = compromised            */
    int bad_index;           /* index of the first problem block, -1 if valid */
    const char *reason;      /* static, human-readable reason                 */
} ChainValidation;

/* ---- Lending events: these only ever add to the PENDING POOL ------------- */

/* Validates both IDs against the registry and the book's current loan state
 * (chain + pending pool), then queues an unconfirmed BORROWED record. */
LendResult lending_borrow(const Blockchain *chain, PendingPool *pool, const Registry *reg,
                          const char *book_id, const char *member_id);

/* Finds the book's open loan, decides on-time vs late from the simulated
 * clock, and queues an unconfirmed RETURNED record carrying the reward and
 * the reward transaction's ID. The queued record is returned via *queued. */
LendResult lending_return(const Blockchain *chain, PendingPool *pool, const Registry *reg,
                          const char *book_id, const Block **queued);

/* Queues an OVERDUE record (reward 0, no transaction) for every book that is
 * still on loan past the loan period and not already flagged overdue.
 * Returns the number of records queued. */
int lending_check_overdue(const Blockchain *chain, PendingPool *pool, const Registry *reg);

/* True if the book's latest record (chain, then pending pool) leaves it on loan. */
int lending_book_on_loan(const Blockchain *chain, const PendingPool *pool, const char *book_id);

/* ---- Hashing, proof of work and sealing ---------------------------------- */

/* SHA-256 over every hashed field (all fields except signature and hash). */
void block_compute_hash(const Block *b, char out[HASH_HEX_LEN]);

/* SHA-256 identifier of a block's reward transaction, derived from the
 * member, book, reward, fee and event time. Empty string if no reward. */
void block_compute_tx_id(const Block *b, char out[HASH_HEX_LEN]);

/* 1 if `hash` starts with `difficulty` '0' characters. */
int hash_meets_difficulty(const char *hash, int difficulty);

/* Fills in the chain-position fields of a pending block (index,
 * previous_hash, difficulty) and resets its nonce, ready for mining. */
void blockchain_prepare(const Blockchain *chain, Block *b, int difficulty);

/* Tries nonces starting from b->nonce, at most `max_attempts` times. On
 * success b->hash holds a hash meeting the difficulty and 1 is returned.
 * *attempts receives the number of hashes computed in this call. On failure
 * b->nonce is left at the next untried value so mining can resume later. */
int blockchain_pow(Block *b, unsigned long max_attempts, unsigned long *attempts);

/* Signs a mined block's hash with ECDSA and appends it to the chain. */
int blockchain_sign_and_append(Blockchain *chain, Block *b, EVP_PKEY *priv_key);

/* Creates and mines the genesis block (index 0, previous_hash = 64 zeros). */
int blockchain_create_genesis(Blockchain *chain, EVP_PKEY *priv_key, int difficulty);

/* Walks the full chain verifying: index order, previous_hash linkage,
 * recomputed hash, proof-of-work target, reward transaction ID, and ECDSA
 * signature. Stops at, and reports, the first problem found. */
ChainValidation blockchain_validate(const Blockchain *chain, EVP_PKEY *pub_key);

void blockchain_zero_hash(char out[HASH_HEX_LEN]);
const char *lend_result_message(LendResult r);

#endif
