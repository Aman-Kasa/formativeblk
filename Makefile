CC      := gcc
CFLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -g
LDLIBS  := -lcrypto

SRC_DIR := src
BUILD_DIR := build
BIN     := lending_tracker

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean debug asan run test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ASan/UBSan build for the memory-safety test pass (Phase 8)
asan: CFLAGS += -fsanitize=address,undefined -O0
asan: clean all

run: $(BIN)
	./$(BIN)

# Phase 8 test matrix (registry, borrow, return, blockchain, cryptography,
# tampering, memory safety) — see tests/run_tests.sh.
test:
	bash tests/run_tests.sh

clean:
	rm -rf $(BUILD_DIR) $(BIN)
