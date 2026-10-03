#ifndef CLI_H
#define CLI_H

#include "blockchain.h"
#include "registry.h"

/* Runs the interactive command loop until the user exits. `priv_key` signs
 * new blocks; `pub_key` verifies signatures for "view records" and
 * "validate chain". The chain is re-saved to `chain_path` after every
 * state-changing command (borrow/return), so persistence stays current even
 * if the program is killed rather than exited cleanly. Returns 0. */
int cli_run(Blockchain *chain, const Registry *reg,
            EVP_PKEY *priv_key, EVP_PKEY *pub_key, const char *chain_path);

#endif
