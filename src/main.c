#include <stdio.h>

#include "registry.h"
#include "blockchain.h"
#include "persistence.h"
#include "cli.h"

#define BOOKS_PATH   "data/books.txt"
#define MEMBERS_PATH "data/members.txt"
#define CHAIN_PATH   "data/chain.txt"
#define PRIV_KEY_PATH "keys/private.pem"
#define PUB_KEY_PATH  "keys/public.pem"

int main(void) {
    Registry reg;
    if (!registry_load(&reg, BOOKS_PATH, MEMBERS_PATH)) {
        fprintf(stderr, "FATAL: could not load registries. Exiting.\n");
        return 1;
    }

    EVP_PKEY *priv_key = crypto_load_or_create_keypair(PRIV_KEY_PATH, PUB_KEY_PATH);
    if (!priv_key) {
        fprintf(stderr, "FATAL: could not load or create the ECDSA keypair. Exiting.\n");
        return 1;
    }
    EVP_PKEY *pub_key = crypto_load_public_key(PUB_KEY_PATH);
    if (!pub_key) {
        fprintf(stderr, "FATAL: could not load the ECDSA public key. Exiting.\n");
        EVP_PKEY_free(priv_key);
        return 1;
    }

    Blockchain chain;
    blockchain_init(&chain);

    PersistResult pr = persistence_load(&chain, CHAIN_PATH);
    if (pr == PERSIST_NOT_FOUND) {
        printf("No existing chain found at '%s' — creating genesis block.\n", CHAIN_PATH);
        if (!blockchain_create_genesis(&chain, priv_key) || !persistence_save(&chain, CHAIN_PATH)) {
            fprintf(stderr, "FATAL: could not create or persist the genesis block. Exiting.\n");
            blockchain_free(&chain);
            EVP_PKEY_free(priv_key);
            EVP_PKEY_free(pub_key);
            return 1;
        }
    } else if (pr == PERSIST_CORRUPT) {
        /* The chain file exists but doesn't even parse — this is more severe
         * than a cryptographic tamper (which a well-formed but altered file
         * would still parse and then fail blockchain_validate below), so it
         * is treated as fatal rather than something the CLI can inspect. */
        fprintf(stderr, "FATAL: chain file '%s' is unreadable/corrupt at the parsing level. "
                         "Restore it from backup or delete it to start a new chain.\n", CHAIN_PATH);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        return 1;
    }

    /* Never trust persisted data without verifying it first (R9.2/R9.3). A
     * validation failure here is NOT fatal — the program still starts so the
     * required tamper-detection demo can show 'validate chain' reporting the
     * compromise live, and cli.c refuses to append new blocks on top of a
     * chain in this state. */
    ChainValidation v = blockchain_validate(&chain, pub_key);
    if (v.valid) {
        printf("Loaded chain: %zu block(s), integrity verified.\n", chain.length);
    } else {
        fprintf(stderr, "WARNING: chain integrity check FAILED at block %d: %s\n", v.bad_index, v.reason);
        fprintf(stderr, "The chain file may have been tampered with. Borrow/return are disabled until this is resolved;\n");
        fprintf(stderr, "use 'view records' and 'validate chain' to inspect the damage.\n");
    }

    int rc = cli_run(&chain, &reg, priv_key, pub_key, CHAIN_PATH);

    blockchain_free(&chain);
    EVP_PKEY_free(priv_key);
    EVP_PKEY_free(pub_key);
    return rc;
}
