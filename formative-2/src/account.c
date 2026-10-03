#include "account.h"
#include "config.h"
#include "simclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void account_init(AccountLedger *led, const Registry *reg) {
    memset(led, 0, sizeof(*led));
    for (size_t i = 0; i < reg->member_count && i < MAX_MEMBERS; i++) {
        snprintf(led->accounts[i].member_id, MEMBER_ID_LEN, "%s", reg->members[i].member_id);
        led->count++;
    }
}

void account_free(AccountLedger *led) {
    for (size_t i = 0; i < led->count; i++) {
        TxRecord *cur = led->accounts[i].history_head;
        while (cur) {
            TxRecord *next = cur->next;
            free(cur);
            cur = next;
        }
        led->accounts[i].history_head = led->accounts[i].history_tail = NULL;
    }
}

Account *account_find(AccountLedger *led, const char *member_id) {
    for (size_t i = 0; i < led->count; i++) {
        if (strcmp(led->accounts[i].member_id, member_id) == 0) return &led->accounts[i];
    }
    return NULL;
}

/* Appends a copy of the transaction to one account's history list. */
static int log_tx(Account *acc, const char *kind, const char *sender, const char *recipient,
                  long amount, long fee, long nonce) {
    TxRecord *r = calloc(1, sizeof(TxRecord));
    if (!r) return 0;
    snprintf(r->kind, sizeof(r->kind), "%s", kind);
    snprintf(r->sender, MEMBER_ID_LEN, "%s", sender);
    snprintf(r->recipient, MEMBER_ID_LEN, "%s", recipient);
    r->amount = amount;
    r->fee = fee;
    r->nonce = nonce;
    r->timestamp = sim_now();
    if (acc->history_tail) acc->history_tail->next = r; else acc->history_head = r;
    acc->history_tail = r;
    return 1;
}

long account_credit_reward(AccountLedger *led, const char *member_id, long gross) {
    Account *acc = account_find(led, member_id);
    if (!acc || gross <= TX_FEE) return 0;
    acc->balance += gross - TX_FEE;
    log_tx(acc, "REWARD", "SYSTEM", member_id, gross - TX_FEE, TX_FEE, -1);
    return TX_FEE;
}

int account_transfer(AccountLedger *led, const char *from, const char *to,
                     long amount, long nonce, long *fee_out, char *err, size_t err_len, int verbose) {
    Account *sender = account_find(led, from);
    Account *recipient = account_find(led, to);
    if (!sender || !recipient) {
        snprintf(err, err_len, "ERROR: sender or recipient is not a registered member");
        return 0;
    }
    if (sender == recipient) {
        snprintf(err, err_len, "ERROR: sender and recipient must be different members");
        return 0;
    }
    if (amount <= 0) {
        snprintf(err, err_len, "ERROR: amount must be a positive whole number of coins");
        return 0;
    }
    if (nonce != sender->nonce) {
        snprintf(err, err_len, "ERROR: invalid nonce %ld for %s - expected %ld (%s)",
                 nonce, from, sender->nonce,
                 nonce < sender->nonce ? "nonce already used: replay rejected" : "nonce skips ahead");
        return 0;
    }
    if (sender->balance < amount + TX_FEE) {
        snprintf(err, err_len,
                 "ERROR: insufficient balance - %s has %ld, needs amount %ld + fee %d = %ld",
                 from, sender->balance, amount, TX_FEE, amount + TX_FEE);
        return 0;
    }

    sender->balance -= amount + TX_FEE;      /* debit  */
    recipient->balance += amount;            /* credit */
    log_tx(sender, "TRANSFER", from, to, amount, TX_FEE, nonce);
    log_tx(recipient, "TRANSFER", from, to, amount, TX_FEE, nonce);
    sender->nonce++;

    *fee_out = TX_FEE;
    if (!verbose) return 1;
    printf("Transfer: %s -> %s, amount %ld, fee %d, nonce %ld. %s now has %ld (next nonce %ld).\n",
           from, to, amount, TX_FEE, nonce, from, sender->balance, sender->nonce);
    return 1;
}

void account_print_balances(const AccountLedger *led) {
    printf("Account balances:\n");
    printf("  %-10s %-8s %s\n", "Member", "Balance", "Next nonce");
    for (size_t i = 0; i < led->count; i++) {
        printf("  %-10s %-8ld %ld\n", led->accounts[i].member_id,
               led->accounts[i].balance, led->accounts[i].nonce);
    }
}

int account_print_history(AccountLedger *led, const char *member_id) {
    Account *acc = account_find(led, member_id);
    if (!acc) return 0;
    printf("Transaction history for %s (balance %ld, next nonce %ld):\n",
           member_id, acc->balance, acc->nonce);
    if (!acc->history_head) {
        printf("  (no transactions)\n");
        return 1;
    }
    printf("  %-9s %-10s %-10s %-7s %-4s %s\n", "Kind", "Sender", "Recipient", "Amount", "Fee", "Nonce");
    for (const TxRecord *r = acc->history_head; r; r = r->next) {
        char nonce_str[24];
        if (r->nonce < 0) snprintf(nonce_str, sizeof(nonce_str), "-");
        else snprintf(nonce_str, sizeof(nonce_str), "%ld", r->nonce);
        printf("  %-9s %-10s %-10s %-7ld %-4ld %s\n",
               r->kind, r->sender, r->recipient, r->amount, r->fee, nonce_str);
    }
    return 1;
}
