#ifndef LEDGER_H
#define LEDGER_H

#include "blockchain.h"
#include "utxo.h"
#include "account.h"

/* The member token ledger under the transaction model chosen at startup.
 * Only the chosen model is used for the whole session; the other stays
 * empty. Both models use the registry's member IDs as account identifiers.
 *
 * Balances are never stored directly. They are rebuilt at startup from two
 * sources, applied in the order they originally happened:
 *   1. reward transactions in confirmed blocks on the chain, and
 *   2. the signed transfer log (one file per model), where each transfer is
 *      tagged with the chain length at the moment it was made.
 * Replaying in that order reproduces the exact same state every time, so a
 * UTXO that was spent stays spent and a nonce that was used stays used,
 * even across restarts. */

typedef enum { MODEL_UTXO, MODEL_ACCOUNT } LedgerModel;

typedef struct {
    LedgerModel model;
    UtxoSet utxo;
    AccountLedger accounts;
    const Registry *reg;
    long unclaimed_fees;     /* transfer fees waiting for the next miner      */
    const char *log_path;    /* signed transfer log for this model            */
    EVP_PKEY *priv_key;      /* signs each transfer log entry                 */
    EVP_PKEY *pub_key;       /* verifies the log on replay                    */
} Ledger;

void ledger_init(Ledger *led, LedgerModel model, const Registry *reg,
                 const char *log_path, EVP_PKEY *priv_key, EVP_PKEY *pub_key);
void ledger_free(Ledger *led);

const char *ledger_model_name(LedgerModel model);

/* Applies a confirmed block's reward transaction (if it has one) to the
 * chosen model. Returns the fee collected from it, for the miner. */
long ledger_apply_confirmed_block(Ledger *led, const Block *b);

/* Rebuilds balances from the chain and the transfer log. Returns 1 on
 * success, 0 if the log is unreadable, has an invalid signature, refers to
 * blocks that are not on the chain, or contains a transfer that no longer
 * validates - all signs the log was edited outside the program. */
int ledger_replay(Ledger *led, const Blockchain *chain);

/* Member-to-member transfers. Each validates under its model's rules, is
 * applied, and is appended to the signed log tagged with chain_length.
 * Return 1 on success, 0 on rejection (reason printed). */
int ledger_transfer_utxo(Ledger *led, size_t chain_length, const char *from, const char *to,
                         long amount, const int *input_ids, size_t n_inputs);
int ledger_transfer_account(Ledger *led, size_t chain_length, const char *from, const char *to,
                            long amount, long nonce);

/* Prints balances (and, for UTXO, the full unspent-output set). */
void ledger_print(const Ledger *led);

/* Hands the transfer fees collected since the last mining run to the miner. */
long ledger_take_unclaimed_fees(Ledger *led);

#endif
