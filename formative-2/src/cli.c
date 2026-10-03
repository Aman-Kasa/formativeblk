#include "cli.h"
#include "config.h"
#include "display.h"
#include "persistence.h"
#include "simclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#define INPUT_BUF_LEN 256
#define MAX_TOKENS 24

static void trim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

/* Splits on spaces in place. Returns the number of tokens. */
static int tokenize(char *line, char *tok[], int max) {
    int n = 0;
    for (char *t = strtok(line, " \t"); t && n < max; t = strtok(NULL, " \t")) tok[n++] = t;
    return n;
}

/* Strict whole-number parsing: rejects empty strings, trailing junk and overflow. */
static int parse_long_arg(const char *s, long *out) {
    if (!s || *s == '\0') return 0;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0') return 0;
    *out = v;
    return 1;
}

static void print_help(const MiningContext *app) {
    printf("Lending (records go to the pending pool until mined):\n"
           "  borrow <book_id> <member_id>   Queue a BORROWED record\n"
           "  return <book_id>               Queue a RETURNED record + reward transaction\n"
           "                                 (%d coins on time, %d coins late; loan period %d days)\n"
           "  check overdue                  Queue OVERDUE records (no reward transaction)\n"
           "  advance <days>                 Move the simulated clock forward (demo late/overdue)\n"
           "  pending                        Show the pending pool\n"
           "Mining (confirms pending records onto the chain):\n"
           "  mine solo                      One miner, full reward, shows hash attempts\n"
           "  mine pool [miners]             %d-%d miners share rewards (default %d, %.0f%% pool fee)\n"
           "  mine cloud <rounds>            Rent hash power for %d-%d rounds, profit summary\n"
           "  difficulty [1-4]               Show or set proof-of-work difficulty\n"
           "Tokens (%s model):\n"
           "  balances                       Show member balances%s\n",
           REWARD_ON_TIME, REWARD_LATE, LOAN_PERIOD_DAYS,
           POOL_MIN_MINERS, POOL_MAX_MINERS, POOL_DEFAULT_MINERS, POOL_FEE_PERCENT,
           CLOUD_MIN_ROUNDS, CLOUD_MAX_ROUNDS,
           ledger_model_name(app->ledger->model),
           app->ledger->model == MODEL_UTXO ? " and the full UTXO set" : "");
    if (app->ledger->model == MODEL_UTXO) {
        printf("  utxos                          Show the full UTXO set\n"
               "  transfer <from> <to> <amount> [#id ...]\n"
               "                                 Spend UTXOs (auto-selected, or the listed ids)\n");
    } else {
        printf("  transfer <from> <to> <amount> <nonce>\n"
               "                                 Transfer coins; nonce must be the sender's next nonce\n"
               "  history <member_id>            Show a member's transaction history\n");
    }
    printf("Chain:\n"
           "  view records                   Show every confirmed block\n"
           "  validate chain                 Verify hashes, links, proof of work, tx_ids, signatures\n"
           "  list books | list members      Show the registries\n"
           "  help | exit\n");
}

/* Every write is refused on a chain that fails validation: new blocks mined
 * on top of a tampered chain would otherwise launder the tampering. */
static int refuse_if_compromised(const MiningContext *app) {
    ChainValidation v = blockchain_validate(app->chain, app->pub_key);
    if (!v.valid) {
        printf("ERROR: chain integrity check failed at block %d (%s) - refusing to change "
               "anything until this is resolved.\n", v.bad_index, v.reason);
        return 1;
    }
    return 0;
}

static void save_pending(const MiningContext *app) {
    if (!persistence_save_pending(app->pool, app->pending_path)) {
        printf("WARNING: pending pool could not be saved to disk.\n");
    }
}

static void cmd_borrow(MiningContext *app, char *tok[], int n) {
    if (n != 3) { printf("Usage: borrow <book_id> <member_id>\n"); return; }
    if (refuse_if_compromised(app)) return;
    LendResult r = lending_borrow(app->chain, app->pool, app->reg, tok[1], tok[2]);
    if (r != LEND_OK) { printf("%s\n", lend_result_message(r)); return; }
    save_pending(app);
    printf("PENDING: %s borrowed by %s - queued (not yet on the chain; %zu record(s) waiting to be mined).\n",
           tok[1], tok[2], app->pool->length);
}

static void cmd_return(MiningContext *app, char *tok[], int n) {
    if (n != 2) { printf("Usage: return <book_id>\n"); return; }
    if (refuse_if_compromised(app)) return;
    const Block *q = NULL;
    LendResult r = lending_return(app->chain, app->pool, app->reg, tok[1], &q);
    if (r != LEND_OK) { printf("%s\n", lend_result_message(r)); return; }
    save_pending(app);
    printf("PENDING: %s returned by %s - %s return, reward transaction of %d coins created\n",
           q->book_id, q->member_id, q->token_reward == REWARD_ON_TIME ? "ON-TIME" : "LATE",
           q->token_reward);
    printf("         tx_id=%s\n", q->tx_id);
    printf("         The member is credited only after this record is mined.\n");
}

static void cmd_check_overdue(MiningContext *app) {
    if (refuse_if_compromised(app)) return;
    int n = lending_check_overdue(app->chain, app->pool, app->reg);
    if (n == 0) {
        printf("No books are newly overdue (loan period %d days).\n", LOAN_PERIOD_DAYS);
        return;
    }
    save_pending(app);
    printf("PENDING: %d OVERDUE record(s) queued. Overdue books earn no reward: "
           "token_reward = 0 and no transaction is created.\n", n);
}

static void cmd_advance(char *tok[], int n) {
    long days;
    if (n != 2 || !parse_long_arg(tok[1], &days) || days < 1 || days > 365) {
        printf("Usage: advance <days>   (1-365)\n");
        return;
    }
    sim_advance_days((int)days);
    printf("Simulated clock moved forward %ld day(s); now %ld day(s) ahead of real time.\n",
           days, sim_offset_days());
}

static void cmd_mine(MiningContext *app, char *tok[], int n) {
    if (n < 2) { printf("Usage: mine solo | mine pool [miners] | mine cloud <rounds>\n"); return; }
    if (refuse_if_compromised(app)) return;

    if (strcasecmp(tok[1], "solo") == 0 && n == 2) {
        mine_solo(app);
    } else if (strcasecmp(tok[1], "pool") == 0 && n <= 3) {
        long miners = POOL_DEFAULT_MINERS;
        if (n == 3 && !parse_long_arg(tok[2], &miners)) {
            printf("Usage: mine pool [%d-%d]\n", POOL_MIN_MINERS, POOL_MAX_MINERS);
            return;
        }
        mine_pool(app, (int)miners);
    } else if (strcasecmp(tok[1], "cloud") == 0 && n == 3) {
        long rounds;
        if (!parse_long_arg(tok[2], &rounds)) {
            printf("Usage: mine cloud <%d-%d>\n", CLOUD_MIN_ROUNDS, CLOUD_MAX_ROUNDS);
            return;
        }
        mine_cloud(app, (int)rounds);
    } else {
        printf("Usage: mine solo | mine pool [miners] | mine cloud <rounds>\n");
    }
}

static void cmd_difficulty(MiningContext *app, char *tok[], int n) {
    if (n == 1) {
        printf("Current difficulty: %d (hash must start with %d zero%s). Range %d-%d.\n",
               app->difficulty, app->difficulty, app->difficulty == 1 ? "" : "s",
               MIN_DIFFICULTY, MAX_DIFFICULTY);
        return;
    }
    long d;
    if (n != 2 || !parse_long_arg(tok[1], &d) || d < MIN_DIFFICULTY || d > MAX_DIFFICULTY) {
        printf("ERROR: difficulty must be a whole number from %d to %d.\n", MIN_DIFFICULTY, MAX_DIFFICULTY);
        return;
    }
    app->difficulty = (int)d;
    printf("Difficulty set to %d: blocks mined from now on need %d leading zero%s "
           "(~%lu attempts expected).\n", app->difficulty, app->difficulty,
           app->difficulty == 1 ? "" : "s", 1UL << (4 * app->difficulty));
}

static void cmd_transfer(MiningContext *app, char *tok[], int n) {
    if (refuse_if_compromised(app)) return;
    long amount;

    if (app->ledger->model == MODEL_ACCOUNT) {
        long nonce;
        if (n != 5 || !parse_long_arg(tok[3], &amount) || !parse_long_arg(tok[4], &nonce)) {
            printf("Usage: transfer <from> <to> <amount> <nonce>\n");
            return;
        }
        ledger_transfer_account(app->ledger, app->chain->length, tok[1], tok[2], amount, nonce);
        return;
    }

    if (n < 4 || !parse_long_arg(tok[3], &amount)) {
        printf("Usage: transfer <from> <to> <amount> [#id ...]\n");
        return;
    }
    int ids[UTXO_MAX_INPUTS];
    size_t n_ids = 0;
    for (int i = 4; i < n; i++) {
        long id;
        const char *s = tok[i][0] == '#' ? tok[i] + 1 : tok[i];
        if (n_ids == UTXO_MAX_INPUTS || !parse_long_arg(s, &id) || id <= 0) {
            printf("ERROR: invalid input id '%s' (use the #id shown by 'utxos').\n", tok[i]);
            return;
        }
        ids[n_ids++] = (int)id;
    }
    if (ledger_transfer_utxo(app->ledger, app->chain->length, tok[1], tok[2], amount, ids, n_ids)) {
        utxo_print_set(&app->ledger->utxo, app->reg);
    }
}

static void cmd_view_records(const MiningContext *app) {
    printf("Confirmed lending chain (%zu blocks):\n", app->chain->length);
    for (const Block *b = app->chain->head; b; b = b->next) {
        print_confirmed_block(b, app->pub_key);
    }
    if (app->pool->length > 0) {
        printf("(%zu record(s) still pending - run 'pending' to see them, 'mine ...' to confirm them)\n",
               app->pool->length);
    }
}

static void cmd_validate(const MiningContext *app) {
    ChainValidation v = blockchain_validate(app->chain, app->pub_key);
    printf("Chain status: %zu block(s), %s\n", app->chain->length, v.valid ? "VALID" : "COMPROMISED");
    if (v.valid) {
        printf("  every hash recomputes, every link matches, every block meets its proof-of-work\n"
               "  target, every tx_id matches its reward, and every signature verifies.\n");
    } else {
        printf("  first problem at block index %d: %s\n", v.bad_index, v.reason);
    }
}

int cli_run(MiningContext *app) {
    char line[INPUT_BUF_LEN];
    printf("Library Lending Chain (Formative 2) - %s model, difficulty %d. Type 'help' for commands.\n",
           ledger_model_name(app->ledger->model), app->difficulty);

    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        trim(line);
        if (line[0] == '\0') continue;
        printf("\n");

        char *tok[MAX_TOKENS];
        int n = tokenize(line, tok, MAX_TOKENS);
        const char *cmd = tok[0];
        const char *arg = n > 1 ? tok[1] : "";

        if (strcasecmp(cmd, "borrow") == 0) {
            cmd_borrow(app, tok, n);
        } else if (strcasecmp(cmd, "return") == 0) {
            cmd_return(app, tok, n);
        } else if (strcasecmp(cmd, "check") == 0 && strcasecmp(arg, "overdue") == 0) {
            cmd_check_overdue(app);
        } else if (strcasecmp(cmd, "advance") == 0) {
            cmd_advance(tok, n);
        } else if (strcasecmp(cmd, "pending") == 0) {
            print_pending_pool(app->pool);
        } else if (strcasecmp(cmd, "mine") == 0) {
            cmd_mine(app, tok, n);
        } else if (strcasecmp(cmd, "difficulty") == 0) {
            cmd_difficulty(app, tok, n);
        } else if (strcasecmp(cmd, "balances") == 0) {
            ledger_print(app->ledger);
        } else if (strcasecmp(cmd, "utxos") == 0) {
            if (app->ledger->model == MODEL_UTXO) utxo_print_set(&app->ledger->utxo, app->reg);
            else printf("'utxos' is only available in the UTXO model (this session uses the account model).\n");
        } else if (strcasecmp(cmd, "transfer") == 0) {
            cmd_transfer(app, tok, n);
        } else if (strcasecmp(cmd, "history") == 0) {
            if (app->ledger->model != MODEL_ACCOUNT) {
                printf("'history' is only available in the account model (this session uses UTXO;"
                       " use 'utxos').\n");
            } else if (n != 2) {
                printf("Usage: history <member_id>\n");
            } else if (!account_print_history(&app->ledger->accounts, tok[1])) {
                printf("ERROR: Book or Member not found\n");
            }
        } else if (strcasecmp(cmd, "view") == 0) {
            cmd_view_records(app);
        } else if (strcasecmp(cmd, "validate") == 0 ||
                   (strcasecmp(cmd, "chain") == 0 && strcasecmp(arg, "status") == 0)) {
            cmd_validate(app);
        } else if (strcasecmp(cmd, "list") == 0 && strcasecmp(arg, "books") == 0) {
            printf("Books (%zu):\n", app->reg->book_count);
            for (size_t i = 0; i < app->reg->book_count; i++) {
                const Book *b = &app->reg->books[i];
                printf("  %-8s %-34s %s%s\n", b->book_id, b->title, b->author,
                       lending_book_on_loan(app->chain, app->pool, b->book_id) ? "  [on loan]" : "");
            }
        } else if (strcasecmp(cmd, "list") == 0 && strcasecmp(arg, "members") == 0) {
            printf("Members (%zu):\n", app->reg->member_count);
            for (size_t i = 0; i < app->reg->member_count; i++) {
                const Member *m = &app->reg->members[i];
                printf("  %-8s %-24s %s\n", m->member_id, m->full_name, m->course_code);
            }
        } else if (strcasecmp(cmd, "help") == 0) {
            print_help(app);
        } else if (strcasecmp(cmd, "exit") == 0 || strcasecmp(cmd, "quit") == 0) {
            break;
        } else {
            printf("Unrecognized command: '%s'. Type 'help' for the command list.\n", cmd);
        }
    }
    return 0;
}
