#include "persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PERSIST_FIELDS 10
#define LINE_BUF_LEN 1024

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Splits `line` in place on '|' into at most PERSIST_FIELDS fields. */
static int split_fields(char *line, char *fields[PERSIST_FIELDS]) {
    int count = 0;
    char *p = line;
    fields[count++] = p;
    while (count < PERSIST_FIELDS) {
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = '\0';
        p = bar + 1;
        fields[count++] = p;
    }
    return count;
}

/* Parses a base-10 integer with full error checking — no bare atoi/atol,
 * per Standing Rule 8 (integer-conversion errors must be caught). */
static int parse_long(const char *s, long *out) {
    if (*s == '\0') return 0;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0') return 0;
    *out = v;
    return 1;
}

static Block *parse_line(char *line) {
    char *fields[PERSIST_FIELDS];
    int n = split_fields(line, fields);
    if (n != PERSIST_FIELDS) return NULL;

    long index_val, timestamp_val;
    if (!parse_long(fields[0], &index_val)) return NULL;
    if (!parse_long(fields[1], &timestamp_val)) return NULL;

    if (strlen(fields[2]) >= BOOK_ID_LEN) return NULL;
    if (strlen(fields[3]) >= BOOK_TITLE_LEN) return NULL;
    if (strlen(fields[4]) >= MEMBER_ID_LEN) return NULL;
    if (strlen(fields[5]) >= MEMBER_NAME_LEN) return NULL;
    if (strlen(fields[6]) >= ACTION_LEN) return NULL;
    if (strlen(fields[7]) >= HASH_HEX_LEN) return NULL;
    if (strlen(fields[8]) >= MAX_SIGNATURE_LEN * 2 + 1) return NULL;
    if (strlen(fields[9]) >= HASH_HEX_LEN) return NULL;

    Block *b = calloc(1, sizeof(Block));
    if (!b) return NULL;

    b->index = (int)index_val;
    b->timestamp = (time_t)timestamp_val;
    snprintf(b->book_id, BOOK_ID_LEN, "%s", fields[2]);
    snprintf(b->book_title, BOOK_TITLE_LEN, "%s", fields[3]);
    snprintf(b->member_id, MEMBER_ID_LEN, "%s", fields[4]);
    snprintf(b->member_name, MEMBER_NAME_LEN, "%s", fields[5]);
    snprintf(b->action, ACTION_LEN, "%s", fields[6]);
    snprintf(b->previous_hash, HASH_HEX_LEN, "%s", fields[7]);

    b->sig_len = crypto_hex_to_bytes(fields[8], b->signature, sizeof(b->signature));
    if (b->sig_len == 0 && fields[8][0] != '\0') {
        /* non-empty hex that failed to decode (odd length / bad chars) — corrupt line */
        free(b);
        return NULL;
    }

    snprintf(b->hash, HASH_HEX_LEN, "%s", fields[9]);
    b->next = NULL;
    return b;
}

PersistResult persistence_load(Blockchain *chain, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return PERSIST_NOT_FOUND;

    char line[LINE_BUF_LEN];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (line[0] == '\0') continue;

        Block *b = parse_line(line);
        if (!b) {
            fprintf(stderr, "ERROR: chain file '%s' contains an unparsable line — treating chain as corrupt.\n", path);
            fclose(f);
            blockchain_free(chain);
            return PERSIST_CORRUPT;
        }

        b->next = NULL;
        if (chain->tail) {
            chain->tail->next = b;
        } else {
            chain->head = b;
        }
        chain->tail = b;
        chain->length++;
    }

    fclose(f);

    if (chain->length == 0) {
        fprintf(stderr, "ERROR: chain file '%s' exists but contains no valid blocks — treating chain as corrupt.\n", path);
        return PERSIST_CORRUPT;
    }

    return PERSIST_LOADED;
}

int persistence_save(const Blockchain *chain, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "ERROR: could not open '%s' for writing the chain.\n", path);
        return 0;
    }

    for (const Block *b = chain->head; b; b = b->next) {
        char sig_hex[MAX_SIGNATURE_LEN * 2 + 1];
        crypto_bytes_to_hex(b->signature, b->sig_len, sig_hex, sizeof(sig_hex));

        fprintf(f, "%d|%ld|%s|%s|%s|%s|%s|%s|%s|%s\n",
                b->index, (long)b->timestamp,
                b->book_id, b->book_title,
                b->member_id, b->member_name,
                b->action, b->previous_hash,
                sig_hex, b->hash);
    }

    fclose(f);
    return 1;
}
