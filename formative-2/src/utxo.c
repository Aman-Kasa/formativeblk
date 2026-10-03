#include "utxo.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void utxo_init(UtxoSet *set) {
    set->head = set->tail = NULL;
    set->next_id = 1;
    set->tx_counter = 0;
}

void utxo_free(UtxoSet *set) {
    Utxo *cur = set->head;
    while (cur) {
        Utxo *next = cur->next;
        free(cur);
        cur = next;
    }
    utxo_init(set);
}

static Utxo *find_by_id(const UtxoSet *set, int id) {
    for (Utxo *u = set->head; u; u = u->next) {
        if (u->id == id) return u;
    }
    return NULL;
}

static int add_output(UtxoSet *set, const char *txid, int vout, const char *owner, long amount) {
    Utxo *u = calloc(1, sizeof(Utxo));
    if (!u) return 0;
    u->id = set->next_id++;
    snprintf(u->txid, HASH_HEX_LEN, "%s", txid);
    u->vout = vout;
    snprintf(u->owner, MEMBER_ID_LEN, "%s", owner);
    u->amount = amount;
    u->spent = 0;
    if (set->tail) set->tail->next = u; else set->head = u;
    set->tail = u;
    return 1;
}

long utxo_credit_reward(UtxoSet *set, const char *member_id, long gross, const char *tx_id) {
    /* The reward transaction has no inputs: like a coinbase transaction, it
     * creates new coins. The fee is taken before the member is credited. */
    if (gross <= TX_FEE) return 0;
    if (!add_output(set, tx_id, 0, member_id, gross - TX_FEE)) return 0;
    return TX_FEE;
}

int utxo_transfer(UtxoSet *set, const Registry *reg, const char *from, const char *to,
                  long amount, const int *input_ids, size_t n_inputs,
                  long *fee_out, char *txid_out, char *err, size_t err_len, int verbose) {
    if (!registry_find_member(reg, from) || !registry_find_member(reg, to)) {
        snprintf(err, err_len, "ERROR: sender or recipient is not a registered member");
        return 0;
    }
    if (strcmp(from, to) == 0) {
        snprintf(err, err_len, "ERROR: sender and recipient must be different members");
        return 0;
    }
    if (amount <= 0) {
        snprintf(err, err_len, "ERROR: amount must be a positive whole number of coins");
        return 0;
    }

    const long needed = amount + TX_FEE;
    Utxo *inputs[UTXO_MAX_INPUTS];
    size_t n = 0;
    long total_in = 0;

    if (n_inputs == 0) {
        /* Automatic coin selection: oldest unspent outputs first. */
        for (Utxo *u = set->head; u && total_in < needed; u = u->next) {
            if (u->spent || strcmp(u->owner, from) != 0) continue;
            if (n == UTXO_MAX_INPUTS) break;
            inputs[n++] = u;
            total_in += u->amount;
        }
    } else {
        if (n_inputs > UTXO_MAX_INPUTS) {
            snprintf(err, err_len, "ERROR: at most %d inputs per transaction", UTXO_MAX_INPUTS);
            return 0;
        }
        /* Explicit inputs: every check happens before anything is changed,
         * so a rejected transaction leaves the UTXO set untouched. */
        for (size_t i = 0; i < n_inputs; i++) {
            Utxo *u = find_by_id(set, input_ids[i]);
            if (!u) {
                snprintf(err, err_len, "ERROR: input #%d does not exist", input_ids[i]);
                return 0;
            }
            if (u->spent) {
                snprintf(err, err_len, "ERROR: double spend rejected - input #%d is already spent",
                         input_ids[i]);
                return 0;
            }
            if (strcmp(u->owner, from) != 0) {
                snprintf(err, err_len, "ERROR: input #%d belongs to %s, not %s",
                         input_ids[i], u->owner, from);
                return 0;
            }
            for (size_t j = 0; j < n; j++) {
                if (inputs[j] == u) {
                    snprintf(err, err_len,
                             "ERROR: double spend rejected - input #%d is listed twice", input_ids[i]);
                    return 0;
                }
            }
            inputs[n++] = u;
            total_in += u->amount;
        }
    }

    if (total_in < needed) {
        snprintf(err, err_len,
                 "ERROR: insufficient funds - inputs total %ld, but amount %ld + fee %d = %ld",
                 total_in, amount, TX_FEE, needed);
        return 0;
    }

    /* Transaction id: SHA-256 over the inputs spent and outputs created.
     * The counter keeps two otherwise identical transfers distinct. */
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "TRANSFER|%lu|%s|%s|%ld|%d|in:",
                       ++set->tx_counter, from, to, amount, TX_FEE);
    for (size_t i = 0; i < n && len > 0 && (size_t)len < sizeof(buf); i++) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s:%d,", inputs[i]->txid, inputs[i]->vout);
    }
    char txid[HASH_HEX_LEN];
    crypto_sha256_hex((const unsigned char *)buf, strlen(buf), txid);

    long change = total_in - needed;
    if (!add_output(set, txid, 0, to, amount) ||
        (change > 0 && !add_output(set, txid, 1, from, change))) {
        snprintf(err, err_len, "ERROR: out of memory");
        return 0;
    }
    for (size_t i = 0; i < n; i++) inputs[i]->spent = 1;

    *fee_out = TX_FEE;
    if (txid_out) snprintf(txid_out, HASH_HEX_LEN, "%s", txid);
    if (!verbose) return 1;
    printf("Transfer %.12s...: spent %zu input(s) worth %ld -> %ld to %s",
           txid, n, total_in, amount, to);
    if (change > 0) printf(", %ld change back to %s", change, from);
    printf(", fee %d.\n", TX_FEE);
    return 1;
}

long utxo_balance(const UtxoSet *set, const char *member_id) {
    long total = 0;
    for (const Utxo *u = set->head; u; u = u->next) {
        if (!u->spent && strcmp(u->owner, member_id) == 0) total += u->amount;
    }
    return total;
}

void utxo_print_set(const UtxoSet *set, const Registry *reg) {
    printf("UTXO set (unspent outputs):\n");
    printf("  %-5s %-10s %-8s %-22s\n", "ID", "Owner", "Amount", "Created by (txid:vout)");
    int any = 0;
    for (const Utxo *u = set->head; u; u = u->next) {
        if (u->spent) continue;
        printf("  #%-4d %-10s %-8ld %.16s...:%d\n", u->id, u->owner, u->amount, u->txid, u->vout);
        any = 1;
    }
    if (!any) printf("  (empty)\n");
    printf("  Balances: ");
    for (size_t i = 0; i < reg->member_count; i++) {
        printf("%s=%ld%s", reg->members[i].member_id,
               utxo_balance(set, reg->members[i].member_id),
               i + 1 < reg->member_count ? ", " : "\n");
    }
}
