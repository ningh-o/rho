# rho — build the seed compiler (boot). One binary, no dependencies
# beyond a C11 compiler. Deterministic by construction.
CC ?= clang
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror
SRC := $(wildcard boot/*.c)
BIN := build/rho

.PHONY: all test clean selftest

all: $(BIN)

$(BIN): $(SRC) boot/rho.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

selftest: $(BIN)
	./$(BIN) selftest

test: selftest
	./tests/run-check-tests.sh
	./tests/run-emit-tests.sh
	./tests/run-fmt-tests.sh
	./tests/run-fmt-self.sh
	./tests/run-set-tests.sh
	./tests/run-selfhost.sh
	./tests/run-diff.sh
	./tests/run-corpus-diff.sh
	./tests/run-corpus-repo.sh
	./tests/run-robust.sh
	./build/rho test tests/suites

clean:
	rm -rf build
