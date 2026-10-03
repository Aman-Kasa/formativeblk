#ifndef BLOCK_H
#define BLOCK_H

#include <time.h>
#include <stddef.h>

#include "registry.h"
#include "crypto.h"

#define ACTION_LEN 10

/* One lending record. Formative 1 fields are unchanged; Formative 2 adds
 * token_reward and tx_id (required by the brief) plus difficulty and nonce,
 * which proof of work needs: nonce is the counter the miner increments, and
 * difficulty is stored per block so validation can re-check each block
 * against the target it was actually mined at (difficulty is adjustable
 * between mining runs). */
typedef struct Block {
    /* ---- Formative 1 fields ---- */
    int index;
    time_t timestamp;
    char book_id[BOOK_ID_LEN];
    char book_title[BOOK_TITLE_LEN];
    char member_id[MEMBER_ID_LEN];
    char member_name[MEMBER_NAME_LEN];
    char action[ACTION_LEN];            /* GENESIS, BORROWED, RETURNED, OVERDUE */
    char previous_hash[HASH_HEX_LEN];
    unsigned char signature[MAX_SIGNATURE_LEN];
    char hash[HASH_HEX_LEN];

    /* ---- Formative 2 fields ---- */
    int token_reward;                   /* 10 on time, 5 late, 0 otherwise      */
    char tx_id[HASH_HEX_LEN];           /* SHA-256 of the reward transaction,
                                           empty string when no reward exists  */
    int difficulty;                     /* leading '0's this block was mined at */
    unsigned long nonce;                /* proof-of-work counter                */

    /* In-memory only: ECDSA DER signatures vary in length (70-72 bytes for
     * P-256) but the assignment's signature field is a fixed 72-byte array,
     * so the real length is tracked here. On disk it is implied by the hex
     * string's length. */
    size_t sig_len;

    struct Block *next;
} Block;

/* A singly linked list of blocks. Used both for the confirmed chain and for
 * the pending pool of unconfirmed lending records. */
typedef struct {
    Block *head;
    Block *tail;
    size_t length;
} BlockList;

void blocklist_init(BlockList *list);
void blocklist_free(BlockList *list);
void blocklist_append(BlockList *list, Block *b);
/* Detaches and returns the first block, or NULL if the list is empty. */
Block *blocklist_pop_front(BlockList *list);

#endif
