#include "ledger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define LOG_LINE_LEN 512
#define LOG_FIELDS 6     /* tag|from|to|amount|extra|signature */

void ledger_init(Ledger *led, LedgerModel model, const Registry *reg,
                 const char *log_path, EVP_PKEY *priv_key, EVP_PKEY *pub_key) {
    led->model = model;
    led->reg = reg;
    led->unclaimed_fees = 0;
    led->log_path = log_path;
    led->priv_key = priv_key;
    led->pub_key = pub_key;
    utxo_init(&led->utxo);
    account_init(&led->accounts, reg);
}

void ledger_free(Ledger *led) {
    utxo_free(&led->utxo);
    account_free(&led->accounts);
}

const char *ledger_model_name(LedgerModel model) {
    return model == MODEL_UTXO ? "UTXO" : "Account-based";
}

long ledger_apply_confirmed_block(Ledger *led, const Block *b) {
    /* Only a RETURNED block carries a reward. BORROWED and OVERDUE blocks
     * have token_reward 0 and an empty tx_id: no transaction exists. */
    if (b->token_reward <= 0 || b->tx_id[0] == '\0') return 0;
    if (led->model == MODEL_UTXO) {
        return utxo_credit_reward(&led->utxo, b->member_id, b->token_reward, b->tx_id);
    }
    return account_credit_reward(&led->accounts, b->member_id, b->token_reward);
}

/* ===================================================================== */
/* Signed transfer log                                                   */
/* ===================================================================== */

/* "extra" is the UTXO input list ("3,7" or "-" for automatic selection)
 * or the account nonce. The signature covers SHA-256 of everything else. */
static int append_log(const Ledger *led, size_t tag, const char *from, const char *to,
                      long amount, const char *extra) {
    char body[LOG_LINE_LEN];
    snprintf(body, sizeof(body), "%zu|%s|%s|%ld|%s", tag, from, to, amount, extra);

    char digest[HASH_HEX_LEN];
    crypto_sha256_hex((const unsigned char *)body, strlen(body), digest);
    unsigned char sig[MAX_SIGNATURE_LEN];
    size_t sig_len = 0;
    if (!crypto_sign_hash(led->priv_key, digest, sig, sizeof(sig), &sig_len)) return 0;
    char sig_hex[MAX_SIGNATURE_LEN * 2 + 1];
    crypto_bytes_to_hex(sig, sig_len, sig_hex, sizeof(sig_hex));

    FILE *f = fopen(led->log_path, "a");
    if (!f) return 0;
    fprintf(f, "%s|%s\n", body, sig_hex);
    fclose(f);
    return 1;
}

static int apply_transfer(Ledger *led, const char *from, const char *to, long amount,
                          const char *extra, long *fee, char *err, size_t err_len, int verbose) {
    if (led->model == MODEL_ACCOUNT) {
        char *end;
        errno = 0;
        long nonce = strtol(extra, &end, 10);
        if (*extra == '\0' || *end != '\0' || errno) {
            snprintf(err, err_len, "bad nonce field");
            return 0;
        }
        return account_transfer(&led->accounts, from, to, amount, nonce, fee, err, err_len, verbose);
    }

    int ids[UTXO_MAX_INPUTS];
    size_t n = 0;
    if (strcmp(extra, "-") != 0) {
        char copy[LOG_LINE_LEN];
        snprintf(copy, sizeof(copy), "%s", extra);
        for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
            char *end;
            long id = strtol(t, &end, 10);
            if (*end != '\0' || id <= 0 || n == UTXO_MAX_INPUTS) {
                snprintf(err, err_len, "bad input list");
                return 0;
            }
            ids[n++] = (int)id;
        }
    }
    return utxo_transfer(&led->utxo, led->reg, from, to, amount, ids, n, fee, NULL,
                         err, err_len, verbose);
}

int ledger_transfer_utxo(Ledger *led, size_t chain_length, const char *from, const char *to,
                         long amount, const int *input_ids, size_t n_inputs) {
    char extra[LOG_LINE_LEN] = "-";
    if (n_inputs > 0) {
        size_t len = 0;
        extra[0] = '\0';
        for (size_t i = 0; i < n_inputs && len < sizeof(extra); i++) {
            len += (size_t)snprintf(extra + len, sizeof(extra) - len, "%s%d", i ? "," : "", input_ids[i]);
        }
    }
    char err[256];
    long fee = 0;
    if (!apply_transfer(led, from, to, amount, extra, &fee, err, sizeof(err), 1)) {
        printf("%s\n", err);
        return 0;
    }
    led->unclaimed_fees += fee;
    if (!append_log(led, chain_length, from, to, amount, extra)) {
        printf("WARNING: transfer applied but could not be written to '%s'.\n", led->log_path);
    }
    return 1;
}

int ledger_transfer_account(Ledger *led, size_t chain_length, const char *from, const char *to,
                            long amount, long nonce) {
    char extra[32];
    snprintf(extra, sizeof(extra), "%ld", nonce);
    char err[256];
    long fee = 0;
    if (!apply_transfer(led, from, to, amount, extra, &fee, err, sizeof(err), 1)) {
        printf("%s\n", err);
        return 0;
    }
    led->unclaimed_fees += fee;
    if (!append_log(led, chain_length, from, to, amount, extra)) {
        printf("WARNING: transfer applied but could not be written to '%s'.\n", led->log_path);
    }
    return 1;
}

/* ===================================================================== */
/* Replay                                                                */
/* ===================================================================== */

typedef struct {
    size_t tag;
    char from[MEMBER_ID_LEN];
    char to[MEMBER_ID_LEN];
    long amount;
    char extra[LOG_LINE_LEN];
} LogEntry;

static int parse_log_line(const Ledger *led, char *line, LogEntry *e) {
    char *f[LOG_FIELDS];
    int n = 0;
    char *p = line;
    f[n++] = p;
    for (char *bar = strchr(p, '|'); bar; bar = strchr(p, '|')) {
        if (n == LOG_FIELDS) return 0;
        *bar = '\0';
        p = bar + 1;
        f[n++] = p;
    }
    if (n != LOG_FIELDS) return 0;

    /* Verify the signature over the body before trusting any field. */
    char body[LOG_LINE_LEN];
    snprintf(body, sizeof(body), "%s|%s|%s|%s|%s", f[0], f[1], f[2], f[3], f[4]);
    char digest[HASH_HEX_LEN];
    crypto_sha256_hex((const unsigned char *)body, strlen(body), digest);
    unsigned char sig[MAX_SIGNATURE_LEN];
    size_t sig_len = crypto_hex_to_bytes(f[5], sig, sizeof(sig));
    if (sig_len == 0 || !crypto_verify_hash(led->pub_key, digest, sig, sig_len)) return 0;

    char *end;
    errno = 0;
    unsigned long tag = strtoul(f[0], &end, 10);
    if (*f[0] == '\0' || *end != '\0' || errno) return 0;
    long amount = strtol(f[3], &end, 10);
    if (*f[3] == '\0' || *end != '\0' || errno) return 0;
    if (strlen(f[1]) >= MEMBER_ID_LEN || strlen(f[2]) >= MEMBER_ID_LEN) return 0;

    e->tag = tag;
    e->amount = amount;
    snprintf(e->from, MEMBER_ID_LEN, "%s", f[1]);
    snprintf(e->to, MEMBER_ID_LEN, "%s", f[2]);
    snprintf(e->extra, sizeof(e->extra), "%s", f[4]);
    return 1;
}

int ledger_replay(Ledger *led, const Blockchain *chain) {
    LogEntry *entries = NULL;
    size_t count = 0, cap = 0;

    FILE *f = fopen(led->log_path, "r");
    if (f) {
        char line[LOG_LINE_LEN];
        size_t line_no = 0;
        while (fgets(line, sizeof(line), f)) {
            line_no++;
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0') continue;
            if (count == cap) {
                cap = cap ? cap * 2 : 16;
                LogEntry *grown = realloc(entries, cap * sizeof(LogEntry));
                if (!grown) { fclose(f); free(entries); return 0; }
                entries = grown;
            }
            if (!parse_log_line(led, line, &entries[count])) {
                fprintf(stderr, "ERROR: transfer log '%s' line %zu is malformed or its signature "
                                "is invalid.\n", led->log_path, line_no);
                fclose(f);
                free(entries);
                return 0;
            }
            size_t prev_tag = count ? entries[count - 1].tag : 1;
            if (entries[count].tag < prev_tag || entries[count].tag > chain->length) {
                fprintf(stderr, "ERROR: transfer log '%s' line %zu refers to chain length %zu, "
                                "which does not match the chain.\n",
                        led->log_path, line_no, entries[count].tag);
                fclose(f);
                free(entries);
                return 0;
            }
            count++;
        }
        fclose(f);
    }

    /* Interleave: after confirming block i the chain length is i + 1, and
     * every transfer made at that chain length is replayed next. */
    size_t next = 0, length = 0;
    int ok = 1;
    for (const Block *b = chain->head; b && ok; b = b->next) {
        ledger_apply_confirmed_block(led, b);
        length++;
        while (next < count && entries[next].tag == length) {
            char err[256];
            long fee = 0;
            if (!apply_transfer(led, entries[next].from, entries[next].to, entries[next].amount,
                                entries[next].extra, &fee, err, sizeof(err), 0)) {
                fprintf(stderr, "ERROR: transfer log entry %zu no longer validates (%s).\n",
                        next + 1, err);
                ok = 0;
                break;
            }
            /* Fees from transfers made before the latest block were already
             * paid to that block's miner; only the newest ones are unclaimed. */
            if (length == chain->length) led->unclaimed_fees += fee;
            next++;
        }
    }
    free(entries);
    return ok;
}

void ledger_print(const Ledger *led) {
    if (led->model == MODEL_UTXO) {
        utxo_print_set(&led->utxo, led->reg);
    } else {
        account_print_balances(&led->accounts);
    }
}

long ledger_take_unclaimed_fees(Ledger *led) {
    long fees = led->unclaimed_fees;
    led->unclaimed_fees = 0;
    return fees;
}
