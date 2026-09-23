# boot keeps only: compile rho → wasm32-wasi, and the corpus oracle.
# Features frozen at 0.4.0 + triple-quote; fmt and the native backends
# live in the self-hosted compiler package. Two package roots:
# libs/compiler/full.rho (every target) and libs/compiler/web.rho
# (wasm-only — reachability drops the six native backend modules).
CC ?= cc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wno-unused-parameter
# prelude_data.c is listed explicitly: after `make clean` the wildcard cannot
# see it, and the explicit entry forces generation before compiling; sort
# dedupes once the generated file shows up in the wildcard too
BOOT_SRC := $(sort $(wildcard boot/src/*.c) boot/src/prelude_data.c)
BOOT_OBJ := $(BOOT_SRC:.c=.o)

build/rho-boot: $(BOOT_OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(BOOT_OBJ)

boot/src/%.o: boot/src/%.c boot/src/rho.h boot/src/ir.h boot/src/prelude_data.c
	$(CC) $(CFLAGS) -c -o $@ $<

# one prelude: the pure core + the wasi tail (the only target boot emits)
CORE := boot/prelude/core.rho

boot/src/prelude_data.c: $(CORE) boot/prelude/wasi.rho tools/embed.py
	python3 tools/embed.py PRELUDE_SOURCE boot/src/prelude_data.c $(CORE) boot/prelude/wasi.rho

# the served compiler asset is the self-hosted compiler's WEB root: the
# C seed turns the wasm-only package (libs/compiler/web.rho) into a
# wasm32-wasi module — one megabyte lighter than the full root, because
# reachability drops the native backends the browser cannot use. The full
# root (libs/compiler/full.rho) is the toolchain proper; the gate grades
# it as build/gate/m.wasm. wasi-sdk is retired: rho has always emitted
# wasm (and native images) directly, in-process, and no C compiler takes
# part in any shipped artifact anymore.
MIRROR_SRC := libs/compiler/web.rho libs/compiler/full.rho $(wildcard libs/compiler/*.rho) $(wildcard libs/compiler/native/*.rho)
site/assets/rho.wasm: build/rho-boot $(MIRROR_SRC)
	@mkdir -p site/assets
	./build/rho-boot build libs/compiler/web.rho --target wasm32-wasi -o $@

# the same artifact at its historical path — the site tests, the LSP and
# the vite plugin all read build/rho.wasm. A copy of the self-built asset,
# never a wasi-sdk product.
build/rho.wasm: site/assets/rho.wasm
	cp $< $@

# everything the static site needs, ready to serve from site/
site: build/rho.wasm
	mkdir -p site/spec
	cp spec/spec.md site/spec/spec.md

.PHONY: test test-lang test-site site goldens clean
test: build/rho-boot
	./build/rho-boot selftest

# the language suites (boot features: strops/multiline/modsys;
# mirror-only: opt, eq). Each runner is self-sufficient and prints its
# own verdict.
test-lang: build/rho-boot
	sh tests/lang/strops/run.sh
	sh tests/lang/multiline/run.sh
	sh tests/lang/modsys/run.sh
	sh tests/lang/opt/run.sh
	sh tests/lang/eq/run.sh

test-site: build/rho-boot build/rho.wasm
	./build/rho-boot test corpus --target wasm32-wasi
	node tools/test_browser_compiler.mjs
	node tools/verify_examples.mjs

goldens: build/rho-boot
	./build/rho-boot selftest --update-goldens

clean:
	rm -rf build boot/src/*.o boot/src/prelude_data.c
