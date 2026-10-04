# mmap

A chess move generator in which **the rules are frozen into a map of
constants** and the generator only has to query it.

## The idea

The classic starting point is a `moves[piece][square]` table holding the
squares reachable on an empty board. But some algorithm is always needed to
refine that result: is the square occupied? By whom? Does the bishop's ray
stop here? Can the pawn push two squares? Is castling still allowed?

**mmap takes this idea to the extreme.** All of those questions are answered
*once and for all* when the table is generated. At runtime a single loop is
left, which:

1. reads the **serialization** of the current position;
2. uses it as an index into the mmap;
3. gets the destination squares directly.

The generation loop contains no `if (square occupied)`, no "walk the ray until
you hit something", and no branch on colour or piece type.

## How a position is serialized

Serializing the whole board into a single index is impossible (there are
~10⁴⁴ positions). The serialization is therefore **split into keys**: each key
describes a short sequence of cells, and each cell is a base-3 digit:

| digit | meaning |
|---|---|
| 0 | empty |
| 1 | white piece |
| 2 | black piece |

A sequence of `n` cells becomes a number in `[0, 3ⁿ)`. The keys are:

| keys | cells | count |
|---|---|---|
| ranks, files, diagonals, anti-diagonals | the squares of the line | 46 |
| knight ring of every square | the squares a knight jumps to | 64 |
| king ring of every square | the adjacent squares | 64 |
| pawn push (per colour) | 1 or 2 squares ahead | 96 |
| pawn capture (per colour) | the 2 diagonals, on the capture layer | 112 |
| castling | squares to be empty + virtual right cell | 4 |

**386 keys** in total: this is the serialized position. The keys are
**maintained incrementally**: every cell knows which keys it belongs to and with
which weight `3ⁱ`, so moving a piece is just a series of `key[k] += Δ · 3ⁱ`.

### Non-geometric state becomes occupancy

What is usually special-case logic is also turned into cells:

- **En passant**: after a double push, a *ghost pawn* appears on the skipped
  square. It is visible only on the **capture layer** (cells 64..127), which
  is what pawn capture keys read. The enemy pawn "captures" it with the
  ordinary capture table without knowing it.
- **Castling rights**: 4 virtual cells (128..131) included in the castling
  key. A lost right is an "occupied" cell, so castling is blocked exactly as if
  a piece stood in the way.
- **Promotion**: a property of the view (a pawn on the seventh rank), not a
  test on the destination.

## The mmap

For every combination

```
(semantics, colour, key length, key value, position)
```

the file `mmap.bin` holds one byte: the **destination mask** over the cell
sequence. There are only a few semantics:

| semantics | frozen rule |
|---|---|
| `SLIDE` | from `pos`, slide both ways: empty yes, enemy yes then stop, own stop |
| `LEAP` | every cell not occupied by an own piece |
| `PUSH` | consecutive empty cells from the start |
| `CAPT` | enemy cells only |
| `CASTLE` | the destination only if *all* cells are empty (right included) |
| `HIT` | (attacks) only the first enemy piece in each direction of the line |

The file is about 1 MB, is produced by `mmapgen`, and is loaded by the engine
with `mmap(2)`: the rules are literally read-only memory.

## The single loop

Every piece on every square has a precomputed list of **views**: which key it
reads, with which semantics, and how the mask bits translate into squares. An
enemy piece (or an empty square) has zero views. This is the whole generator:

```c
for (int sq = 0; sq < 64; sq++) {
    VList vl = GV[side][board[sq]][sq];
    for (const View *v = first(vl); v < last(vl); v++) {
        unsigned m = MM[v->base + key[v->key] * 8 + v->pos];
        while (m) {
            int b = ctz(m); m &= m - 1;
            emit(sq, v->to[b], ...);
        }
    }
}
```

## Legality

The first iteration splits legality into two levels:

1. **Pseudo-legality** lives entirely in the mmap (en passant, castling,
   promotions and blocking included).
2. **King safety**: the attack test also queries the mmap. From the king's
   square it reads the same keys (lines with `HIT`, rings and captures with
   `CAPT`) and matches the pieces found against a mask of possible attackers.
   Each move is filtered by playing it on a copy of the position.

The legality filter is therefore the only part that is still "algorithmic",
and it is the next frontier.

## Usage

```sh
make            # builds mmapgen, perft and bench, generates mmap.bin
make test       # perft on the reference positions
make benchmark  # three-way comparison (see below)
./perft 6                  # perft from the initial position
./perft divide 3 "<fen>"   # per-root-move counts
```

The generator passes perft on the six classic reference positions (initial
position, Kiwipete, positions 3–6 of the Chess Programming Wiki).

## Layout

```
src/mmap.h       types, constants, cell layout, shared read primitives
src/geometry.c   keys and views (deterministic, shared)
src/mmapgen.c    freezes the semantics into mmap.bin
src/engine.c     incremental serialization, single loop, make, FEN
src/classic.c    classic IF-based generator (baseline)
src/bitboard.c   Stockfish-style bitboard generator (baseline)
src/perft.c      verification
src/bench.c      benchmark
```

---

## Experiment: mmap vs classic IF-based vs Stockfish-style bitboards

### Question

Is the mmap generator, where every occupancy decision is precomputed in a
table, more efficient than (a) a classic IF-based generator and (b) a
Stockfish-style bitboard generator?

### Implementations under test

| | state it reads | how it decides |
|---|---|---|
| **mmap** (`engine.c`) | `board[64]` + 386 base-3 keys | one table lookup per view, no occupancy branches |
| **classic IF** (`classic.c`) | `board[64]` | empty-board target arrays (knight/king jumps, rays) + one `if` per square read |
| **bitboard** (`bitboard.c`) | Stockfish layout: `byType[7]`, `byColor[2]` | PEXT slider attacks, pawn moves via shifts in bulk |
| **bitboard, Stockfish legality** | same | checkers + pinned pieces computed up front; evasions restricted to "capture or block"; king moves checked with `attackers_to`; en passant fully verified; **no make, no copy** |

### Controls

- **Same state structure.** All engines use the same `Pos` (944 bytes: board
  and flags 72, bitboards 72, mmap keys 800) and the same positions in memory.
- **Same read primitives.** Every engine reads state only through
  `static inline` accessors (`piece_at`, `key_at`, `bb_type`, `bb_color`,
  `side_to_move`, ...) and writes moves with the same `emit()`. After
  optimization each accessor is a single memory load, so it adds no layer of
  cost.
- **Same legality filter.** In the "same state" rows, mmap, classic and
  bitboard use one shared filter (`DEFINE_GEN_LEGAL`: pseudo-legal moves → copy
  + `make_move` → king attack test). Only the pseudo-legal generator and the
  attack test differ. Stockfish-style legality is reported separately because
  it is a different algorithm.
- **Two state regimes.**
  - *Same state*: every engine copies and updates the whole `Pos`.
  - *Own state*: each engine copies and updates only what it reads (mmap:
    board + keys; classic: board; Stockfish: board + bitboards).
- **Same build.** Same compiler flags, direct calls, no function pointers in
  measured loops.
- **Correctness first.** Before any timing, a cross-check over all sampled
  positions requires identical pseudo-legal move sets, identical legal move
  sets across all 6 legal variants, and identical attack answers (64 squares
  × 2 sides). Perft node counts are also compared.

### Method

- **Workload.** 2048 positions sampled (p = 1/16) from the depth-3 perft trees
  of the six reference positions, then shuffled so the branch predictor cannot
  learn a repeating pattern. Averages: 40.9 pseudo-legal and 39.6 legal moves
  per position.
- **Timing.** `clock_gettime(CLOCK_MONOTONIC)` around a full pass over the
  set. 15 rounds with rotated order (ABC, BCA, CAB, ...) so that no engine
  benefits systematically from warm caches or CPU frequency. The **median** is
  reported. Perft uses the best of 5.
- **Machine.** Intel Core Ultra 7 155H (hybrid), process pinned with `taskset`
  to a 4.8 GHz P-core. Linux 7.0, gcc 13.3, `-O2 -march=native` (BMI2/PEXT
  available).
- **Read counts.** A separate build (`-DCOUNT_READS`) instruments the
  accessors to count state reads; the timed build contains no counters.

### Results

Absolute times drift with CPU frequency from run to run; **ratios are stable**
and are what should be compared. The ratio is time relative to mmap: below 1
is faster than mmap, above 1 is slower. Medians from one representative run;
the robustness column gives the ratio range across `-O2`/`-O3` and workloads
of 256, 2048 and 16384 positions.

**1. Pseudo-legal generation (same state)**

| | ns/position | ratio | robustness |
|---|---|---|---|
| mmap | 264.7 | 1.00 | |
| classic IF | 300.0 | 1.13 | 1.00 – 1.17 |
| bitboard | 87.4 | **0.33** | 0.29 – 0.35 |

**2. Attack test**

| | king square, ns/query | ratio | all 64 squares, ns/query | ratio |
|---|---|---|---|---|
| mmap | 11.7 | 1.00 | 22.4 | 1.00 |
| classic IF | 22.6 | 1.93 (0.90 – 2.07) | 26.2 | 1.17 (1.14 – 1.18) |
| bitboard | 3.5 | **0.30** (0.30 – 0.46) | 3.4 | **0.15** (0.13 – 0.17) |

**3. Legal generation**

| | ns/position | ratio | robustness |
|---|---|---|---|
| mmap, same state | 2536.8 | 1.00 | |
| classic IF, same state | 3073.6 | 1.21 | 1.16 – 1.22 |
| bitboard + shared filter, same state | 2017.5 | 0.80 | 0.79 – 0.81 |
| bitboard, Stockfish legality | 103.7 | **0.04** | 0.04 |
| mmap, own state | 2342.5 | 1.00 | |
| classic IF, own state | 1376.6 | 0.59 | 0.51 – 0.59 |
| Stockfish, own state | 106.8 | **0.05** | 0.04 – 0.05 |

**4. Cost of the state: copy + make per move**

| state maintained | bytes copied | ns/move |
|---|---|---|
| everything | 944 | 41.8 |
| board + mmap keys | 72 + 800 | 36.9 |
| board + bitboards | 144 | 10.0 |
| board only | 72 | 5.9 |

**5. Perft, bulk counting at the leaves (Mnps, higher is better)**

| | mmap | classic IF | bitboard + shared filter | Stockfish legality |
|---|---|---|---|---|
| *same state* pos1 d5 | 18.0 | 15.7 | 20.7 | 262.6 |
| *same state* pos2 d4 | 18.9 | 16.8 | 21.8 | 350.3 |
| *same state* pos3 d6 | 13.3 | 11.7 | 17.2 | 168.3 |
| *own state* pos1 d5 | 19.1 | 37.7 | – | 454.0 |
| *own state* pos2 d4 | 20.0 | 37.3 | – | 502.1 |
| *own state* pos3 d6 | 14.0 | 23.9 | – | 277.8 |

**6. State reads per position** (instrumented build)

| | board | mmap keys | mmap table | bitboards | geometry tables* |
|---|---|---|---|---|---|
| pseudo, mmap | 64.0 | 30.2 | 30.2 | – | 94.2 |
| pseudo, classic IF | 137.7 | – | – | – | 48.9 |
| pseudo, bitboard | – | – | – | 13.0 | 5.9 |
| king attack, mmap | 0.4 | 6.9 | 6.9 | – | 7.9 |
| king attack, classic IF | 18.3 | – | – | – | 16.3 |
| king attack, bitboard | – | – | – | 12.0 | 2.0 |
| legal, mmap | 121.7 | 319.7 | 319.7 | – | 425.6 |
| legal, classic IF | 945.6 | – | – | – | 732.7 |
| legal, Stockfish | – | – | – | 57.1 | 13.9 |

\* mmap: view lists and views; classic: ray/jump array entries; bitboard:
slider PEXT lookups only (knight, king and pawn attack tables are not counted).

### Analysis

1. **mmap beats the classic IF generator at reading.** With the same state, it
   reads the board less than half as often and replaces data-dependent
   branches (empty? enemy? does the ray stop?) with a table lookup. The gain
   is large on attack tests (about 2x on the king square), where a whole line
   is resolved by one lookup, and modest on generation (about 1.1x).
2. **mmap loses at writing.** Keeping 386 keys up to date makes copy + make
   cost about 37 ns per move instead of 6 ns. With each engine keeping only its
   own state, the classic generator is about 1.7–2x faster than mmap
   end-to-end, because the write cost (about 31 ns × 40 moves) outweighs the
   read gain (about 0.1 µs per position).
3. **Bitboards beat both at reading.** Bitboards are also a "frozen table"
   approach: PEXT attack tables are precomputed for every relevant occupancy.
   The difference is in the index. A bitboard occupancy is extracted with one
   PEXT instruction from state that is 64 bits per piece set. An mmap key is a
   base-3 number that must be maintained incrementally across many keys. The
   bitboard generator also produces many moves per operation (pawn shifts in
   bulk). Pseudo-legal generation is 3x faster than mmap, and attack tests
   3–7x faster.
4. **The dominant factor is the legality algorithm, not the generator.**
   Stockfish-style legality (pins and checks computed once, no make/copy per
   move) is about 20–25x faster than any make-and-test filter, including the
   same bitboard generator using the shared filter (0.80 → 0.04).

### Conclusion

The hypothesis holds only partly. **Freezing occupancy decisions into a table
does beat explicit branching** at equal state: mmap is consistently faster than
the classic IF generator when both read the same structure. But **the way
mmap serializes the position is too expensive to maintain**, and bitboards
already realize the "precomputed table indexed by occupancy" idea with a much
cheaper index (PEXT on 64-bit sets). The biggest gap of all comes from
legality: computing pins and checks up front instead of playing and testing
each move.

### Threats to validity

- One machine, one compiler (clang not tested). Code alignment alone moved
  the classic/mmap pseudo-legal ratio between 1.00 and 1.25 across builds.
- No hardware counters: `perf` was unavailable (`perf_event_paranoid = 4`), so
  instructions, IPC, branch misses and cache misses were not measured. The
  explanations above are inferred from read counts and timings.
- The bitboard engine is Stockfish-*style*, not Stockfish. It uses copy-make
  instead of do/undo with `StateInfo` and has no hashing.
- Perft with bulk counting favours generators that produce legal moves
  without make (Stockfish style), as it is meant to.

### Reproduce

```sh
make benchmark                 # read counts + timings pinned to CPU 2
taskset -c 2 ./bench 16384 15  # other workload size / round count
BENCH_CPU=4 make benchmark     # pick another core
```

## Next iterations

- **Compute the serialization instead of maintaining it.** The benchmark shows
  the cost is in keeping the keys up to date. With two bitboards (white,
  black), a line's key can be computed on the fly with `PEXT`: two
  instructions per line, indexing `[pext(white)][pext(black)]` instead of
  base 3. This is how bitboards win, and the natural next step for mmap.
- **Legality inside the mmap.** A richer cell alphabet (king, slider matching
  the line, other piece) would let a line key answer "is this piece pinned?"
  and "is the king in check along this line?" directly. That is the piece of
  information that gives Stockfish-style legality its 20x advantage.
- **Make/unmake instead of copy**, if the keys are kept: today every move copies
  800 bytes of keys.
- **Hardware counters.** Instructions retired, IPC, branch misses and cache
  misses per section via `perf_event_open`.
