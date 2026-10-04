#include "mining.h"
#include "config.h"
#include "display.h"
#include "persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Upper bound for a single uninterrupted proof-of-work search. At the
 * maximum difficulty (4) the expected number of attempts is 16^4 = 65,536,
 * so this cap is never reached in practice; it only guards against a
 * runaway loop. */
#define SOLO_ATTEMPT_CAP 100000000UL

static double elapsed_ms(clock_t start) {
    return 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC;
}

/* Moves the mined block at the front of the pending pool onto the chain:
 * sign, append, apply its reward transaction, show it, and save state.
 * Returns the transaction fee it carried, or -1 if signing failed (the
 * block then stays at the front of the pending pool). */
static long confirm_front_block(MiningContext *ctx) {
    Block *b = blocklist_pop_front(ctx->pool);
    if (!blockchain_sign_and_append(ctx->chain, b, ctx->priv_key)) {
        /* Put it back so no lending record is ever lost. */
        b->next = ctx->pool->head;
        ctx->pool->head = b;
        if (!ctx->pool->tail) ctx->pool->tail = b;
        ctx->pool->length++;
        printf("ERROR: signing failed - block left in the pending pool.\n");
        return -1;
    }

    long fee = ledger_apply_confirmed_block(ctx->ledger, b);
    printf("Confirmed:\n");
    print_confirmed_block(b, ctx->pub_key);
    if (b->token_reward > 0) {
        printf("        -> reward transaction confirmed: %s credited %d - fee %ld = %ld coins\n",
               b->member_id, b->token_reward, fee, (long)b->token_reward - fee);
    }
    if (ctx->ledger->model == MODEL_UTXO) {
        utxo_print_set(&ctx->ledger->utxo, ctx->reg);
    }

    persistence_save_chain(ctx->chain, ctx->chain_path);
    persistence_save_pending(ctx->pool, ctx->pending_path);
    return fee;
}

static int nothing_to_mine(const MiningContext *ctx) {
    if (ctx->pool->length == 0) {
        printf("Pending pool is empty - nothing to mine. Borrow or return a book first.\n");
        return 1;
    }
    return 0;
}

/* ===================================================================== */
/* Solo mining                                                           */
/* ===================================================================== */

void mine_solo(MiningContext *ctx) {
    if (nothing_to_mine(ctx)) return;

    printf("=== Solo mining: %zu pending record(s), difficulty %d (hash must start with %d zero%s) ===\n",
           ctx->pool->length, ctx->difficulty, ctx->difficulty, ctx->difficulty == 1 ? "" : "s");

    int blocks = 0;
    long fees = ledger_take_unclaimed_fees(ctx->ledger);
    unsigned long total_attempts = 0;
    clock_t start = clock();

    while (ctx->pool->head) {
        Block *b = ctx->pool->head;
        blockchain_prepare(ctx->chain, b, ctx->difficulty);

        unsigned long attempts = 0;
        clock_t block_start = clock();
        if (!blockchain_pow(b, SOLO_ATTEMPT_CAP, &attempts)) {
            printf("ERROR: no valid nonce found within %lu attempts - stopping.\n", SOLO_ATTEMPT_CAP);
            break;
        }
        total_attempts += attempts;
        printf("\nBlock %d: valid hash found after %lu attempts (nonce %lu, %.1f ms)\n",
               b->index, attempts, b->nonce, elapsed_ms(block_start));

        long fee = confirm_front_block(ctx);
        if (fee < 0) break;
        fees += fee;
        blocks++;
    }

    double reward = blocks * BLOCK_REWARD + (double)fees;
    printf("\n--- Solo mining summary ---\n");
    printf("  Blocks confirmed     : %d\n", blocks);
    printf("  Total hash attempts  : %lu (avg %.0f per block, expected ~%.0f at difficulty %d)\n",
           total_attempts, blocks ? (double)total_attempts / blocks : 0.0,
           (double)(1UL << (4 * ctx->difficulty)), ctx->difficulty);
    printf("  Time                 : %.1f ms\n", elapsed_ms(start));
    printf("  Miner reward         : %d x %.0f block reward + %ld fees = %.2f coins (all to the solo miner)\n",
           blocks, BLOCK_REWARD, fees, reward);
}

/* ===================================================================== */
/* Pool mining                                                           */
/* ===================================================================== */

typedef struct {
    int hash_rate;               /* attempts per round */
    unsigned long attempts;      /* total attempts across the whole run */
    int blocks_found;
} PoolMiner;

void mine_pool(MiningContext *ctx, int n_miners) {
    if (nothing_to_mine(ctx)) return;
    if (n_miners < POOL_MIN_MINERS || n_miners > POOL_MAX_MINERS) {
        printf("ERROR: pool size must be between %d and %d miners.\n", POOL_MIN_MINERS, POOL_MAX_MINERS);
        return;
    }

    PoolMiner miners[POOL_MAX_MINERS];
    printf("=== Pool mining: %d miners, %zu pending record(s), difficulty %d ===\n",
           n_miners, ctx->pool->length, ctx->difficulty);
    for (int i = 0; i < n_miners; i++) {
        miners[i].hash_rate = POOL_MIN_HASHRATE + rand() % (POOL_MAX_HASHRATE - POOL_MIN_HASHRATE + 1);
        miners[i].attempts = 0;
        miners[i].blocks_found = 0;
        printf("  Miner M%d hash rate: %d attempts/round\n", i + 1, miners[i].hash_rate);
    }

    int blocks = 0;
    long fees = ledger_take_unclaimed_fees(ctx->ledger);

    while (ctx->pool->head) {
        Block *b = ctx->pool->head;
        blockchain_prepare(ctx->chain, b, ctx->difficulty);

        /* All miners hash at the same time. In each round, miner i searches
         * its own slice of the nonce space (hash_rate nonces, starting where
         * miner i-1's slice ends), so no two miners repeat each other's
         * work. A miner that finds a valid hash after k of its r attempts
         * found it at time k/r through the round; the earliest finder wins
         * the block. Every miner is credited only with the attempts it had
         * made by that moment, which is what the reward share is based on. */
        int winner = -1, rounds = 0;
        unsigned long round_start = 0;
        double win_time = 0.0;
        while (winner < 0) {
            rounds++;
            unsigned long slice_start = round_start;
            unsigned long found_at[POOL_MAX_MINERS];
            Block trial[POOL_MAX_MINERS];
            for (int i = 0; i < n_miners; i++) {
                trial[i] = *b;
                trial[i].nonce = slice_start;
                unsigned long a = 0;
                found_at[i] = blockchain_pow(&trial[i], (unsigned long)miners[i].hash_rate, &a) ? a : 0;
                slice_start += (unsigned long)miners[i].hash_rate;
                if (found_at[i]) {
                    double t = (double)found_at[i] / miners[i].hash_rate;
                    if (winner < 0 || t < win_time) { winner = i; win_time = t; }
                }
            }
            for (int i = 0; i < n_miners; i++) {
                unsigned long rate = (unsigned long)miners[i].hash_rate;
                unsigned long done = rate;                       /* no winner: full round */
                if (winner == i) {
                    done = found_at[i];
                } else if (winner >= 0) {
                    done = (unsigned long)(win_time * rate + 0.999999); /* work done by then */
                    if (done > rate) done = rate;
                }
                miners[i].attempts += done;
            }
            if (winner >= 0) {
                b->nonce = trial[winner].nonce;
                memcpy(b->hash, trial[winner].hash, HASH_HEX_LEN);
            }
            round_start = slice_start;
        }
        miners[winner].blocks_found++;
        printf("\nBlock %d: found by M%d in round %d, %.0f%% of the way through it (nonce %lu)\n",
               b->index, winner + 1, rounds, win_time * 100.0, b->nonce);

        long fee = confirm_front_block(ctx);
        if (fee < 0) break;
        fees += fee;
        blocks++;
    }

    unsigned long total_attempts = 0;
    for (int i = 0; i < n_miners; i++) total_attempts += miners[i].attempts;

    double gross = blocks * BLOCK_REWARD + (double)fees;
    double pool_fee = gross * POOL_FEE_PERCENT / 100.0;
    double distributable = gross - pool_fee;

    printf("\n--- Pool reward distribution ---\n");
    printf("  Total reward: %d x %.0f block reward + %ld fees = %.2f coins\n",
           blocks, BLOCK_REWARD, fees, gross);
    printf("  Pool fee (%.0f%%): %.2f coins  ->  distributed to miners: %.2f coins\n",
           POOL_FEE_PERCENT, pool_fee, distributable);
    printf("  Share = miner_attempts / total_attempts; reward = share x distributable reward\n\n");
    printf("  +----------+-----------+----------+--------------+---------+------------+\n");
    printf("  | Miner ID | Hash rate | Attempts | Blocks found | Share %% | Reward     |\n");
    printf("  +----------+-----------+----------+--------------+---------+------------+\n");
    for (int i = 0; i < n_miners; i++) {
        double share = total_attempts ? (double)miners[i].attempts / (double)total_attempts : 0.0;
        printf("  | M%-7d | %9d | %8lu | %12d | %6.2f%% | %10.2f |\n",
               i + 1, miners[i].hash_rate, miners[i].attempts, miners[i].blocks_found,
               share * 100.0, share * distributable);
    }
    printf("  +----------+-----------+----------+--------------+---------+------------+\n");
    printf("  | TOTAL    |           | %8lu | %12d | 100.00%% | %10.2f |\n",
           total_attempts, blocks, distributable);
    printf("  +----------+-----------+----------+--------------+---------+------------+\n");
}

/* ===================================================================== */
/* Cloud mining                                                          */
/* ===================================================================== */

void mine_cloud(MiningContext *ctx, int rounds) {
    if (rounds < CLOUD_MIN_ROUNDS || rounds > CLOUD_MAX_ROUNDS) {
        printf("ERROR: rental duration must be between %d and %d rounds.\n",
               CLOUD_MIN_ROUNDS, CLOUD_MAX_ROUNDS);
        return;
    }

    printf("=== Cloud mining: renting %d round(s), difficulty %d ===\n", rounds, ctx->difficulty);
    printf("  Rented hash power : %d attempts per round\n", CLOUD_HASHRATE);
    printf("  Rental fee        : %.2f coins per round\n", CLOUD_RENTAL_FEE);
    printf("  Maintenance fee   : %.0f%% of every reward earned\n", CLOUD_MAINTENANCE_PERCENT);
    printf("  Pending records   : %zu\n", ctx->pool->length);
    if (ctx->pool->length == 0) {
        printf("  Note: the pending pool is empty, so the rig has nothing to confirm.\n");
    }

    double cum_reward = 0.0, cum_fees = 0.0;
    long bonus_fees = ledger_take_unclaimed_fees(ctx->ledger);
    const Block *in_progress = NULL;   /* block whose PoW carries across rounds */
    int total_blocks = 0;
    int ever_unprofitable = 0;

    for (int r = 1; r <= rounds; r++) {
        unsigned long budget = CLOUD_HASHRATE, used = 0;
        int blocks = 0;
        double reward = 0.0;

        printf("\n-- Round %d --\n", r);
        while (budget > 0 && ctx->pool->head) {
            Block *b = ctx->pool->head;
            if (b != in_progress) {
                blockchain_prepare(ctx->chain, b, ctx->difficulty);
                in_progress = b;
            }
            unsigned long a = 0;
            int found = blockchain_pow(b, budget, &a);
            budget -= a;
            used += a;
            if (!found) break;   /* out of hash power this round; resume next round */

            printf("Block %d: valid hash found (nonce %lu)\n", b->index, b->nonce);
            long fee = confirm_front_block(ctx);
            in_progress = NULL;
            if (fee < 0) break;
            reward += BLOCK_REWARD + (double)fee + (double)bonus_fees;
            bonus_fees = 0;
            blocks++;
        }

        double maintenance = reward * CLOUD_MAINTENANCE_PERCENT / 100.0;
        double round_fees = CLOUD_RENTAL_FEE + maintenance;
        cum_reward += reward;
        cum_fees += round_fees;
        total_blocks += blocks;

        printf("Round %d: %lu attempts used, %d block(s) confirmed, reward %.2f, "
               "rental %.2f, maintenance %.2f\n",
               r, used, blocks, reward, CLOUD_RENTAL_FEE, maintenance);
        printf("         cumulative rewards %.2f, cumulative fees %.2f, running net %.2f\n",
               cum_reward, cum_fees, cum_reward - cum_fees);
        if (cum_fees > cum_reward) {
            ever_unprofitable = 1;
            printf("         WARNING: rental is unprofitable - cumulative fees (%.2f) exceed "
                   "cumulative rewards (%.2f)\n", cum_fees, cum_reward);
        }
    }

    if (in_progress) {
        printf("\nNote: the block being mined when the rental ended stays in the pending pool.\n");
    }
    /* Transfer fees not paid out (no block was confirmed) wait for the next miner. */
    ctx->ledger->unclaimed_fees += bonus_fees;

    printf("\n--- Cloud mining summary (%d round%s) ---\n", rounds, rounds == 1 ? "" : "s");
    printf("  Blocks confirmed : %d\n", total_blocks);
    printf("  Gross earnings   : %.2f coins\n", cum_reward);
    printf("  Total fees paid  : %.2f coins (rental %.2f + maintenance %.2f)\n",
           cum_fees, CLOUD_RENTAL_FEE * rounds, cum_fees - CLOUD_RENTAL_FEE * rounds);
    printf("  Net profit       : %.2f coins\n", cum_reward - cum_fees);
    if (cum_reward - cum_fees < 0) {
        printf("  WARNING: this rental was UNPROFITABLE overall.\n");
    } else if (ever_unprofitable) {
        printf("  Note: the rental was unprofitable during at least one round but ended in profit.\n");
    }
}
