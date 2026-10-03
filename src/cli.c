#include "cli.h"
#include "persistence.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define INPUT_BUF_LEN 256

static void trim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

static void print_help(void) {
    printf(
        "Commands:\n"
        "  borrow <book_id> <member_id>   Record a book as borrowed\n"
        "  return <book_id>                Record a book as returned\n"
        "  view records                    Show every lending record on the chain\n"
        "  validate chain                  Verify chain integrity (hashes, links, signatures)\n"
        "  list books                      Show the book registry\n"
        "  list members                    Show the member registry\n"
        "  chain status                    Show block count and validity at a glance\n"
        "  help                            Show this message\n"
        "  exit                            Quit\n");
}

static void format_time(time_t t, char *out, size_t out_len) {
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

static void print_records(const Blockchain *chain, EVP_PKEY *pub_key) {
    printf("Lending chain (%zu blocks):\n", chain->length);
    for (const Block *b = chain->head; b; b = b->next) {
        char ts[32];
        format_time(b->timestamp, ts, sizeof(ts));
        int sig_ok = crypto_verify_hash(pub_key, b->hash, b->signature, b->sig_len);

        if (b->index == 0) {
            printf("  [%d] GENESIS  time=%s\n", b->index, ts);
        } else {
            printf("  [%d] %-8s book=%s (%s) member=%s (%s) time=%s\n",
                   b->index, b->action, b->book_id, b->book_title,
                   b->member_id, b->member_name, ts);
        }
        printf("        hash=%s\n", b->hash);
        printf("        prev=%s\n", b->previous_hash);
        printf("        signature=%s\n", sig_ok ? "VALID" : "INVALID");
    }
}

static void print_status(const Blockchain *chain, EVP_PKEY *pub_key) {
    ChainValidation v = blockchain_validate(chain, pub_key);
    printf("Chain status: %zu block(s), %s\n", chain->length, v.valid ? "VALID" : "COMPROMISED");
    if (!v.valid) {
        printf("  first problem at block index %d: %s\n", v.bad_index, v.reason);
    }
}

/* A block appended on top of an already-compromised chain would itself hash
 * and sign cleanly, quietly laundering the tamper — so every write first
 * re-checks chain integrity and refuses if it doesn't hold. */
static int refuse_if_compromised(const Blockchain *chain, EVP_PKEY *pub_key) {
    ChainValidation v = blockchain_validate(chain, pub_key);
    if (!v.valid) {
        printf("ERROR: chain integrity check failed at block %d (%s) — refusing to add new records "
               "until this is resolved.\n", v.bad_index, v.reason);
        return 1;
    }
    return 0;
}

static void handle_borrow(Blockchain *chain, const Registry *reg, EVP_PKEY *priv_key, EVP_PKEY *pub_key,
                           const char *chain_path, char *rest) {
    if (refuse_if_compromised(chain, pub_key)) return;

    char book_id[BOOK_ID_LEN], member_id[MEMBER_ID_LEN];
    if (sscanf(rest, "%19s %19s", book_id, member_id) != 2) {
        printf("Usage: borrow <book_id> <member_id>\n");
        return;
    }

    LendResult r = blockchain_borrow(chain, reg, priv_key, book_id, member_id);
    if (r != LEND_OK) {
        printf("%s\n", lend_result_message(r));
        return;
    }
    if (!persistence_save(chain, chain_path)) {
        printf("WARNING: block was recorded in memory but could not be persisted to disk.\n");
        return;
    }
    printf("OK: %s borrowed by %s (block %d recorded).\n", book_id, member_id, (int)chain->length - 1);
}

static void handle_return(Blockchain *chain, const Registry *reg, EVP_PKEY *priv_key, EVP_PKEY *pub_key,
                           const char *chain_path, char *rest) {
    if (refuse_if_compromised(chain, pub_key)) return;

    char book_id[BOOK_ID_LEN];
    if (sscanf(rest, "%19s", book_id) != 1) {
        printf("Usage: return <book_id>\n");
        return;
    }

    LendResult r = blockchain_return(chain, reg, priv_key, book_id);
    if (r != LEND_OK) {
        printf("%s\n", lend_result_message(r));
        return;
    }
    if (!persistence_save(chain, chain_path)) {
        printf("WARNING: block was recorded in memory but could not be persisted to disk.\n");
        return;
    }
    printf("OK: %s returned (block %d recorded).\n", book_id, (int)chain->length - 1);
}

int cli_run(Blockchain *chain, const Registry *reg,
            EVP_PKEY *priv_key, EVP_PKEY *pub_key, const char *chain_path) {
    char line[INPUT_BUF_LEN];

    printf("Library Lending Chain — type 'help' for commands.\n");

    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        trim(line);
        if (line[0] == '\0') continue;

        if (strncasecmp(line, "borrow ", 7) == 0) {
            handle_borrow(chain, reg, priv_key, pub_key, chain_path, line + 7);
        } else if (strncasecmp(line, "return ", 7) == 0) {
            handle_return(chain, reg, priv_key, pub_key, chain_path, line + 7);
        } else if (strcasecmp(line, "view records") == 0 || strcasecmp(line, "view") == 0) {
            print_records(chain, pub_key);
        } else if (strcasecmp(line, "validate chain") == 0 || strcasecmp(line, "validate") == 0) {
            print_status(chain, pub_key);
        } else if (strcasecmp(line, "list books") == 0) {
            printf("Books (%zu):\n", reg->book_count);
            for (size_t i = 0; i < reg->book_count; i++) {
                printf("  %-8s %-40s %s\n", reg->books[i].book_id, reg->books[i].title, reg->books[i].author);
            }
        } else if (strcasecmp(line, "list members") == 0) {
            printf("Members (%zu):\n", reg->member_count);
            for (size_t i = 0; i < reg->member_count; i++) {
                printf("  %-8s %-24s %s\n", reg->members[i].member_id, reg->members[i].full_name, reg->members[i].course_code);
            }
        } else if (strcasecmp(line, "chain status") == 0) {
            print_status(chain, pub_key);
        } else if (strcasecmp(line, "help") == 0) {
            print_help();
        } else if (strcasecmp(line, "exit") == 0 || strcasecmp(line, "quit") == 0) {
            break;
        } else {
            printf("Unrecognized command: '%s'. Type 'help' for the command list.\n", line);
        }
    }

    return 0;
}
