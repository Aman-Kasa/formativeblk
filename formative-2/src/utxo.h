#ifndef UTXO_H
#define UTXO_H

#include <stddef.h>

#include "registry.h"
#include "crypto.h"

/* UTXO model. A member's coins are not a number stored anywhere; they are
 * the set of unspent transaction outputs that name the member as owner.
 * Every transaction consumes whole outputs (inputs) and creates new ones
 * (outputs). Each output gets a short sequential id (#1, #2, ...) so it can
 * be named on the command line; its full identity is txid:vout. */

#define UTXO_MAX_INPUTS 16

typedef struct Utxo {
    int id;                          /* short id shown in the CLI           */
    char txid[HASH_HEX_LEN];         /* transaction that created it         */
    int vout;                        /* output position within that tx      */
    char owner[MEMBER_ID_LEN];
    long amount;
    int spent;                       /* 1 once used as an input             */
    struct Utxo *next;
} Utxo;

typedef struct {
    Utxo *head;
    Utxo *tail;
    int next_id;
    unsigned long tx_counter;        /* makes every transfer's txid unique  */
} UtxoSet;

void utxo_init(UtxoSet *set);
void utxo_free(UtxoSet *set);

/* Confirmed reward: a transaction with no inputs paying (gross - TX_FEE) to
 * the member, identified by the block's tx_id. Returns the fee collected
 * (for the miner), or 0 if gross <= TX_FEE. */
long utxo_credit_reward(UtxoSet *set, const char *member_id, long gross, const char *tx_id);

/* Member-to-member transfer. If n_inputs is 0, unspent outputs owned by
 * `from` are selected automatically (oldest first) until they cover
 * amount + TX_FEE. Otherwise exactly the listed output ids are spent.
 * Rejected (returns 0, reason in err) when: amount <= 0, sender == recipient,
 * an input is unknown, already spent (double spend), not owned by the
 * sender, or listed twice, or the inputs do not cover amount + fee. On
 * success the inputs are marked spent, an output pays `to`, any excess comes
 * back to `from` as a change output, and *fee_out receives the fee. */
int utxo_transfer(UtxoSet *set, const Registry *reg, const char *from, const char *to,
                  long amount, const int *input_ids, size_t n_inputs,
                  long *fee_out, char *txid_out, char *err, size_t err_len, int verbose);

long utxo_balance(const UtxoSet *set, const char *member_id);

/* Prints every unspent output with its owner, then a per-member total. */
void utxo_print_set(const UtxoSet *set, const Registry *reg);

#endif
