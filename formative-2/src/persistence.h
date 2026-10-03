#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "blockchain.h"

typedef enum {
    PERSIST_LOADED,     /* file existed and parsed                         */
    PERSIST_NOT_FOUND,  /* no file yet                                      */
    PERSIST_CORRUPT     /* file existed but could not be parsed safely      */
} PersistResult;

/* Chain file: one confirmed block per line, '|'-separated:
 *   index|timestamp|book_id|title|member_id|member_name|action|previous_hash|
 *   difficulty|nonce|token_reward|tx_id|signature_hex|hash
 * Loading only checks the file's shape. Hashes, proof of work and signatures
 * are verified separately by blockchain_validate() straight after loading. */
PersistResult persistence_load_chain(Blockchain *chain, const char *path);
int persistence_save_chain(const Blockchain *chain, const char *path);

/* Pending file: one unconfirmed lending record per line:
 *   timestamp|book_id|title|member_id|member_name|action|token_reward|tx_id
 * Saved so queued borrows/returns survive a restart before they are mined.
 * A missing file simply means an empty pool. */
PersistResult persistence_load_pending(PendingPool *pool, const char *path);
int persistence_save_pending(const PendingPool *pool, const char *path);

#endif
