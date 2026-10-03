#ifndef DISPLAY_H
#define DISPLAY_H

#include "blockchain.h"

/* Shared output formatting used by both the CLI and the mining module. */

void format_time(time_t t, char *out, size_t out_len);

/* Prints one confirmed block: action, book title, member name, timestamp,
 * token reward, tx_id, proof-of-work details and signature validity. */
void print_confirmed_block(const Block *b, EVP_PKEY *pub_key);

/* Prints the unconfirmed records waiting in the pending pool. */
void print_pending_pool(const PendingPool *pool);

#endif
