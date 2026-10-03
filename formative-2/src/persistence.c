#include "persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CHAIN_FIELDS   14
#define PENDING_FIELDS  8
#define MAX_FIELDS     14
#define LINE_BUF_LEN 1024

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Splits `line` in place on '|'. Returns the number of fields found; a line
 * with more separators than `max` yields max+1 so the caller rejects it. */
static int split_fields(char *line, char *fields[], int max) {
    int count = 0;
    char *p = line;
    fields[count++] = p;
    for (char *bar = strchr(p, '|'); bar; bar = strchr(p, '|')) {
        if (count == max) return max + 1;
        *bar = '\0';
        p = bar + 1;
        fields[count++] = p;
    }
    return count;
}

/* Base-10 integer parsing with full error checking (no bare atoi/atol). */
static int parse_long(const char *s, long *out) {
    if (*s == '\0') return 0;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0') return 0;
    *out = v;
    return 1;
}

static int parse_ulong(const char *s, unsigned long *out) {
    if (*s == '\0' || *s == '-') return 0;
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0') return 0;
    *out = v;
    return 1;
}

static int fits(const char *s, size_t cap) { return strlen(s) < cap; }

/* Copies the lending-record fields shared by both file formats. */
static int fill_record(Block *b, char *ts, char *book_id, char *title, char *member_id,
                       char *name, char *action, char *reward, char *tx_id) {
    long ts_val, reward_val;
    if (!parse_long(ts, &ts_val) || !parse_long(reward, &reward_val)) return 0;
    if (!fits(book_id, BOOK_ID_LEN) || !fits(title, BOOK_TITLE_LEN) ||
        !fits(member_id, MEMBER_ID_LEN) || !fits(name, MEMBER_NAME_LEN) ||
        !fits(action, ACTION_LEN) || !fits(tx_id, HASH_HEX_LEN)) return 0;

    b->timestamp = (time_t)ts_val;
    b->token_reward = (int)reward_val;
    snprintf(b->book_id, BOOK_ID_LEN, "%s", book_id);
    snprintf(b->book_title, BOOK_TITLE_LEN, "%s", title);
    snprintf(b->member_id, MEMBER_ID_LEN, "%s", member_id);
    snprintf(b->member_name, MEMBER_NAME_LEN, "%s", name);
    snprintf(b->action, ACTION_LEN, "%s", action);
    snprintf(b->tx_id, HASH_HEX_LEN, "%s", tx_id);
    return 1;
}

static Block *parse_chain_line(char *line) {
    char *f[MAX_FIELDS];
    if (split_fields(line, f, CHAIN_FIELDS) != CHAIN_FIELDS) return NULL;

    long index_val, difficulty_val;
    unsigned long nonce_val;
    if (!parse_long(f[0], &index_val) || !parse_long(f[8], &difficulty_val) ||
        !parse_ulong(f[9], &nonce_val)) return NULL;
    if (!fits(f[7], HASH_HEX_LEN) || !fits(f[12], MAX_SIGNATURE_LEN * 2 + 1) ||
        !fits(f[13], HASH_HEX_LEN)) return NULL;

    Block *b = calloc(1, sizeof(Block));
    if (!b) return NULL;
    if (!fill_record(b, f[1], f[2], f[3], f[4], f[5], f[6], f[10], f[11])) {
        free(b);
        return NULL;
    }
    b->index = (int)index_val;
    b->difficulty = (int)difficulty_val;
    b->nonce = nonce_val;
    snprintf(b->previous_hash, HASH_HEX_LEN, "%s", f[7]);
    snprintf(b->hash, HASH_HEX_LEN, "%s", f[13]);

    b->sig_len = crypto_hex_to_bytes(f[12], b->signature, sizeof(b->signature));
    if (b->sig_len == 0 && f[12][0] != '\0') {   /* bad hex = corrupt line */
        free(b);
        return NULL;
    }
    return b;
}

static Block *parse_pending_line(char *line) {
    char *f[MAX_FIELDS];
    if (split_fields(line, f, PENDING_FIELDS) != PENDING_FIELDS) return NULL;

    Block *b = calloc(1, sizeof(Block));
    if (!b) return NULL;
    if (!fill_record(b, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7])) {
        free(b);
        return NULL;
    }
    b->index = -1;
    return b;
}

static PersistResult load_lines(BlockList *list, const char *path, Block *(*parse)(char *),
                                const char *what) {
    FILE *f = fopen(path, "r");
    if (!f) return PERSIST_NOT_FOUND;

    char line[LINE_BUF_LEN];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (line[0] == '\0') continue;
        Block *b = parse(line);
        if (!b) {
            fprintf(stderr, "ERROR: %s file '%s' contains an unparsable line.\n", what, path);
            fclose(f);
            blocklist_free(list);
            return PERSIST_CORRUPT;
        }
        blocklist_append(list, b);
    }
    fclose(f);
    return PERSIST_LOADED;
}

PersistResult persistence_load_chain(Blockchain *chain, const char *path) {
    PersistResult r = load_lines(chain, path, parse_chain_line, "chain");
    if (r == PERSIST_LOADED && chain->length == 0) {
        fprintf(stderr, "ERROR: chain file '%s' exists but contains no blocks.\n", path);
        return PERSIST_CORRUPT;
    }
    return r;
}

PersistResult persistence_load_pending(PendingPool *pool, const char *path) {
    return load_lines(pool, path, parse_pending_line, "pending pool");
}

int persistence_save_chain(const Blockchain *chain, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "ERROR: could not open '%s' for writing the chain.\n", path);
        return 0;
    }
    for (const Block *b = chain->head; b; b = b->next) {
        char sig_hex[MAX_SIGNATURE_LEN * 2 + 1];
        crypto_bytes_to_hex(b->signature, b->sig_len, sig_hex, sizeof(sig_hex));
        fprintf(f, "%d|%ld|%s|%s|%s|%s|%s|%s|%d|%lu|%d|%s|%s|%s\n",
                b->index, (long)b->timestamp,
                b->book_id, b->book_title, b->member_id, b->member_name,
                b->action, b->previous_hash,
                b->difficulty, b->nonce, b->token_reward, b->tx_id,
                sig_hex, b->hash);
    }
    fclose(f);
    return 1;
}

int persistence_save_pending(const PendingPool *pool, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "ERROR: could not open '%s' for writing the pending pool.\n", path);
        return 0;
    }
    for (const Block *b = pool->head; b; b = b->next) {
        fprintf(f, "%ld|%s|%s|%s|%s|%s|%d|%s\n",
                (long)b->timestamp, b->book_id, b->book_title, b->member_id,
                b->member_name, b->action, b->token_reward, b->tx_id);
    }
    fclose(f);
    return 1;
}
