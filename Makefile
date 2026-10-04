CC      ?= cc
CFLAGS  ?= -O2 -march=native -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L

all: perft bench mmap.bin

mmapgen: src/mmapgen.c src/geometry.c src/mmap.h
	$(CC) $(CFLAGS) -o $@ src/mmapgen.c src/geometry.c

ENGINE  = src/engine.c src/geometry.c src/classic.c src/bitboard.c
HEADERS = src/mmap.h src/bitboard.h

perft: src/perft.c $(ENGINE) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/perft.c $(ENGINE)

bench: src/bench.c $(ENGINE) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/bench.c $(ENGINE)

bench-count: src/bench.c $(ENGINE) $(HEADERS)
	$(CC) $(CFLAGS) -DCOUNT_READS -o $@ src/bench.c $(ENGINE)

mmap.bin: mmapgen
	./mmapgen $@

test: all
	./perft test

# pin su un core P (CPU ibrida): evita che il confronto finisca su un core E.
# Senza taskset (es. macOS) il benchmark gira senza pin.
BENCH_CPU ?= 2
PIN = $(shell command -v taskset >/dev/null 2>&1 && echo taskset -c $(BENCH_CPU))
benchmark: bench bench-count mmap.bin
	./bench-count
	$(PIN) ./bench

clean:
	rm -f mmapgen perft bench bench-count mmap.bin

.PHONY: all test benchmark clean
