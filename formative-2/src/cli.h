#ifndef CLI_H
#define CLI_H

#include "mining.h"

/* Runs the interactive command loop until the user exits. The mining
 * context doubles as the application state: chain, pending pool, registry,
 * ledger, keys, current difficulty and file paths. Returns 0. */
int cli_run(MiningContext *app);

#endif
