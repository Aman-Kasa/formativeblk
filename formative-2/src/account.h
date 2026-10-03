#ifndef ACCOUNT_H
#define ACCOUNT_H

#include <time.h>

#include "registry.h"

/* Account model. Each registered member is an account holding a balance
 * that is debited and credited directly, plus a nonce: the number of
 * outgoing transactions the account has made. A transfer must quote the
 * account's current nonce exactly; this rejects replayed (reused) and
 * out-of-order transactions. */

typedef struct TxRecord {
    char kind[10];                   /* REWARD or TRANSFER                  */
    char sender[MEMBER_ID_LEN];      /* "SYSTEM" for rewards                */
    char recipient[MEMBER_ID_LEN];
    long amount;                     /* amount the recipient received       */
    long fee;
    long nonce;                      /* sender's nonce, -1 for rewards      */
    time_t timestamp;
    struct TxRecord *next;
} TxRecord;

typedef struct {
    char member_id[MEMBER_ID_LEN];
    long balance;
    long nonce;                      /* next nonce this account must use    */
    TxRecord *history_head;          /* linked list, oldest first           */
    TxRecord *history_tail;
} Account;

typedef struct {
    Account accounts[MAX_MEMBERS];
    size_t count;
} AccountLedger;

/* One account per registered member, every balance and nonce starting at 0. */
void account_init(AccountLedger *led, const Registry *reg);
void account_free(AccountLedger *led);

Account *account_find(AccountLedger *led, const char *member_id);

/* Confirmed reward: credits (gross - TX_FEE) to the member and logs it.
 * Returns the fee collected (for the miner), or 0 if nothing was credited. */
long account_credit_reward(AccountLedger *led, const char *member_id, long gross);

/* Member-to-member transfer. Rejected (returns 0, reason in err) when either
 * member is unknown, they are the same member, amount <= 0, the nonce is not
 * exactly the sender's current nonce, or the balance cannot cover
 * amount + TX_FEE. On success: sender debited amount + fee, recipient
 * credited amount, sender nonce incremented, and the transaction logged in
 * both members' histories. *fee_out receives the fee. */
int account_transfer(AccountLedger *led, const char *from, const char *to,
                     long amount, long nonce, long *fee_out, char *err, size_t err_len, int verbose);

void account_print_balances(const AccountLedger *led);
/* Returns 0 if the member has no account. */
int account_print_history(AccountLedger *led, const char *member_id);

#endif
