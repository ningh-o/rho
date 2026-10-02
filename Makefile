# rho — build the seed compiler (boot). One binary, no dependencies
# beyond a C11 compiler. Deterministic by construction.
CC ?= clang
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror
SRC := $(wildcard boot/*.c)
BIN := build/rho

.PHONY: all test clean selftest asan

all: $(BIN)

$(BIN): $(SRC) boot/rho.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

# the sanitizer build (T3.17's mechanical detector): the gate leg runs
# it over robust/, the corpus, and the saved fuzz programs; UBSAN
# errors are fatal (no recover), LeakSanitizer stays off — the arena
# never frees by design and would drown the report
asan:
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined \
	  -fno-sanitize-recover=all -o build/rho-asan $(SRC)

selftest: $(BIN)
	./$(BIN) selftest

test: selftest
	./tests/run-fmt-tests.sh
	./tests/run-fmt-self.sh
	./tests/run-selfhost.sh
	./tests/run-diff.sh
	./tests/run-corpus-diff.sh
	./tests/run-corpus-repo.sh
	./tests/run-robust.sh
	node tools/fuzz/gen.mjs --from 1 --to 150 --budget 780 --step 15
	./build/rho test tests/suites

clean:
	rm -rf build
