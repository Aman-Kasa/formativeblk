#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "blockchain.h"

typedef enum {
    PERSIST_LOADED,     /* file existed and parsed into a chain */
    PERSIST_NOT_FOUND,  /* no chain file yet — caller should create genesis */
    PERSIST_CORRUPT     /* file existed but could not be parsed safely */
} PersistResult;

/* Parses the on-disk chain format (see ARCHITECTURE.md "Persistence
 * strategy") into `chain`. Parsing only checks the file's *shape* (field
 * count, field lengths, numeric fields) — it does NOT verify hashes or
 * signatures. That is blockchain_validate()'s job, run separately by the
 * caller immediately after a successful load, so cryptographic tamper
 * detection stays in one place. `chain` must be freshly initialized before
 * this call. */
PersistResult persistence_load(Blockchain *chain, const char *path);

/* Overwrites `path` with the full contents of `chain`, one line per block.
 * Returns 1 on success, 0 on failure (e.g. file could not be opened for
 * writing). */
int persistence_save(const Blockchain *chain, const char *path);

#endif
