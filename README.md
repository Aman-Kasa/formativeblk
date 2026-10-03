# Blockchain-Based Library Book Lending Tracker

A C-based blockchain application for tracking library book lending and returns using SHA-256 hashing, ECDSA digital signatures, persistent storage, and blockchain integrity validation.

---

## 1. Project Overview

This project implements a simple blockchain-based library book lending tracker in C.

The application records book borrowing and returning activities as blocks in a linked-list blockchain. Each block is protected using cryptographic hashing and digital signatures, while the blockchain validates the integrity of its records.

The application supports:

* Loading book and member registries
* Borrowing books
* Returning books
* Viewing lending records
* Validating the blockchain
* Detecting tampering
* Persistent blockchain storage
* SHA-256 hashing
* ECDSA digital signatures
* Automated testing

This is an educational, single-machine blockchain implementation and does not implement networking, mining, or distributed consensus.

---

## 2. Repository Structure

```text
individual-assignment_FA-1/
├── data/
│   ├── books.txt
│   ├── members.txt
│   └── chain.txt
│
├── docs/
│   └── Individual-Assignment [F:A](Aman-kasa).pdf
│
├── keys/
│   ├── private.pem
│   └── public.pem
│
├── src/
│   ├── main.c
│   ├── registry.c
│   ├── registry.h
│   ├── blockchain.c
│   ├── blockchain.h
│   ├── crypto.c
│   ├── crypto.h
│   ├── persistence.c
│   ├── persistence.h
│   ├── cli.c
│   └── cli.h
│
├── tests/
│   └── ...
│
├── .gitignore
├── Makefile
└── README.md
```

---

## 3. Requirements

The project requires:

* Linux / POSIX-compatible operating system
* GCC
* GNU Make
* OpenSSL 3
* OpenSSL development libraries

### Ubuntu / Pop!_OS

Install the required dependencies:

```bash
sudo apt update
sudo apt install build-essential libssl-dev
```

Check the installations:

```bash
gcc --version
make --version
openssl version
```

---

## 4. Getting the Project

Clone the repository:

```bash
git clone https://github.com/Aman-Kasa/blockchain-coursework.git
```

Enter the assignment directory:

```bash
cd blockchain-coursework/individual-assignment_FA-1
```

---

## 5. Compilation

Build the application using the provided Makefile:

```bash
make
```

The project builds the `lending_tracker` executable.

---

## 6. Running the Application

Run:

```bash
./lending_tracker
```

A successful startup looks similar to:

```text
Loaded chain: 1 block(s), integrity verified.
Library Lending Chain — type 'help' for commands.
>
```

The application loads the existing blockchain, verifies its integrity, and then starts the command-line interface.

---

## 7. Available Commands

Type:

```text
help
```

to display the available commands.

### Borrow a Book

```text
borrow <book_id> <member_id>
```

Records a book as borrowed by a registered member.

Example:

```text
borrow BK001 ALU001
```

### Return a Book

```text
return <book_id>
```

Records a book as returned.

Example:

```text
return BK001
```

### View Records

```text
view records
```

Displays every lending record stored on the blockchain.

### Validate the Blockchain

```text
validate chain
```

Verifies:

* Block hashes
* Previous-hash links
* Digital signatures
* Overall chain integrity

### List Books

```text
list books
```

Displays the book registry.

### List Members

```text
list members
```

Displays the member registry.

### Chain Status

```text
chain status
```

Shows the block count and overall chain validity.

### Help

```text
help
```

Displays the available commands.

### Exit

```text
exit
```

Closes the application.

---

## 8. Blockchain Implementation

Each lending or return operation is recorded as a block.

The blockchain uses:

* A genesis block
* Sequential block indexes
* Timestamps
* Previous-block hashes
* SHA-256 hashing
* ECDSA digital signatures
* Linked-list block storage

The chain links each block to the previous block using its cryptographic hash.

For example:

```text
Block 0
   │
   │ previous_hash → hash of Block 0
   ▼
Block 1
   │
   │ previous_hash → hash of Block 1
   ▼
Block 2
```

The chain can be verified using:

```text
validate chain
```

---

## 9. Security

### SHA-256

SHA-256 is used to generate a cryptographic hash for each block.

The block hash is calculated from the block's relevant data and its previous-block hash.

Changing block data causes the recomputed hash to differ from the stored hash.

### ECDSA

ECDSA using the NIST P-256 curve is used to digitally sign blocks.

The project stores the cryptographic keys in:

```text
keys/private.pem
keys/public.pem
```

The private key should **never be committed to GitHub**.

The repository's `.gitignore` excludes private key files.

---

## 10. Data Persistence

The blockchain is persisted in:

```text
data/chain.txt
```

The application can therefore save the blockchain and load it again when restarted.

On startup, the stored chain is loaded and its integrity is verified before normal commands are accepted.

---

## 11. Tamper Detection

The project includes a demonstration of blockchain tamper detection.

A basic demonstration is:

1. Create valid blockchain records.
2. Run:

```text
validate chain
```

3. Modify data belonging to an existing block in:

```text
data/chain.txt
```

4. Restart the application.
5. Run:

```text
validate chain
```

The application detects that the modified block no longer matches its stored cryptographic hash.

The system also prevents new lending transactions from being written when the existing blockchain has been compromised.

---

## 12. Registry Data

The application uses two registry files:

### Books

```text
data/books.txt
```

### Members

```text
data/members.txt
```

Book and member IDs are checked against these registries before lending transactions are recorded.

---

## 13. Testing

The project includes automated tests covering:

* Registry validation
* Borrow operations
* Return operations
* Blockchain creation
* Blockchain validation
* Persistence
* Hash tampering
* Signature validation
* Invalid input
* Error handling
* Memory safety

The completed test suite achieved:

```text
44 passed
0 failed
```

Memory-safety testing was also performed using:

* AddressSanitizer
* UndefinedBehaviorSanitizer

---

## 14. Main Source Modules

| File            | Purpose                                    |
| --------------- | ------------------------------------------ |
| `main.c`        | Application startup and initialization     |
| `registry.c`    | Book and member registry management        |
| `blockchain.c`  | Blockchain and lending operations          |
| `crypto.c`      | SHA-256 and ECDSA cryptographic operations |
| `persistence.c` | Blockchain saving and loading              |
| `cli.c`         | Command-line interface                     |

---

## 15. Assignment Deliverables

### Source Code

GitHub repository:

https://github.com/Aman-Kasa/blockchain-coursework/tree/main/individual-assignment_FA-1

### Demo Video

8-minute demonstration covering the required application functionality:

https://youtu.be/JaJVdVHRytU?si=eITR-bCaeq_FolFw

### Technical Report

The technical report contains the project design, implementation, security mechanisms, persistence, error handling, screenshots, challenges, and system design documentation.

The report is available in the repository under:

```text
docs/
```

Direct link:

https://github.com/Aman-Kasa/blockchain-coursework/blob/main/individual-assignment_FA-1/docs/Individual-Assignment%201-%5BF%3AA%5D(Aman-kasa).pdf

---

## 16. Author

**Aman Abraha Kasa**
#lastbagtime
**September 26, 2026**
