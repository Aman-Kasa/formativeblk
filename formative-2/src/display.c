#include "display.h"

#include <stdio.h>
#include <string.h>

void format_time(time_t t, char *out, size_t out_len) {
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

void print_confirmed_block(const Block *b, EVP_PKEY *pub_key) {
    char ts[32];
    format_time(b->timestamp, ts, sizeof(ts));
    int sig_ok = crypto_verify_hash(pub_key, b->hash, b->signature, b->sig_len);

    if (strcmp(b->action, "GENESIS") == 0) {
        printf("  [%d] GENESIS   time=%s\n", b->index, ts);
    } else {
        printf("  [%d] %-9s \"%s\" (%s) - %s (%s)\n",
               b->index, b->action, b->book_title, b->book_id, b->member_name, b->member_id);
        printf("        time=%s  token_reward=%d  tx_id=%s\n",
               ts, b->token_reward, b->tx_id[0] ? b->tx_id : "none (no reward transaction)");
    }
    printf("        hash=%s\n", b->hash);
    printf("        prev=%s\n", b->previous_hash);
    printf("        difficulty=%d  nonce=%lu  signature=%s\n",
           b->difficulty, b->nonce, sig_ok ? "VALID" : "INVALID");
}

void print_pending_pool(const PendingPool *pool) {
    printf("Pending pool: %zu unconfirmed record(s) waiting to be mined\n", pool->length);
    if (pool->length == 0) return;
    printf("  %-3s %-9s %-7s %-26s %-8s %-19s %-6s %s\n",
           "#", "Action", "Book", "Title", "Member", "Time", "Reward", "tx_id");
    int i = 1;
    for (const Block *b = pool->head; b; b = b->next, i++) {
        char ts[32];
        format_time(b->timestamp, ts, sizeof(ts));
        printf("  %-3d %-9s %-7s %-26.26s %-8s %-19s %-6d %.16s%s\n",
               i, b->action, b->book_id, b->book_title, b->member_id, ts,
               b->token_reward, b->tx_id[0] ? b->tx_id : "none",
               b->tx_id[0] ? "..." : "");
    }
}
