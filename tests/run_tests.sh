#!/usr/bin/env bash
# Phase 8 systematic test matrix for the library lending chain.
# Every test runs the real compiled binary in an isolated sandbox directory
# (own data/, keys/, chain file) so tests never touch the project's real
# data/ or keys/ and never depend on run order.

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/lending_tracker"

PASS=0
FAIL=0
FAILED_NAMES=()

# ---- assertion helpers -----------------------------------------------------

assert_contains() { # haystack needle name
    if [[ "$1" == *"$2"* ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED_NAMES+=("$3")
        echo "FAIL: $3"
        echo "  expected output to contain: $2"
        echo "  --- actual output ---"
        echo "$1" | sed 's/^/  /'
    fi
}

assert_not_contains() { # haystack needle name
    if [[ "$1" != *"$2"* ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED_NAMES+=("$3")
        echo "FAIL: $3"
        echo "  expected output NOT to contain: $2"
        echo "  --- actual output ---"
        echo "$1" | sed 's/^/  /'
    fi
}

assert_eq() { # actual expected name
    if [[ "$1" == "$2" ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED_NAMES+=("$3")
        echo "FAIL: $3 (expected '$2', got '$1')"
    fi
}

# ---- sandbox helpers --------------------------------------------------------

make_sandbox() {
    local dir
    dir=$(mktemp -d)
    mkdir -p "$dir/data" "$dir/keys"
    printf 'BK001,Things Fall Apart,Chinua Achebe\nBK002,Americanah,Chimamanda Ngozi Adichie\n' > "$dir/data/books.txt"
    printf 'ALU001,John Doe,BLK101\nALU002,Jane Smith,BLK101\n' > "$dir/data/members.txt"
    echo "$dir"
}

run_cli() { # sandbox stdin_commands
    ( cd "$1" && printf '%s' "$2" | "$BIN" ) 2>&1
}

run_cli_status() { # sandbox stdin_commands -> prints exit code
    ( cd "$1" && printf '%s' "$2" | "$BIN" >/dev/null 2>&1 ); echo $?
}

seed_chain() { # sandbox — leaves a 3-block chain: genesis, BORROWED, RETURNED
    run_cli "$1" $'borrow BK001 ALU001\nreturn BK001\nexit\n' >/dev/null
}

tamper_field() { # file line_no sed_script
    sed -i "$2" "$1"
}

echo "== Building release binary =="
make -C "$ROOT_DIR" >/dev/null || { echo "build failed"; exit 1; }

# =============================================================================
echo "== REGISTRY =="
# =============================================================================

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'exit\n')
assert_contains "$OUT" "Loaded chain: 1 block(s), integrity verified." "registry: valid books.txt + members.txt loads and boots"

SB=$(make_sandbox); rm "$SB/data/books.txt"
OUT=$(run_cli "$SB" $'exit\n'); CODE=$(run_cli_status "$SB" $'exit\n')
assert_contains "$OUT" "not found or could not be opened" "registry: missing books.txt reported"
assert_eq "$CODE" "1" "registry: missing books.txt exits non-zero"

SB=$(make_sandbox); rm "$SB/data/members.txt"
OUT=$(run_cli "$SB" $'exit\n')
assert_contains "$OUT" "Member registry file" "registry: missing members.txt reported"

SB=$(make_sandbox); > "$SB/data/books.txt"
OUT=$(run_cli "$SB" $'exit\n')
assert_contains "$OUT" "Book registry" "registry: empty books.txt reported"
assert_contains "$OUT" "empty or contains no valid records" "registry: empty books.txt reason is specific"

SB=$(make_sandbox); > "$SB/data/members.txt"
OUT=$(run_cli "$SB" $'exit\n')
assert_contains "$OUT" "Member registry" "registry: empty members.txt reported"

SB=$(make_sandbox); printf 'BK003,Missing Author Field\n' >> "$SB/data/books.txt"
OUT=$(run_cli "$SB" $'list books\nexit\n')
assert_contains "$OUT" "malformed line" "registry: malformed line (wrong field count) warned, not crashed"
# registry entries print as "  <id>   <title>..." (two leading spaces); the
# warning text that echoes the bad line uses "): BK003", not that prefix, so
# this distinguishes "was loaded as a registry entry" from "was only quoted
# in the warning".
assert_not_contains "$OUT" "  BK003   " "registry: malformed line's book not loaded into the registry"
assert_contains "$OUT" "  BK001   " "registry: other valid books still loaded alongside a malformed line"

SB=$(make_sandbox); printf 'BK004,%s,Some Author\n' "$(printf 'x%.0s' $(seq 1 90))" >> "$SB/data/books.txt"
OUT=$(run_cli "$SB" $'exit\n')
assert_contains "$OUT" "field too long" "registry: oversized field rejected without crashing"

# =============================================================================
echo "== BORROW =="
# =============================================================================

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BK001 ALU001\nexit\n')
assert_contains "$OUT" "OK: BK001 borrowed by ALU001" "borrow: valid book + valid member succeeds"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BADID ALU001\nexit\n')
assert_contains "$OUT" "ERROR: Book or Member not found" "borrow: invalid book id rejected"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BK001 BADMEM\nexit\n')
assert_contains "$OUT" "ERROR: Book or Member not found" "borrow: invalid member id rejected"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BADID BADMEM\nexit\n')
assert_contains "$OUT" "ERROR: Book or Member not found" "borrow: both invalid rejected"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BK001 ALU001\nborrow BK001 ALU002\nexit\n')
assert_contains "$OUT" "ERROR: this book is already on loan" "borrow: already-borrowed book rejected"

# =============================================================================
echo "== RETURN =="
# =============================================================================

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BK001 ALU001\nreturn BK001\nexit\n')
assert_contains "$OUT" "OK: BK001 returned" "return: valid return succeeds"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'return BK001\nexit\n')
assert_contains "$OUT" "never borrowed, or has already been returned" "return: never-borrowed book rejected"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BK001 ALU001\nreturn BK001\nreturn BK001\nexit\n')
assert_contains "$OUT" "never borrowed, or has already been returned" "return: already-returned book rejected"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'return BADID\nexit\n')
assert_contains "$OUT" "ERROR: Book or Member not found" "return: invalid book id rejected"

# =============================================================================
echo "== BLOCKCHAIN =="
# =============================================================================

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'exit\n')
assert_contains "$OUT" "creating genesis block" "blockchain: genesis created on first run"

OUT=$(run_cli "$SB" $'chain status\nexit\n')
assert_contains "$OUT" "1 block(s), VALID" "blockchain: genesis-only chain validates"
assert_not_contains "$OUT" "creating genesis block" "blockchain: genesis NOT recreated on reload"

SB=$(make_sandbox)
OUT=$(run_cli "$SB" $'borrow BK001 ALU001\nborrow BK002 ALU002\nreturn BK001\nchain status\nexit\n')
assert_contains "$OUT" "4 block(s), VALID" "blockchain: multi-block chain (genesis + 3 actions) validates"

SB=$(make_sandbox)
run_cli "$SB" $'borrow BK001 ALU001\nexit\n' >/dev/null
OUT=$(run_cli "$SB" $'view records\nexit\n')
assert_contains "$OUT" "Loaded chain: 2 block(s), integrity verified." "blockchain: state persists and reloads across separate process runs"
assert_contains "$OUT" "BORROWED book=BK001 (Things Fall Apart) member=ALU001 (John Doe)" \
    "blockchain: reloaded record has canonical title/name copied from the registry, not just the raw IDs"

# genesis correctness: corrupt the genesis block's previous_hash (must be 64
# zeros) and confirm it is caught specifically, at block 0.
SB=$(make_sandbox)
run_cli "$SB" $'exit\n' >/dev/null
sed -i '1s/|0000000000000000000000000000000000000000000000000000000000000000|/|1111111111111111111111111111111111111111111111111111111111111111|/' "$SB/data/chain.txt"
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "block 0" "blockchain: corrupted genesis previous_hash reported at block 0"
assert_contains "$OUT" "COMPROMISED" "blockchain: corrupted genesis previous_hash fails validation"

# index/order consistency: duplicate an index so two blocks both claim "1".
SB=$(make_sandbox)
run_cli "$SB" $'borrow BK001 ALU001\nborrow BK002 ALU002\nexit\n' >/dev/null
sed -i '3s/^2|/1|/' "$SB/data/chain.txt"
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "block index out of sequence" "blockchain: duplicated/out-of-sequence index detected"

# =============================================================================
echo "== CRYPTOGRAPHY =="
# =============================================================================

SB=$(make_sandbox)
seed_chain "$SB"
OUT=$(run_cli "$SB" $'view records\nexit\n')
assert_not_contains "$OUT" "signature=INVALID" "crypto: every block's signature verifies as VALID on an untouched chain"

# invalid signature / modified signed data: flip one hex digit inside the
# signature field (field 9 of 10) without touching any hashed field, so the
# stored hash still matches recomputation but the signature no longer does.
SB=$(make_sandbox)
seed_chain "$SB"
LINE2=$(sed -n '2p' "$SB/data/chain.txt")
SIG=$(echo "$LINE2" | cut -d'|' -f9)
FIRST_CHAR="${SIG:0:1}"
BAD_CHAR="1"; [[ "$FIRST_CHAR" == "1" ]] && BAD_CHAR="2"
BAD_SIG="${BAD_CHAR}${SIG:1}"
sed -i "2s#|$SIG|#|$BAD_SIG|#" "$SB/data/chain.txt"
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "COMPROMISED" "crypto: corrupted signature (hash untouched) still fails validation"
assert_contains "$OUT" "ECDSA signature is invalid" "crypto: corrupted signature is reported as a signature failure, not a hash failure"

# wrong public key: swap in an unrelated keypair's public key and confirm a
# perfectly intact chain now fails signature verification.
SB=$(make_sandbox)
seed_chain "$SB"
openssl ecparam -name prime256v1 -genkey -noout -out "$SB/keys/other_priv.pem" 2>/dev/null
openssl ec -in "$SB/keys/other_priv.pem" -pubout -out "$SB/keys/public.pem" 2>/dev/null
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "COMPROMISED" "crypto: wrong public key fails validation on an otherwise-intact chain"
assert_contains "$OUT" "ECDSA signature is invalid" "crypto: wrong public key reported specifically as a signature failure"

# =============================================================================
echo "== TAMPERING =="
# =============================================================================

# modify block data (a hashed field)
SB=$(make_sandbox)
seed_chain "$SB"
sed -i '2s/Things Fall Apart/Tampered Title Here/' "$SB/data/chain.txt"
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "block 1" "tamper(block data): reported at the correct block index"
assert_contains "$OUT" "stored hash does not match recomputed hash" "tamper(block data): detected as a hash mismatch"

# modify the stored hash field directly
SB=$(make_sandbox)
seed_chain "$SB"
OLDHASH=$(sed -n '2p' "$SB/data/chain.txt" | cut -d'|' -f10)
sed -i "2s#|$OLDHASH\$#|0000000000000000000000000000000000000000000000000000000000000099#" "$SB/data/chain.txt"
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "COMPROMISED" "tamper(stored hash): detected"

# modify previous_hash of a middle block, breaking the link to its predecessor
SB=$(make_sandbox)
run_cli "$SB" $'borrow BK001 ALU001\nborrow BK002 ALU002\nexit\n' >/dev/null
PREVHASH=$(sed -n '3p' "$SB/data/chain.txt" | cut -d'|' -f8)
sed -i "3s#|$PREVHASH|#|1111111111111111111111111111111111111111111111111111111111111111|#" "$SB/data/chain.txt"
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "block 2" "tamper(previous_hash): reported at the correct block index"
assert_contains "$OUT" "previous_hash does not match" "tamper(previous_hash): detected as a linkage failure"

# single external byte modification -> reload -> validate fails (the exact
# assignment-required demonstration)
SB=$(make_sandbox)
seed_chain "$SB"
python3 - "$SB/data/chain.txt" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read())
    # flip one alphabetic byte inside line 2 (the BORROWED block)
    lines = data.split(b"\n")
    target = bytearray(lines[1])
    for i, c in enumerate(target):
        if 97 <= c <= 122:  # a lowercase ascii letter
            target[i] = target[i] ^ 0x20  # flip case -> single-byte change
            break
    lines[1] = bytes(target)
    f.seek(0)
    f.write(b"\n".join(lines))
    f.truncate()
PYEOF
OUT=$(run_cli "$SB" $'validate chain\nexit\n')
assert_contains "$OUT" "COMPROMISED" "tamper(single external byte): reload + validate reports failure"

# refuse to write on top of a compromised chain
SB=$(make_sandbox)
seed_chain "$SB"
sed -i '2s/Things Fall Apart/Tampered Title Here/' "$SB/data/chain.txt"
OUT=$(run_cli "$SB" $'borrow BK002 ALU002\nexit\n')
assert_contains "$OUT" "refusing to add new records" "tamper: borrow is refused while the chain is compromised"

# =============================================================================
echo "== MEMORY SAFETY (ASan/UBSan) =="
# =============================================================================

make -C "$ROOT_DIR" asan >/dev/null 2>&1
SB=$(make_sandbox)
SESSION=$'borrow BK001 ALU001\nborrow BK002 ALU002\nreturn BK001\nreturn BK001\nborrow BADID BADID\nview records\nvalidate chain\nlist books\nlist members\nchain status\nhelp\nbadcommand\nexit\n'
OUT=$(run_cli "$SB" "$SESSION")
assert_not_contains "$OUT" "ERROR: AddressSanitizer" "memory: no AddressSanitizer errors across a full session"
assert_not_contains "$OUT" "runtime error:" "memory: no UndefinedBehaviorSanitizer errors across a full session"
assert_not_contains "$OUT" "LeakSanitizer" "memory: no leaks reported across a full session"
make -C "$ROOT_DIR" clean >/dev/null 2>&1
make -C "$ROOT_DIR" >/dev/null 2>&1

# =============================================================================
echo ""
echo "================================================================"
echo "  $PASS passed, $FAIL failed"
if [[ $FAIL -gt 0 ]]; then
    echo "  Failed: ${FAILED_NAMES[*]}"
fi
echo "================================================================"
[[ $FAIL -eq 0 ]]
