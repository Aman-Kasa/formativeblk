#!/usr/bin/env bash
# Formative 2 test suite. Every test runs the real compiled binary inside an
# isolated sandbox directory (its own data/, keys/, chain and pending files),
# so tests never touch the project's real data and never depend on run order.
#
# Covers the edge cases the brief asks for: an overdue book generates no
# transaction, insufficient balance, failed/reused nonce, unprofitable cloud
# rental - plus the pending pool, proof of work, both transaction models,
# all three mining methods, tamper detection and memory safety.

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/lending_tracker"
FORGE="$ROOT_DIR/tests/forge_chain.py"

PASS=0
FAIL=0

# ---- assertions ------------------------------------------------------------

check() { # name, then a command that must succeed
    local name="$1"; shift
    if "$@"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $name"
    fi
}

has()    { [[ "$1" == *"$2"* ]]; }      # haystack needle
hasnt()  { [[ "$1" != *"$2"* ]]; }
matches() { grep -Eq -- "$2" <<< "$1"; } # haystack regex

# ---- sandbox helpers -------------------------------------------------------

make_sandbox() {
    local dir
    dir=$(mktemp -d)
    mkdir -p "$dir/data" "$dir/keys"
    printf 'BK001,Things Fall Apart,Chinua Achebe\nBK002,Americanah,Chimamanda Ngozi Adichie\nBK003,The River Between,Ngugi wa Thiongo\n' > "$dir/data/books.txt"
    printf 'ALU001,John Doe,BLK101\nALU002,Jane Smith,BLK101\nALU003,Amara Diallo,BLK101\n' > "$dir/data/members.txt"
    echo "$dir"
}

run() { # sandbox model commands [extra args...]
    local dir="$1" model="$2" cmds="$3"; shift 3
    ( cd "$dir" && printf '%b' "$cmds" | "$BIN" --model "$model" --seed 42 "$@" ) 2>&1
}

chain_lines()   { grep -c . "$1/data/chain.txt"; }
pending_lines() { [[ -f "$1/data/pending.txt" ]] && grep -c . "$1/data/pending.txt" || echo 0; }

# ---------------------------------------------------------------------------
echo "== Building =="
make -C "$ROOT_DIR" -s clean all >/dev/null || { echo "build failed"; exit 1; }

# ---------------------------------------------------------------------------
echo "== REGISTRY =="
S=$(make_sandbox); rm "$S/data/books.txt"
OUT=$(run "$S" utxo 'exit\n'); check "missing books.txt reported" has "$OUT" "not found"
S=$(make_sandbox); : > "$S/data/members.txt"
OUT=$(run "$S" utxo 'exit\n'); check "empty members.txt reported" has "$OUT" "empty"

# ---------------------------------------------------------------------------
echo "== PENDING POOL =="
S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK001 ALU001\nexit\n')
check "borrow is queued, not confirmed" has "$OUT" "PENDING: BK001 borrowed by ALU001"
check "chain still holds only genesis" [ "$(chain_lines "$S")" -eq 1 ]
check "pending record saved to disk" [ "$(pending_lines "$S")" -eq 1 ]
OUT=$(run "$S" utxo 'pending\nexit\n')
check "pending pool restored after restart" has "$OUT" "1 unconfirmed record(s)"
OUT=$(run "$S" utxo 'borrow BK001 ALU002\nexit\n')
check "double borrow rejected while first borrow is still pending" has "$OUT" "already on loan"
OUT=$(run "$S" utxo 'borrow BK999 ALU001\nborrow BK002 ALU999\nexit\n')
check "unknown book/member rejected" [ "$(grep -c 'Book or Member not found' <<< "$OUT")" -eq 2 ]
OUT=$(run "$S" utxo 'return BK002\nexit\n')
check "return of a book never borrowed rejected" has "$OUT" "never borrowed"

# ---------------------------------------------------------------------------
echo "== TOKEN REWARDS =="
S=$(make_sandbox)
OUT=$(run "$S" account 'borrow BK001 ALU001\nreturn BK001\nbalances\nexit\n')
check "on-time return creates a 10-coin reward transaction" has "$OUT" "ON-TIME return, reward transaction of 10 coins"
check "reward tx_id is a SHA-256 hash" matches "$OUT" "tx_id=[0-9a-f]{64}"
check "balance not credited before mining" matches "$OUT" "ALU001 +0 "
OUT=$(run "$S" account 'mine solo\nbalances\nexit\n')
check "balance credited after mining (10 - 1 fee = 9)" matches "$OUT" "ALU001 +9 "

S=$(make_sandbox)
run "$S" utxo 'borrow BK001 ALU001\nreturn BK001\nborrow BK001 ALU001\nreturn BK001\nexit\n' >/dev/null
check "every reward transaction gets a unique tx_id" [ "$(cut -d'|' -f8 "$S/data/pending.txt" | grep . | sort | uniq -d | wc -l)" -eq 0 ]

S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK002 ALU002\nadvance 15\nreturn BK002\nexit\n')
check "late return creates a 5-coin reward transaction" has "$OUT" "LATE return, reward transaction of 5 coins"

S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK003 ALU003\nadvance 20\ncheck overdue\ncheck overdue\npending\nmine solo\nbalances\nexit\n')
check "overdue book queued as OVERDUE" has "$OUT" "1 OVERDUE record(s) queued"
check "overdue book not flagged twice" has "$OUT" "No books are newly overdue"
check "overdue record has reward 0 and no transaction" matches "$OUT" "OVERDUE +BK003 .* 0 +none"
check "overdue earns nothing after mining" has "$OUT" "ALU003=0"
OUT=$(run "$S" utxo 'borrow BK003 ALU001\nexit\n')
check "overdue book still counts as on loan" has "$OUT" "already on loan"

# ---------------------------------------------------------------------------
echo "== PROOF OF WORK =="
S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK001 ALU001\nmine solo\nexit\n' --difficulty 3)
check "solo mining reports hash attempts" matches "$OUT" "valid hash found after [0-9]+ attempts"
check "every block hash meets difficulty 3" [ "$(cut -d'|' -f14 "$S/data/chain.txt" | grep -vc '^000')" -eq 0 ]
check "difficulty stored in each block" [ "$(cut -d'|' -f9 "$S/data/chain.txt" | sort -u)" = "3" ]
OUT=$(run "$S" utxo 'difficulty 0\ndifficulty 5\ndifficulty abc\ndifficulty 4\nexit\n')
check "difficulty outside 1-4 rejected" [ "$(grep -c 'difficulty must be' <<< "$OUT")" -eq 3 ]
check "difficulty 4 accepted" has "$OUT" "Difficulty set to 4"
OUT=$( (cd "$S" && echo exit | "$BIN" --model utxo --difficulty 9) 2>&1 )
check "--difficulty 9 rejected" has "$OUT" "difficulty must be 1-4"
OUT=$(run "$S" utxo 'mine solo\nexit\n')
check "mining an empty pool is refused" has "$OUT" "nothing to mine"

# ---------------------------------------------------------------------------
echo "== UTXO MODEL =="
S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK001 ALU001\nreturn BK001\nborrow BK002 ALU001\nreturn BK002\nmine solo\nexit\n')
check "UTXO set printed after each confirmed block" [ "$(grep -c 'UTXO set (unspent outputs)' <<< "$OUT")" -eq 4 ]
check "rewards become UTXOs owned by the member" matches "$OUT" "#2 +ALU001 +9 "
OUT=$(run "$S" utxo 'transfer ALU001 ALU002 5 #1\nexit\n')
check "transfer returns change (9 - 5 - 1 fee = 3)" has "$OUT" "3 change back to ALU001, fee 1"
check "recipient receives a new UTXO" matches "$OUT" "#3 +ALU002 +5 "
OUT=$(run "$S" utxo 'transfer ALU001 ALU002 5 #1\nexit\n')
check "double spend rejected after a restart (transfer log replayed)" has "$OUT" "double spend rejected - input #1 is already spent"
OUT=$(run "$S" utxo 'balances\nexit\n')
check "balances identical after restart" has "$OUT" "ALU001=12, ALU002=5"
OUT=$(run "$S" utxo 'transfer ALU001 ALU002 1 #2 #2\ntransfer ALU002 ALU003 1 #2\ntransfer ALU001 ALU002 100\ntransfer ALU001 ALU002 9 #2\nexit\n')
check "same input listed twice rejected" has "$OUT" "listed twice"
check "spending someone else's UTXO rejected" has "$OUT" "belongs to ALU001, not ALU002"
check "insufficient inputs rejected" has "$OUT" "insufficient funds"
check "inputs must cover amount + fee" has "$OUT" "inputs total 9, but amount 9 + fee 1 = 10"

# ---------------------------------------------------------------------------
echo "== ACCOUNT MODEL =="
S=$(make_sandbox)
run "$S" account 'borrow BK001 ALU001\nreturn BK001\nmine solo\nexit\n' >/dev/null
OUT=$(run "$S" account 'transfer ALU001 ALU002 3 0\ntransfer ALU001 ALU002 1 0\ntransfer ALU001 ALU002 1 7\ntransfer ALU001 ALU002 50 1\ntransfer ALU001 ALU002 2 1\nhistory ALU001\nhistory ALU002\nbalances\nexit\n')
check "valid transfer accepted" has "$OUT" "ALU001 -> ALU002, amount 3, fee 1, nonce 0"
check "reused nonce rejected" has "$OUT" "nonce already used: replay rejected"
check "skipped nonce rejected" has "$OUT" "nonce skips ahead"
check "insufficient balance rejected" has "$OUT" "insufficient balance - ALU001 has 5"
check "nonce increments per outgoing transfer" has "$OUT" "next nonce 2"
check "history lists sender, recipient, amount, fee, nonce" matches "$OUT" "TRANSFER +ALU001 +ALU002 +2 +1 +1"
check "history logs the reward" matches "$OUT" "REWARD +SYSTEM +ALU001 +9 +1 +-"
check "recipient history shows incoming transfer" matches "$OUT" "for ALU002 \\(balance 5"
check "debit = amount + fee (9 - 4 - 3 = 2)" matches "$OUT" "ALU001 +2 +2"
OUT=$(run "$S" account 'transfer ALU001 ALU002 1 1\nbalances\nexit\n')
check "nonce stays used after a restart" has "$OUT" "nonce already used: replay rejected"
check "account balances identical after restart" matches "$OUT" "ALU002 +5 +0"
sed -i '1s/|3|/|9|/' "$S/data/transfers_account.txt"
OUT=$(run "$S" account 'exit\n')
check "edited transfer log rejected (signature check)" has "$OUT" "signature is invalid"

# ---------------------------------------------------------------------------
echo "== POOL MINING =="
S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK001 ALU001\nreturn BK001\nmine pool 3\nexit\n')
check "pool table printed" has "$OUT" "| Miner ID | Hash rate | Attempts | Blocks found | Share % | Reward     |"
check "2% pool fee deducted (101 -> 98.98)" has "$OUT" "Pool fee (2%): 2.02 coins  ->  distributed to miners: 98.98"
check "shares sum to 100%" has "$OUT" "| TOTAL    |"
check "attempt totals add up" [ "$(grep -E '^  \| M[0-9]' <<< "$OUT" | awk -F'|' '{s+=$4} END{print s}')" = "$(grep -E '^  \| TOTAL' <<< "$OUT" | awk -F'|' '{print $4+0}')" ]
OUT=$(run "$S" utxo 'borrow BK002 ALU002\nmine pool 1\nmine pool 9\nexit\n')
check "pool size outside 2-8 rejected" [ "$(grep -c 'pool size must be between' <<< "$OUT")" -eq 2 ]

# ---------------------------------------------------------------------------
echo "== CLOUD MINING =="
S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK001 ALU001\nreturn BK001\nmine cloud 2\nexit\n')
check "profitable rental shows net profit" matches "$OUT" "Net profit +: [0-9]+\\.[0-9]+ coins"
check "summary shows gross earnings and total fees" has "$OUT" "Gross earnings   : 101.00"
# Deterministic unprofitable case: the pending pool is empty, so the rig
# earns nothing while the rental fee is still charged every round. (At
# difficulty 4 a rental is usually unprofitable too, but a lucky nonce can
# occasionally find a block, so that case is shown in the demo, not asserted.)
OUT=$(run "$S" utxo 'mine cloud 2\nexit\n')
check "unprofitable rental warns per round" has "$OUT" "WARNING: rental is unprofitable - cumulative fees"
check "unprofitable rental warns overall" has "$OUT" "this rental was UNPROFITABLE overall"
OUT=$(run "$S" utxo 'mine cloud 0\nmine cloud 6\nexit\n')
check "rental duration outside 1-5 rejected" [ "$(grep -c 'between 1 and 5 rounds' <<< "$OUT")" -eq 2 ]

# ---------------------------------------------------------------------------
echo "== TAMPER DETECTION =="
S=$(make_sandbox)
run "$S" utxo 'borrow BK001 ALU001\nreturn BK001\nmine solo\nexit\n' >/dev/null
cp "$S/data/chain.txt" "$S/chain.bak"
OUT=$(run "$S" utxo 'validate\nexit\n'); check "untouched chain validates" has "$OUT" "VALID"

sed -i '3s/|10|/|50|/' "$S/data/chain.txt"
OUT=$(run "$S" utxo 'validate\nborrow BK002 ALU002\nmine solo\nexit\n')
check "edited reward detected" has "$OUT" "first problem at block index 2: stored hash does not match"
check "lending refused on tampered chain" has "$OUT" "refusing to change anything"

cp "$S/chain.bak" "$S/data/chain.txt"
awk -F'|' 'BEGIN{OFS="|"} NR==2{$10=$10+1}1' "$S/chain.bak" > "$S/data/chain.txt"
OUT=$(run "$S" utxo 'validate\nexit\n')
check "edited nonce detected" has "$OUT" "first problem at block index 1"

cp "$S/chain.bak" "$S/data/chain.txt"
python3 "$FORGE" "$S/data/chain.txt" 50 >/dev/null
OUT=$(run "$S" utxo 'validate\nexit\n')
check "re-mined forgery still caught by the ECDSA signature" has "$OUT" "ECDSA signature is invalid"

cp "$S/chain.bak" "$S/data/chain.txt"
printf '1|BK002|Americanah|ALU002|Jane Smith|BORROWED|10|\n' > "$S/data/pending.txt"
OUT=$(run "$S" utxo 'exit\n')
check "edited pending file rejected" has "$OUT" "fails validation"
rm "$S/data/pending.txt"

rm "$S/keys/public.pem"
OUT=$(run "$S" utxo 'validate\nexit\n')
check "missing public key re-derived, chain still valid" has "$OUT" "re-deriving it"
check "  ...and every signature still verifies" has "$OUT" "VALID"

# ---------------------------------------------------------------------------
echo "== MEMORY SAFETY (ASan/UBSan) =="
make -C "$ROOT_DIR" -s asan >/dev/null 2>&1 || { echo "asan build failed"; FAIL=$((FAIL + 1)); }
S=$(make_sandbox)
OUT=$(run "$S" utxo 'borrow BK001 ALU001\nreturn BK001\nborrow BK002 ALU002\nadvance 20\nreturn BK002\nborrow BK003 ALU003\nadvance 20\ncheck overdue\nmine pool 4\ntransfer ALU001 ALU002 3\ntransfer ALU001 ALU002 3 #1\nview records\nexit\n')
check "ASan: UTXO session clean" hasnt "$OUT" "ERROR: AddressSanitizer"
check "ASan: no leaks (UTXO)" hasnt "$OUT" "LeakSanitizer"
OUT=$(run "$S" account 'borrow BK001 ALU002\nreturn BK001\nmine cloud 3\ntransfer ALU001 ALU002 1 0\nhistory ALU001\nhistory ALU002\nexit\n')
check "ASan: account session clean" hasnt "$OUT" "ERROR: AddressSanitizer"
check "ASan: no leaks (account)" hasnt "$OUT" "LeakSanitizer"
check "ASan: no undefined behaviour" hasnt "$OUT" "runtime error"
make -C "$ROOT_DIR" -s clean all >/dev/null

echo
echo "================================================================"
echo "  $PASS passed, $FAIL failed"
echo "================================================================"
[ "$FAIL" -eq 0 ]
