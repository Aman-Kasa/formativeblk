#!/usr/bin/env python3
"""Attacker simulation for the tamper-detection demo.

Raises the token_reward of the first RETURNED block in data/chain.txt to a
new amount, then does everything an attacker without the private key CAN do
to hide the change:
  * recomputes that block's tx_id,
  * re-runs proof of work so the block hash meets its difficulty again,
  * relinks and re-mines every later block so all previous_hash links match.

The one thing it cannot do is produce valid ECDSA signatures, so it keeps the
old ones. `validate chain` should then report an invalid signature - which
shows why hashing and proof of work alone are not enough to protect the chain.

Usage: python3 tests/forge_chain.py [chain_file] [new_reward]
"""
import hashlib
import sys

TX_FEE = 1


def sha(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def block_hash(f):
    # Must match serialize_for_hash() in src/blockchain.c
    payload = "|".join([f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]])
    return sha(payload)


def remine(f):
    difficulty = int(f[8])
    nonce = 0
    while True:
        f[9] = str(nonce)
        h = block_hash(f)
        if h.startswith("0" * difficulty):
            f[13] = h
            return nonce + 1
        nonce += 1


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "data/chain.txt"
    new_reward = sys.argv[2] if len(sys.argv) > 2 else "50"
    with open(path, encoding="utf-8") as fh:
        rows = [line.rstrip("\n").split("|") for line in fh if line.strip()]

    target = next((i for i, f in enumerate(rows) if f[6] == "RETURNED"), None)
    if target is None:
        sys.exit("no RETURNED block to forge - return and mine a book first")

    f = rows[target]
    print(f"Forging block {f[0]}: token_reward {f[10]} -> {new_reward}")
    f[10] = new_reward
    f[11] = sha(f"REWARD|SYSTEM|{f[4]}|{f[2]}|{f[10]}|{TX_FEE}|{f[1]}")
    for i in range(target, len(rows)):
        if i > 0:
            rows[i][7] = rows[i - 1][13]
        attempts = remine(rows[i])
        print(f"  re-mined block {rows[i][0]} in {attempts} attempts -> {rows[i][13][:16]}...")
    print("  signatures left unchanged (forging them needs the private key)")

    with open(path, "w", encoding="utf-8") as fh:
        for row in rows:
            fh.write("|".join(row) + "\n")


if __name__ == "__main__":
    main()
