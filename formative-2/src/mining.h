#ifndef MINING_H
#define MINING_H

#include "blockchain.h"
#include "ledger.h"

/* Everything a mining run touches. Mining is the only way a pending lending
 * record reaches the chain: it takes records from the front of the pending
 * pool, runs proof of work on each, signs the result, appends it to the
 * chain, applies any reward transaction to the member ledger, and saves
 * both the chain and the (now smaller) pending pool. */
typedef struct {
    Blockchain *chain;
    PendingPool *pool;
    Ledger *ledger;
    const Registry *reg;
    EVP_PKEY *priv_key;
    EVP_PKEY *pub_key;
    int difficulty;
    const char *chain_path;
    const char *pending_path;
} MiningContext;

/* One miner confirms every pending record and receives the full block
 * reward plus all transaction fees. Prints hash attempts per block. */
void mine_solo(MiningContext *ctx);

/* n_miners miners with random hash rates share the work; rewards (minus a
 * 2% pool fee) are split in proportion to each miner's hash attempts. */
void mine_pool(MiningContext *ctx, int n_miners);

/* A rented rig mines for `rounds` rounds with a fixed hash budget per
 * round. Each round costs a rental fee; rewards earned are reduced by a
 * maintenance fee. Prints a per-round and overall profit summary and warns
 * whenever cumulative fees exceed cumulative rewards. */
void mine_cloud(MiningContext *ctx, int rounds);

#endif
