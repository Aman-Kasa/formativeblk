# Formative 2 – Library Lending Chain with Token Rewards and Mining

This extends the Formative 1 Library Book Lending Tracker (the root of this repository).
Borrowing and returning books now feed a **pending pool**. Returning a book creates a
**token reward transaction**. Nothing reaches the chain until it is **mined with proof of work**,
using solo, pool or cloud mining. Member balances are kept under the **UTXO** or the
**account-based** model, chosen at startup.

---

## 1. Dependencies

| Requirement | Notes |
|---|---|
| GCC (C11) and `make` | Any recent GCC. Clang also works (`make CC=clang`). |
| OpenSSL 3 development headers | SHA-256 and ECDSA (P-256). Ubuntu/Debian: `sudo apt install libssl-dev`. macOS: `brew install openssl@3`. |
| Python 3 *(tests only)* | Used only by `tests/forge_chain.py`, the attacker simulation. |

There are no other libraries.

## 2. Build and run

```bash
cd formative-2
make                     # builds ./lending_tracker
./lending_tracker        # asks which transaction model to use
```

Command-line options:

```bash
./lending_tracker --model utxo            # UTXO model
./lending_tracker --model account         # account-based model
./lending_tracker --difficulty 3          # proof-of-work difficulty 1-4 (default 2)
./lending_tracker --seed 7                # fixed random seed, so pool-mining hash rates repeat
```

On macOS with Homebrew OpenSSL, pass the paths explicitly:
`make CFLAGS+="-I$(brew --prefix openssl@3)/include" LDLIBS="-L$(brew --prefix openssl@3)/lib -lcrypto"`.

Other targets: `make test` runs the test suite, `make asan` builds with AddressSanitizer and
UndefinedBehaviorSanitizer, and `make clean` removes build output.

To start from a fresh chain, delete the runtime files:
`rm -f data/chain.txt data/pending.txt data/transfers_*.txt`.
Keep `keys/` if you want to keep the same signing identity.

## 3. Switching between transaction models

The model is fixed for the whole session, as the brief requires. Choose it at startup with
`--model utxo` or `--model account`, or pick `1` or `2` at the startup prompt. To switch, exit and
restart with the other model.

Balances are rebuilt from the chain every time the program starts, so either model can be used
on the same chain. Each model keeps its own signed transfer log:
`data/transfers_utxo.txt` and `data/transfers_account.txt`.

## 4. Setting the mining difficulty

Difficulty is the number of leading `0` hex characters a block hash must have. The range is 1–4
and the default is 2.

* At startup: `./lending_tracker --difficulty 3`
* During a session: `difficulty 3`. Typing `difficulty` alone shows the current value.

Every block stores the difficulty it was mined at, so blocks mined at different difficulties
all still validate. The expected number of attempts is 16^difficulty: 16, 256, 4,096 and 65,536.

## 5. Commands

| Command | What it does |
|---|---|
| `borrow <book_id> <member_id>` | Queues a BORROWED record in the pending pool |
| `return <book_id>` | Queues a RETURNED record and creates its reward transaction (10 coins on time, 5 late) |
| `check overdue` | Queues an OVERDUE record for every book past the 14-day loan period (reward 0, no transaction) |
| `advance <days>` | Moves the simulated clock forward, so late returns and overdue books can be demonstrated |
| `pending` | Shows the pending pool |
| `mine solo` | One miner confirms every pending record and shows the hash attempts |
| `mine pool [2-8]` | Pool mining with a reward-sharing table (default 4 miners) |
| `mine cloud <1-5>` | Cloud mining for N rented rounds, with a profit summary |
| `difficulty [1-4]` | Shows or sets the proof-of-work difficulty |
| `balances` | Shows member balances (and the full UTXO set in the UTXO model) |
| `utxos` | UTXO model: shows the full UTXO set |
| `transfer <from> <to> <amount> [#id ...]` | UTXO model: spends UTXOs, chosen automatically or by the listed ids |
| `transfer <from> <to> <amount> <nonce>` | Account model: transfer that must quote the sender's next nonce |
| `history <member_id>` | Account model: the member's transaction history |
| `view records` | Every confirmed block, with reward, tx_id, nonce and signature validity |
| `validate chain` | Re-verifies hashes, links, proof of work, tx_ids and signatures |
| `list books`, `list members` | Shows the registries |
| `help`, `exit` | |

## 6. Testing the mining simulations

Run the full suite:

```bash
make test
```

It covers 72 checks: the pending pool, rewards, the overdue case, proof of work, both
transaction models, all three mining methods, tamper detection, and an AddressSanitizer run.

To try each method by hand, start a fresh session with `./lending_tracker --model utxo --seed 7`
and enter:

```text
borrow BK001 ALU001
borrow BK002 ALU002
return BK001
pending                  # 3 unconfirmed records, the return carries a 10-coin reward
mine solo                # hash attempts per block, solo reward summary, UTXO set after each block

borrow BK003 ALU003
advance 20
return BK002             # late: 5-coin reward
check overdue            # BK003 is overdue: OVERDUE record, reward 0, no transaction
mine pool 4              # reward-sharing table with the 2% pool fee

borrow BK001 ALU002
mine cloud 1             # profitable: one block earns 50, costs 20 rental + 5 maintenance
return BK001
mine cloud 3             # unprofitable: one block's reward, but three rounds of rental
difficulty 4
borrow BK002 ALU001
mine cloud 2             # usually unprofitable at difficulty 4: per-round warnings
```

The edge cases from the brief:

| Edge case | How to trigger it | Expected output |
|---|---|---|
| Overdue book creates no transaction | `borrow BK003 ALU003`, `advance 20`, `check overdue` | `OVERDUE ... 0 none` in `pending` |
| Insufficient balance (account) | `transfer ALU001 ALU002 100 0` | `ERROR: insufficient balance ...` |
| Insufficient inputs (UTXO) | `transfer ALU001 ALU002 100` | `ERROR: insufficient funds ...` |
| Failed / reused nonce | repeat a transfer with the same nonce | `nonce already used: replay rejected` |
| Double spend (UTXO) | `transfer ALU001 ALU002 1 #1` twice | `double spend rejected - input #1 is already spent` |
| Unprofitable cloud rental | `difficulty 4` then `mine cloud 2`, or `mine cloud 2` with an empty pool | `WARNING: rental is unprofitable ...` |

To demonstrate tamper detection:

```bash
python3 tests/forge_chain.py      # raises a reward to 50, re-mines every block, keeps old signatures
./lending_tracker --model utxo    # startup warning, then: validate chain
```

The forged chain passes every hash, link and proof-of-work check. It is caught only by the ECDSA
signature, because forging that needs the private key.

## 7. How it works

### Lending events trigger token transactions

| Event | Block action | `token_reward` | `tx_id` |
|---|---|---|---|
| Borrow | BORROWED | 0 | none |
| Return within 14 days | RETURNED | 10 | SHA-256 of the reward transaction |
| Return after 14 days | RETURNED | 5 | SHA-256 of the reward transaction |
| Still out after 14 days (`check overdue`) | OVERDUE | 0 | none, so no transaction is created |

The reward transaction pays SYSTEM → member, `token_reward − 1` coin fee. Its `tx_id` is
`SHA-256("REWARD|SYSTEM|member|book|reward|fee|timestamp")`. It is created when the book is
returned but **applied only when its block is mined**. Until then, the member's balance does not
change.

### The pending pool and mining

1. `borrow`, `return` and `check overdue` validate the request against the registry and the
   book's loan state (chain plus pending pool), then append an **unsigned, unconfirmed** record to
   the pending pool. The pool is saved to `data/pending.txt`.
2. A mining command takes records from the front of the pool. For each one it:
   * sets `index`, `previous_hash` and `difficulty`;
   * starts from nonce 0 and recomputes `SHA-256(all fields + nonce)` until the hash has
     `difficulty` leading zeros;
   * signs the winning hash with the library's ECDSA key;
   * appends the block, applies its reward transaction to the ledger, and saves everything.
3. Only after that is the record part of the chain and the member's balance updated.

`validate chain` re-checks every block's index order, `previous_hash` link, recomputed hash,
proof-of-work target, reward `tx_id`, and ECDSA signature.

### The UTXO model (`src/utxo.c`)

Coins are unspent outputs `{id, txid, vout, owner, amount, spent}`. A member's balance is the sum
of the outputs they own that are not yet spent.

* **Reward:** a transaction with no inputs creates one output of `reward − 1` for the member.
* **Transfer:** selects inputs (oldest first, or the `#ids` you list). It is rejected if any input
  is unknown, already spent, owned by someone else, or listed twice. It is also rejected if the
  inputs are less than `amount + 1 fee`. On success, the inputs are marked spent and it creates an
  output for the recipient plus a **change output** back to the sender for any excess.
* The full UTXO set is printed after every confirmed block.

### The account model (`src/account.c`)

Each member is `{balance, nonce, history}`.

* **Reward:** credits `reward − 1` to the member.
* **Transfer:** must quote the sender's current nonce. A lower nonce is rejected as a replay and a
  higher one as skipping ahead. The sender's balance must cover `amount + 1 fee`. On success the
  sender is debited, the recipient credited, and the sender's nonce incremented.
* **History:** each account keeps a linked list in memory of every transaction it was part of,
  with sender, recipient, amount, fee and nonce. `history <member_id>` prints it.

### Mining methods (`src/mining.c`)

| | How it works | Rewards |
|---|---|---|
| **Solo** | One miner runs proof of work on each pending block and prints the attempts per block. | 50 per block + all fees, to the one miner. |
| **Pool** | 2–8 miners, each with a random hash rate of 50–400 attempts per round. All miners hash at the same time, each on its own slice of the nonce space. The miner that finds a valid hash earliest in the round wins the block, and each miner is credited with the attempts it had made by that moment. | Total minus a 2% pool fee, split by `miner_attempts / total_attempts`, shown in a table. |
| **Cloud** | A rented rig with 2,000 attempts per round, for 1–5 rounds. Proof of work continues across rounds. | Each block earns 50 + fees, minus 10% maintenance. Each round costs 20 rental. It reports gross earnings, total fees and net profit, and warns whenever cumulative fees exceed cumulative rewards. |

## 8. Design choices and assumptions

* **One lending record per block.** The brief says all Formative 1 block fields stay unchanged,
  so each mined block still holds exactly one lending record. A mining run confirms the whole
  pending pool, as consecutive blocks.
* **Block fields added:** `token_reward` and `tx_id`, as the brief requires, plus `difficulty` and
  `nonce`, which proof of work needs. All four are included in the block hash, so a reward cannot
  be changed after mining.
* **Signing happens after mining.** Pending records are unsigned. The ECDSA signature covers the
  final proof-of-work hash, which covers every field. Because pending records are unsigned,
  `pending.txt` is re-checked against the registry and the reward rules on startup.
* **Fees.** Every transaction has a fixed 1-coin fee, including rewards: the brief says to deduct
  the fee before crediting the member. Fees go to whoever mines next. Miners are not library
  members, so their earnings are reported but are not part of either member ledger.
* **Transfers take effect immediately.** They are not lending records, so they don't wait in the
  pending pool. Each one is appended to a transfer log signed with the library's ECDSA key, tagged
  with the chain length at that moment. On startup, rewards from the chain and the logged
  transfers are replayed in their original order. So spent UTXOs stay spent and used nonces stay
  used across restarts, and an edited log is rejected.
* **Simulated clock.** `advance <days>` moves time forward, so late returns and overdue books can
  be shown without waiting 14 days. The offset lasts for one session.
* **Overdue is a status, not a return.** An OVERDUE book is still on loan and can be returned
  later, as a late return worth 5 coins.
* **Key safety fix from Formative 1.** If `keys/public.pem` is missing, it is now re-derived from
  the private key instead of generating a new key pair. Before, a new pair would have invalidated
  every signature on the chain.

All numbers (rewards, fees, loan period, hash rates, rental costs) are in `src/config.h`.

## 9. Source layout

```
src/
  main.c          startup: arguments, model choice, load and verify chain, pending pool and ledger
  cli.c           command loop
  config.h        every tunable number
  block.h         Block struct (Formative 1 fields + token_reward, tx_id, difficulty, nonce)
  blockchain.c    lending rules, pending pool, hashing, proof of work, validation
  mining.c        solo, pool and cloud mining
  ledger.c        model selection, signed transfer log, replay
  utxo.c          UTXO model
  account.c       account model
  persistence.c   chain and pending pool files
  display.c       shared output formatting
  simclock.c      simulated clock
  registry.c      books.txt / members.txt   (unchanged from Formative 1)
  crypto.c        SHA-256 and ECDSA          (Formative 1 + key fix)
tests/
  run_tests.sh    72-check test suite
  forge_chain.py  attacker simulation for the tamper demo
```
