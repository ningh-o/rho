CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
# prelude_data.c is listed explicitly: after `make clean` the wildcard cannot
# see it, and the explicit entry forces generation before compiling; sort
# dedupes once the generated file shows up in the wildcard too
BOOT_SRC := $(sort $(wildcard boot/src/*.c) boot/src/prelude_data.c)
BOOT_OBJ := $(BOOT_SRC:.c=.o)
WASI_SDK ?= $(HOME)/Developer/tools/wasi-sdk
WASI_CC := $(WASI_SDK)/bin/clang
WASI_FLAGS := --target=wasm32-wasi --sysroot=$(WASI_SDK)/share/wasi-sysroot -O2

build/rho-boot: $(BOOT_OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(BOOT_OBJ)

boot/src/%.o: boot/src/%.c boot/src/rho.h boot/src/prelude_data.c
	$(CC) $(CFLAGS) -c -o $@ $<

CORE := boot/prelude/core.rho

boot/src/prelude_data.c: $(CORE) boot/prelude/hosted.rho boot/prelude/wasi.rho boot/prelude/esp32c3.rho tools/embed.py
	python3 tools/embed.py PRELUDE_SOURCE boot/src/prelude_data.c $(CORE) boot/prelude/hosted.rho
	python3 tools/embed.py PRELUDE_WASI_SOURCE boot/src/prelude_wasi_data.inc $(CORE) boot/prelude/wasi.rho
	python3 tools/embed.py PRELUDE_ESP32_SOURCE boot/src/prelude_esp32_data.inc $(CORE) boot/prelude/esp32c3.rho
	cat boot/src/prelude_wasi_data.inc boot/src/prelude_esp32_data.inc >> boot/src/prelude_data.c

# the compiler itself as wasm32-wasi: the browser playground runs this
build/rho-boot.wasm: $(BOOT_SRC) boot/src/rho.h
	@mkdir -p build
	$(WASI_CC) $(WASI_FLAGS) -o $@ $(BOOT_SRC)

# everything the static site needs, ready to serve from site/
site: build/rho-boot.wasm
	cp build/rho-boot.wasm site/assets/rho-boot.wasm
	mkdir -p site/spec
	cp spec/spec.md site/spec/spec.md

.PHONY: test test-site site goldens fmt-check clean
test: build/rho-boot
	./build/rho-boot selftest

test-site: build/rho-boot build/rho-boot.wasm
	./build/rho-boot test corpus --target wasm32-wasi
	node tools/test_browser_compiler.mjs
	node tools/verify_examples.mjs

goldens: build/rho-boot
	./build/rho-boot selftest --update-goldens

fmt-check: build/rho-boot
	./build/rho-boot selftest --fmt

clean:
	rm -rf build boot/src/*.o boot/src/prelude_data.c boot/src/prelude_wasi_data.inc boot/src/prelude_data.h
