#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config.h"
#include "registry.h"
#include "blockchain.h"
#include "persistence.h"
#include "ledger.h"
#include "mining.h"
#include "cli.h"

#define BOOKS_PATH    "data/books.txt"
#define MEMBERS_PATH  "data/members.txt"
#define CHAIN_PATH    "data/chain.txt"
#define PENDING_PATH  "data/pending.txt"
#define PRIV_KEY_PATH "keys/private.pem"
#define PUB_KEY_PATH  "keys/public.pem"
#define UTXO_LOG_PATH    "data/transfers_utxo.txt"
#define ACCOUNT_LOG_PATH "data/transfers_account.txt"

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--model utxo|account] [--difficulty 1-4] [--seed N]\n"
            "  --model       transaction model for the whole session (asked at startup if omitted)\n"
            "  --difficulty  proof-of-work difficulty, leading zero hex characters (default %d)\n"
            "  --seed        random seed for pool-mining hash rates (for repeatable demos)\n",
            prog, DEFAULT_DIFFICULTY);
}

static int parse_int(const char *s, int lo, int hi, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

static int parse_model(const char *s, LedgerModel *out) {
    if (strcasecmp(s, "utxo") == 0 || strcmp(s, "1") == 0) { *out = MODEL_UTXO; return 1; }
    if (strcasecmp(s, "account") == 0 || strcmp(s, "2") == 0) { *out = MODEL_ACCOUNT; return 1; }
    return 0;
}

/* Interactive model choice when --model was not given. */
static LedgerModel ask_model(void) {
    char line[64];
    for (;;) {
        printf("Choose the transaction model for this session:\n"
               "  1) UTXO model\n"
               "  2) Account-based model\n"
               "Choice [1/2]: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\nNo choice entered - using the UTXO model.\n");
            return MODEL_UTXO;
        }
        line[strcspn(line, "\r\n")] = '\0';
        LedgerModel m;
        if (parse_model(line, &m)) return m;
        printf("Please enter 1 or 2.\n");
    }
}

/* Pending records are not signed until they are mined, so anything read back
 * from pending.txt is re-checked against the registry and the reward rules
 * before it is trusted. A record that fails means the file was edited. */
static int pending_record_ok(const Block *b, const Registry *reg) {
    if (!registry_find_book(reg, b->book_id) || !registry_find_member(reg, b->member_id)) return 0;
    if (strcmp(b->action, "RETURNED") == 0) {
        if (b->token_reward != REWARD_ON_TIME && b->token_reward != REWARD_LATE) return 0;
    } else if (strcmp(b->action, "BORROWED") == 0 || strcmp(b->action, "OVERDUE") == 0) {
        if (b->token_reward != REWARD_NONE) return 0;
    } else {
        return 0;
    }
    char expected[HASH_HEX_LEN];
    block_compute_tx_id(b, expected);
    return strcmp(expected, b->tx_id) == 0;
}

int main(int argc, char **argv) {
    int difficulty = DEFAULT_DIFFICULTY;
    int model_given = 0;
    LedgerModel model = MODEL_UTXO;
    unsigned int seed = (unsigned int)time(NULL);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            if (!parse_model(argv[++i], &model)) { usage(argv[0]); return 1; }
            model_given = 1;
        } else if (strcmp(argv[i], "--difficulty") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], MIN_DIFFICULTY, MAX_DIFFICULTY, &difficulty)) {
                fprintf(stderr, "ERROR: difficulty must be %d-%d.\n", MIN_DIFFICULTY, MAX_DIFFICULTY);
                return 1;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            int s;
            if (!parse_int(argv[++i], 0, 2000000000, &s)) { usage(argv[0]); return 1; }
            seed = (unsigned int)s;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    srand(seed);

    Registry reg;
    if (!registry_load(&reg, BOOKS_PATH, MEMBERS_PATH)) {
        fprintf(stderr, "FATAL: could not load registries. Exiting.\n");
        return 1;
    }
    printf("Registries loaded: %zu book(s), %zu member(s).\n", reg.book_count, reg.member_count);

    EVP_PKEY *priv_key = crypto_load_or_create_keypair(PRIV_KEY_PATH, PUB_KEY_PATH);
    EVP_PKEY *pub_key = priv_key ? crypto_load_public_key(PUB_KEY_PATH) : NULL;
    if (!priv_key || !pub_key) {
        fprintf(stderr, "FATAL: could not load or create the ECDSA keypair. Exiting.\n");
        EVP_PKEY_free(priv_key);
        return 1;
    }

    if (!model_given) model = ask_model();

    Blockchain chain;
    PendingPool pool;
    blocklist_init(&chain);
    blocklist_init(&pool);
    int rc = 1;

    PersistResult pr = persistence_load_chain(&chain, CHAIN_PATH);
    if (pr == PERSIST_NOT_FOUND) {
        printf("No existing chain at '%s' - mining the genesis block.\n", CHAIN_PATH);
        /* Transfer logs are tied to the chain they were made against; a
         * brand-new chain makes any old ones meaningless. */
        int removed = (remove(UTXO_LOG_PATH) == 0);
        removed |= (remove(ACCOUNT_LOG_PATH) == 0);
        removed |= (remove(PENDING_PATH) == 0);
        if (removed) printf("Removed pending/transfer files left over from a previous chain.\n");
        if (!blockchain_create_genesis(&chain, priv_key, difficulty) ||
            !persistence_save_chain(&chain, CHAIN_PATH)) {
            fprintf(stderr, "FATAL: could not create or save the genesis block.\n");
            goto cleanup;
        }
    } else if (pr == PERSIST_CORRUPT) {
        fprintf(stderr, "FATAL: chain file '%s' is unreadable. Restore it or delete it to start over.\n",
                CHAIN_PATH);
        goto cleanup;
    }

    /* Persisted data is never trusted without verification. A failure here
     * is not fatal, so 'validate chain' can demonstrate tamper detection
     * live, but every command that would change state is refused. */
    ChainValidation v = blockchain_validate(&chain, pub_key);
    if (v.valid) {
        printf("Loaded chain: %zu block(s), integrity verified.\n", chain.length);
    } else {
        fprintf(stderr, "WARNING: chain integrity check FAILED at block %d: %s\n", v.bad_index, v.reason);
        fprintf(stderr, "Lending, mining and transfers are disabled; use 'view records' and "
                        "'validate chain' to inspect the damage.\n");
    }

    if (persistence_load_pending(&pool, PENDING_PATH) == PERSIST_CORRUPT) {
        fprintf(stderr, "FATAL: pending pool file '%s' is unreadable.\n", PENDING_PATH);
        goto cleanup;
    }
    for (const Block *b = pool.head; b; b = b->next) {
        if (!pending_record_ok(b, &reg)) {
            fprintf(stderr, "FATAL: pending pool file '%s' contains a record that fails validation "
                            "(%s %s %s reward %d) - it was modified outside the program.\n",
                    PENDING_PATH, b->action, b->book_id, b->member_id, b->token_reward);
            goto cleanup;
        }
    }
    if (pool.length > 0) printf("Pending pool: %zu unconfirmed record(s) restored.\n", pool.length);

    Ledger ledger;
    ledger_init(&ledger, model, &reg, model == MODEL_UTXO ? UTXO_LOG_PATH : ACCOUNT_LOG_PATH,
                priv_key, pub_key);
    if (v.valid && !ledger_replay(&ledger, &chain)) {
        fprintf(stderr, "FATAL: the transfer log could not be replayed - it was modified outside "
                        "the program or belongs to a different chain.\n");
        ledger_free(&ledger);
        goto cleanup;
    }
    printf("Transaction model: %s. Balances rebuilt from confirmed rewards and the signed "
           "transfer log.\n", ledger_model_name(model));

    MiningContext app = {
        .chain = &chain, .pool = &pool, .ledger = &ledger, .reg = &reg,
        .priv_key = priv_key, .pub_key = pub_key, .difficulty = difficulty,
        .chain_path = CHAIN_PATH, .pending_path = PENDING_PATH,
    };
    rc = cli_run(&app);
    ledger_free(&ledger);

cleanup:
    blocklist_free(&chain);
    blocklist_free(&pool);
    EVP_PKEY_free(priv_key);
    EVP_PKEY_free(pub_key);
    return rc;
}
