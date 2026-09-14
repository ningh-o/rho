CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
# prelude_data.c is listed explicitly: after `make clean` the wildcard cannot
# see it, and the explicit entry forces generation before compiling; sort
# dedupes once the generated file shows up in the wildcard too
BOOT_SRC := $(sort $(wildcard boot/src/*.c) boot/src/prelude_data.c)
BOOT_OBJ := $(BOOT_SRC:.c=.o)

build/rho-boot: $(BOOT_OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(BOOT_OBJ)

boot/src/%.o: boot/src/%.c boot/src/rho.h boot/src/prelude_data.c
	$(CC) $(CFLAGS) -c -o $@ $<

boot/src/prelude_data.c: boot/prelude/hosted.rho tools/embed.py
	python3 tools/embed.py boot/prelude/hosted.rho PRELUDE_SOURCE boot/src/prelude_data.c

.PHONY: test goldens fmt-check clean
test: build/rho-boot
	./build/rho-boot selftest

goldens: build/rho-boot
	./build/rho-boot selftest --update-goldens

fmt-check: build/rho-boot
	./build/rho-boot selftest --fmt

clean:
	rm -rf build boot/src/*.o boot/src/prelude_data.c boot/src/prelude_data.h
