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
| `KING` | (legality) read from the king's square: interference on the line, see below |

The file is about 2.2 MB, is produced by `mmapgen`, and is loaded by the engine
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

### Flattened

The loop above has four nested levels: 64 squares → views of the piece →
mask bits (`while (m)`) → promotions. Inner loops whose trip count depends on
the data end in branches the CPU cannot predict well, and each misprediction
flushes the pipeline. `gen_pseudo_flat` replaces them with two linear passes:

1. **Collect.** Visit only the side's own pieces (`ctz` over its occupancy
   word) and append their view indices to a flat array. 4 indices are always
   copied and the write position advances by the real count, so there is no
   inner loop.
2. **Emit.** One loop over the active views. Each view carries its moves
   already built (`mv[8]`, promotions expanded to 4 entries per cell through a
   256-byte `EXPAND` table). A branch-free compaction keeps the moves whose bit
   is set:

```c
for (int i = 0; i < na; i++) {
    const View *v = &VIEWS[act[i]];
    unsigned m = EXPAND[v->promo][MM[v->base + key[v->key] * 8 + v->pos]];
    if (!m) continue;
    for (int k = 0; k < v->nmv; k++) { out[n] = v->mv[k]; n += m >> k & 1; }
}
```

Fully branch-free was *not* the fastest. A fixed 8-step compaction with no
`if (!m)` (compile with `-DFLAT_MODE=0`) loses on opening positions, where
many pieces are blocked and their mask is 0. The rule that wins is: **remove
branches that depend on occupancy, keep branches that depend on geometry or
are strongly biased.** `!m` is almost always predicted correctly, and `nmv`
is a constant of the view, not of the position.

| variant (`FLAT_MODE`) | pseudo-legal vs nested | perft pos1 / pos2 / pos3, Mnps |
|---|---|---|
| 0: fixed 8 steps, no skip | 0.79x | 80 / 126 / 78 |
| 1: skip empty masks | 0.86x | 116 / 153 / 81 |
| **2: skip empty + `nmv` steps** (default) | **0.69x** | **142 / 175 / 87** |

Walking only the own pieces requires one occupancy word per colour in mmap's
state (16 bytes, two XORs in make).

## Legality by interference

Most pieces cannot make an illegal move. A move can only be illegal if the
moving piece is **pinned**, the king is **in check**, the king itself moves,
or it is en passant. mmap therefore does not test moves one by one. It asks
the table which pieces **interfere** with the king, and verifies only those.

**1. Interference from the mmap.** From the king's square, each of the 4 lines
through it is read with the `KING` semantics. A single 8-byte entry describes
both directions of the line:

| byte | meaning |
|---|---|
| 0 | the enemy to verify: the first piece if it is an enemy (possible **check**), or the second if the first is ours (possible **pin**) |
| 1 | the squares that answer the check: those between king and enemy, plus the enemy |
| 2 | the x-ray square behind the king on the same line (the king cannot retreat there) |
| 3 | our candidate pinned piece |

Knight and pawn checks come from the existing knight and pawn-capture rings of
the king's square.

**2. Verify only where there is interference.** For each candidate enemy, one
board read checks whether it is really a slider of that line (rook/queen on
ranks and files, bishop/queen on diagonals). In most positions no candidate
exists and nothing is verified at all.

**3. The single loop, now legal.** Every piece other than the king runs through
the same lookup loop as before. Each destination is then filtered,
branch-free, by an `allow` mask:

```c
uint64_t allow = evasion & (pinned >> sq & 1 ? pin_line[sq] : ~0ull);
...
out[n] = move; n += allow >> to & 1;   /* written always, kept if allowed */
```

`evasion` is everything when not in check, the blocking/capturing squares
under a single check, and nothing under a double check. `pin_line` is the
whole line of the pin.

**4. Two point checks remain.**
- King moves: each destination is tested with the mmap attack query, and the
  x-ray square is excluded. This is the equivalent of Stockfish's
  `attackers_to(to, occ ^ ksq)`.
- En passant: rare, so it is verified with a full make and test.

No make, no copy, no per-move test for any other piece. `gen_legal_flat`
combines this with the flattened loop (the `allow` mask goes into the
compaction: `n += m >> k & allow >> to & 1`). `gen_legal_shuf` replaces that
compaction with `pshufb` (see *Experiment 2*); it is what `perft` uses. The
nested version (`gen_legal_int`) and the older make-and-test filter
(`gen_legal`) are kept for the benchmark.

## Usage

```sh
make            # builds mmapgen, perft and bench, generates mmap.bin
make test       # perft on the reference positions
make benchmark  # three-way comparison (see below)
./perft 6                  # perft from the initial position
MMAP_GEN=shuf ./perft test # perft suite with another legal generator
                           # (hyb default, shuf, flat, int, gather, sf, bb, if, mmap)
./perft divide 3 "<fen>"   # per-root-move counts
```

The generator passes perft on the six classic reference positions (initial
position d6, Kiwipete d5, positions 3–6 of the Chess Programming Wiki at
depth 4–6).

## Layout

```
src/mmap.h       types, constants, cell layout, shared read primitives
src/geometry.c   keys and views (deterministic, shared)
src/mmapgen.c    freezes the semantics into mmap.bin
src/engine.c     incremental serialization, single loop, make, FEN
src/classic.c    classic IF-based generator (baseline)
src/bitboard.h   bitboard attack tables and inline primitives (shared)
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
| **mmap, flattened** | `board[64]` + occupancy per colour + keys | same tables, two linear passes (see *Flattened*) |
| **mmap, legality by interference** | `board[64]` + keys | same single loop; pins and checks read from the `KING` table, only candidates verified; destinations filtered by a branch-free `allow` mask (see *Legality by interference*) |

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
  sets across all 8 legal variants, identical pseudo-legal sets across 4
  generators, and identical attack answers (64 squares
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
| bitboard | 87.4 | **0.33** | 0.29 – 0.37 |
| mmap, flattened | 170.0 – 252.9 | **0.67 – 0.69** | 0.67 – 0.69 |

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
| mmap, legality by interference | 397.6 | **0.16** | 0.16 – 0.17 |
| mmap, interference + flattened | 258.5 | **0.10** | 0.10 |
| mmap, own state | 2342.5 | 1.00 | |
| classic IF, own state | 1376.6 | 0.59 | 0.51 – 0.59 |
| Stockfish, own state | 106.8 | **0.05** | 0.04 – 0.05 |
| mmap by interference, own state | 393.8 | **0.17** | 0.16 – 0.17 |
| mmap interference + flattened, own state | 253.1 | **0.10** | 0.10 |

**4. Cost of the state: copy + make per move**

| state maintained | bytes copied | ns/move |
|---|---|---|
| everything | 944 | 41.8 |
| board + mmap keys | 72 + 800 | 36.9 |
| board + bitboards | 144 | 10.0 |
| board only | 72 | 5.9 |

**5. Perft, bulk counting at the leaves (Mnps, higher is better)**

| | mmap | classic IF | bitboard + shared filter | Stockfish legality | mmap by interference | mmap interference + flattened |
|---|---|---|---|---|---|---|
| *same state* pos1 d5 | 18.0 | 15.7 | 20.7 | 262.6 | 99.3 | 141.9 |
| *same state* pos2 d4 | 18.9 | 16.8 | 21.8 | 350.3 | 123.8 | 171.4 |
| *same state* pos3 d6 | 13.3 | 11.7 | 17.2 | 168.3 | 53.3 | 87.0 |
| *own state* pos1 d5 | 19.1 | 37.7 | – | 454.0 | 100.7 | 143.3 |
| *own state* pos2 d4 | 20.0 | 37.3 | – | 502.1 | 125.2 | 172.1 |
| *own state* pos3 d6 | 14.0 | 23.9 | – | 277.8 | 53.7 | 87.7 |

**6. State reads per position** (instrumented build)

| | board | mmap keys | mmap table | bitboards | geometry tables* |
|---|---|---|---|---|---|
| pseudo, mmap | 64.0 | 30.2 | 30.2 | – | 94.2 |
| pseudo, classic IF | 137.7 | – | – | – | 48.9 |
| pseudo, bitboard | – | – | – | 13.0 | 5.9 |
| pseudo, mmap flattened | 15.2 | 30.2 | 30.2 | 1.0 | 45.3 |
| king attack, mmap | 0.4 | 6.9 | 6.9 | – | 7.9 |
| king attack, classic IF | 18.3 | – | – | – | 16.3 |
| king attack, bitboard | – | – | – | 12.0 | 2.0 |
| legal, mmap | 121.7 | 319.7 | 319.7 | – | 425.6 |
| legal, classic IF | 945.6 | – | – | – | 732.7 |
| legal, Stockfish | – | – | – | 57.1 | 13.9 |
| legal, mmap by interference | 67.8 | 55.9 | 55.9 | – | 22.7 |
| legal, mmap interference + flattened | 19.0 | 55.9 | 55.9 | 1.0 | 66.2 |

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
5. **Legality by interference closes most of that gap for mmap.** Moving the
   pin and check logic into the `KING` table makes mmap's legal generation
   **6.4x faster** than its own make-and-test version, and **3.5x faster than
   the classic engine on its own state** (394 vs 1377 ns/position). Legality
   now costs about 130 ns on top of pseudo-legal generation (265 ns). State
   reads drop from 762 to 180 per position. Stockfish-style bitboards remain
   about 4x faster. Most of the remaining gap is in pseudo-legal generation
   (87 vs 265 ns) and in testing king destinations one by one. The endgame
   position (pos3), where king moves dominate, is where mmap trails most
   (0.25x of make-and-test mmap time vs 0.15x on pos2).
6. **Flattening the loop is worth about 1.5x.** Two linear passes instead
   of four nested loops make pseudo-legal generation 1.45x faster (board reads
   64 → 15) and legal generation 1.56x faster (396 → 253 ns/position). perft
   gains 45% on pos1, 41% on pos2 and 69% on pos3. Removing every branch was
   not optimal: the occupancy-dependent ones had to go, the
   geometry-dependent ones are cheap (see *Flattened*). With this, mmap's
   legal generation is **5.4x faster than the classic engine on its own state**
   and about **2.4x behind Stockfish-style bitboards** (253 vs 104
   ns/position; about 3x in perft).

### Conclusion

The hypothesis holds only partly. **Freezing occupancy decisions into a table
does beat explicit branching** at equal state: mmap is consistently faster than
the classic IF generator when both read the same structure. With legality by
interference also frozen into the table, mmap beats the classic engine by
3.5x even when the classic engine keeps only its own, much cheaper state.
But **the way mmap serializes the position is expensive to maintain**, and
bitboards already realize the "precomputed table indexed by occupancy" idea
with a much cheaper index (PEXT on 64-bit sets). Stockfish-style bitboards
therefore stay ahead: about 2.4x on legal generation and 3x in perft once
mmap's loop is flattened.

### Threats to validity

- One machine, one compiler (clang not tested). Code alignment alone moved
  the classic/mmap pseudo-legal ratio between 1.00 and 1.25 across builds.
- No hardware counters in this first experiment (`perf_event_paranoid` was 4).
  The explanations above are inferred from read counts and timings.
  Experiment 2 adds counters and confirms them.
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

## Experiment 2: move generation as a channel

### Hypothesis

Treat the CPU as a channel with three properties:

- **width**: bits processed per instruction;
- **latency**: length of chains of dependent loads;
- **errors**: branch mispredictions, which flush the pipeline like a
  retransmission.

Experiment 1 showed that bitboards are wider (64 squares per operation
against 8). Neither generator, however, comes close to filling the channel:
about 5 Gbit/s of useful output against roughly 1.2 Tbit/s of scalar ALU
width. The hypothesis was that the limit is latency, and that mmap's ~30
*independent* lookups per position could be overlapped with SIMD gathers.

### Variants

Same tables, same collected views; only the emission differs.

| variant | what changes |
|---|---|
| `mmap piatto` | scalar compaction, `if (!m)` and an `nmv`-step loop per view |
| `mmap pshufb` | each view's 8 prebuilt moves (8 × 16 bit = 128 bit) are compacted with one `pshufb` driven by a 256-entry mask table; `n += popcount(m)`. **No branch, no loop per view** |
| `mmap gather+pshufb` | as above, but key, table entry and promotion expansion are fetched for 8 views at a time with `vpgatherdd` (AVX2) |

To make the 128-bit compaction possible, `Move` became 16 bits
(`from | to << 6 | promo << 12`) **for every generator**, so output cost stays
equal. Cross-checks now cover 5 pseudo-legal and 10 legal variants.

### Metrics

Hardware counters via `perf_event_open`, user space only, opened on the P-core
PMU (`cpu_core`) and pinned with `taskset`. They count instructions, cycles,
branches and branch misses. Each counted pass covers at least about 16k
positions, after a warm-up pass. *bit/cycle* is useful output (16 bits per
generated move) per cycle. Cycles are frequency-independent and are the
primary measure here.

### Regimes

The result depends on how state and branch history look, so four regimes
were measured:

| regime | state | branch predictor |
|---|---|---|
| 256 positions, repeated | hot | **memorizes the positions**: unrealistic |
| 2048 positions | mostly L2 | cannot memorize |
| 16384 positions | cold (15 MB of `Pos`) | cannot memorize |
| **children** | **hot (just made in L1)**, never repeated | cannot memorize |

The *children* regime is closest to a real search. Every position of the set
plays each of its moves on a local copy and generates on the child. The
cost of the same loop without generation (copy + make: 175 cycles, 700
instructions) is subtracted.

### Results

**Pseudo-legal generation, 2048 positions, per position**

| | ns | instructions | cycles | IPC | branches | mispredicts | bit/cycle |
|---|---|---|---|---|---|---|---|
| mmap nested | 373 | 2639 | 956 | 2.76 | 313.5 | 11.69 | 0.68 |
| classic IF | 451 | 2542 | 1208 | 2.10 | 435.5 | 26.01 | 0.54 |
| bitboard | 136 | 688 | 297 | 2.32 | 98.6 | 4.62 | 2.20 |
| mmap flattened | 252 | 2006 | 629 | 3.19 | 183.3 | 5.16 | 1.04 |
| **mmap pshufb** | **87** | 1000 | **250** | **4.00** | 51.3 | **1.03** | **2.62** |
| mmap gather+pshufb | 147 | 917 | 438 | 2.09 | 59.2 | 0.65 | 1.49 |

**Children regime, net of copy + make, per child**

| | cycles | instructions | mispredicts |
|---|---|---|---|
| pseudo, bitboard | 282 | 720 | 5.2 |
| **pseudo, mmap pshufb** | **235** | 1006 | **1.5** |
| pseudo, mmap flattened | 562 | 2067 | 4.1 |
| legal, Stockfish | 350 | 986 | 5.5 |
| legal, mmap pshufb | 669 | 2265 | 5.4 |
| legal, mmap flattened | 878 | 3033 | 7.0 |

The children rows are identical under `-O3` within 1–2%.

**Pseudo-legal cycles per position across regimes** (mmap pshufb / bitboard)

| 256 repeated | 2048 | 16384 cold | children |
|---|---|---|---|
| 234 / 135 (1.73x) | 250 / 297 (**0.84x**) | 512 / 341 (1.50x) | 235 / 282 (**0.83x**) |

**Legal generation and perft, own state**

| | ns/position (2048) | cycles | perft pos1 / pos2 / pos3, Mnps |
|---|---|---|---|
| Stockfish | 105 | 346 | 489 / 539 / 291 |
| mmap interference, flattened | 245 | 945 | 146 / 188 / 94 |
| **mmap interference, pshufb** | **176** | **711** | **150 / 230 / 102** |
| mmap interference, gather | 205 | 868 | 134 / 200 / 94 |

### Analysis

1. **The bottleneck was errors, not width.** Going from the flattened loop to
   `pshufb` removes no lookups and *adds* no width, but cuts branches from 183
   to 51 and mispredictions from 5.2 to 1.0. Cycles drop from 629 to 250 and
   IPC rises to 4.0.
2. **Widening the channel with gathers hurts.** `vpgatherdd` cuts
   instructions (917) but IPC collapses to 2.1: on this CPU a gather costs
   about as much as its 8 scalar loads, plus extra latency. The hypothesis
   "mmap is latency-bound, overlap the lookups" is **rejected**. The
   out-of-order core already overlaps independent scalar lookups.
3. **mmap now beats bitboards on pseudo-legal generation in realistic
   regimes**: about 16% fewer cycles with hot, non-repeating state (children
   and 2048). It executes 40% *more* instructions, but at nearly twice the
   IPC with a quarter of the mispredictions. Mispredicts per position
   correlate with cycles far better than instruction counts do.
4. **The win depends on the regime, and honestly so.**
   - With repeated positions the predictor learns the bitboard's branches
     (0.03 mispredicts) and the bitboard wins.
   - With cold state mmap's 944-byte `Pos` costs more cache misses than the
     bitboard's 144 bytes, and the bitboard wins.
   - mmap's advantage is specific to the case of hot state and unpredictable
     positions, which is the search case.
5. **Legal generation is still about 1.9x behind** (669 vs 350 cycles per
   child). The gap is now nearly all king safety: interference and king
   destinations keep ~5 mispredicts per position, the same as Stockfish's
   whole generator. In perft the gap widens to 2.3–3.3x because mmap's copy +
   make (about 38 ns) is about 4x Stockfish's.

### Conclusion

In the channel model the dominant term is the **error rate**, not the width.
A table-driven generator whose emission is branch-free (frozen logic plus
`pshufb` compaction) outperforms a bitboard generator on pseudo-legal moves
by about 16% in cycles. It does so while executing more instructions, which
answers the earlier question: fewer instructions is not the goal, fewer
mispredictions is. Closing the remaining gap means applying the same
treatment to king safety and shrinking the state.

### Reproduce

```sh
taskset -c 2 ./bench 2048 15     # all sections with hardware counters
taskset -c 2 ./bench 256 15      # repeated-positions regime
taskset -c 2 ./bench 16384 15    # cold-state regime
BREAKDOWN_ONLY=1 taskset -c 2 ./bench   # per-phase breakdown only
```

Hardware counters need `kernel.perf_event_paranoid <= 2`.

## Experiment 3: the hybrid

### Motivation

After Experiment 2, mmap won pseudo-legal generation but trailed in legal
generation by about 1.9x. The phase breakdown put the gap in king safety:

| phase (cycles/position) | mmap | Stockfish |
|---|---|---|
| checks and pins | 81 (0.6 mispredicts) | 22 (0.04) |
| king moves | 196 (1.1) | 51 (0.04) |

The difference is the **order** of the two questions. mmap asked about
occupancy first: the `KING` table returns the first piece on every line,
whatever it is. Only then did it read the board to check the piece type.
Every enemy on a line produced an "apparent" interference and branches that
were hard to predict. Stockfish asks about **types first**:

```c
snipers = (ROOK_PSEUDO[ksq]   & (rooks|queens)   & them)   // empty-board geometry
        | (BISHOP_PSEUDO[ksq] & (bishops|queens) & them);  // usually 0
for each s in snipers:
    b = BETWEEN[ksq][s] & occupied;   // interference, only where it can matter
    b == 0               -> check
    one bit, ours        -> pin
```

King safety is a question about **sets** (who could attack, who stands in
between). 64-bit set algebra answers it without data-dependent branches.

### The hybrid (`gen_legal_hyb`)

- **Pieces' moves:** mmap tables, `pshufb` emission (Experiment 2).
- **Checks and pins:** Stockfish's sniper/`BETWEEN` algorithm on typed
  bitboards (`byType`, `byColor`), plus knight/pawn checks by intersection.
- **King destinations:** `attackers_to(to, occupied ^ king)`. Removing the
  king from the occupancy resolves x-rays automatically.
- **Fast path:** with no check, no pin and no en passant square (the common
  case), every pseudo-legal piece move is legal. The pure pseudo-legal
  emission runs, chosen by one well-predicted branch per position.
- **Slow path:** the allowed cells of a view are extracted with one
  instruction, `pext(allow, KEYBB[key])`. This required sorting ring cells by
  square. The only descending keys, black double pushes, swap their two bits.
- **State:** board + bitboards + keys (the whole `Pos`).

`gen_legal_hyb_pdep` is a variant that checks king destinations with
mmap's own attack views, converting each mask to a bitboard with
`pdep(mask, KEYBB[key])`. It tests whether mmap tables can stand in for
`attackers_to`.

### Verification

Cross-check: 12 legal variants identical on the sampled positions. The
sampled positions **missed** a bug that perft caught: the reversed bit order
of black double pushes under `pext`. So `perft` can now run its deep suite
(start d6, Kiwipete d5, ...) with any generator (`MMAP_GEN=...`). All mmap
variants and the Stockfish-style one pass it.

### Results

Cycles from hardware counters. Two runs; the range is shown where they
differ.

**Phases (cycles/position, mispredicts in parentheses)**

| phase | mmap (interference) | **hybrid** | Stockfish |
|---|---|---|---|
| checks and pins | 81 (0.62) | **18.5 (0.05)** | 22 (0.04) |
| king moves | 196 (1.1) | **65 (0.01)** | 50 (0.04) |
| whole legal generation | 958–967 | **416–421** | 344–350 |

**Legal generation, own state, 2048 positions, per position**

| | cycles | instructions | IPC | mispredicts |
|---|---|---|---|---|
| Stockfish | 357 | 940 | 2.63 | 5.1 |
| mmap interference, pshufb | 639–670 | 2021 | 3.0–3.2 | 4.2 |
| **hybrid** | **414–432** | 1488 | 3.5 | **2.6** |
| hybrid, pdep king | 491–496 | 1715 | 3.5 | 2.6 |

**Children regime (hot state, never repeated), net of copy + make**

| | cycles/child |
|---|---|
| Stockfish | 350–351 |
| mmap interference, pshufb | 646–648 |
| **hybrid** | **422–434** |
| hybrid, pdep king | 504–518 |

**perft, own state (Mnps)**

| | pos1 d5 | pos2 d4 | pos3 d6 |
|---|---|---|---|
| Stockfish | 484–490 | 537 | 291 |
| mmap interference, pshufb | 162–164 | 245 | 102 |
| **hybrid** | **204** | **347–349** | **183–185** |

### Analysis

1. **Type first, occupancy second.** Checks and pins drop from 81 to 18.5
   cycles and from 0.62 to 0.05 mispredicts, slightly better than Stockfish's
   own code (22 cycles). King moves drop from 196 to 65 cycles. The
   interference intuition was right: it matters, but it must be counted only
   for the pieces that geometry and type say could attack.
2. **mmap tables do not beat `attackers_to` for attack queries.** The pdep
   variant is about 18% slower. One PEXT per slider family covers a whole
   line pair. mmap needs one lookup per line plus one per ring. The rule from
   the channel model holds again: attacks are set questions.
3. **The legal gap shrinks from 1.9x to about 1.2x** (414–432 vs 357 cycles;
   422–434 vs 350 net in the children regime). The hybrid makes **half the
   mispredictions** of Stockfish (2.6 vs 5.1) but executes **58% more
   instructions** (1488 vs 940). After removing the branch problem, the
   remaining cost is instruction count. Most of it is the per-view emission:
   about 30 views, each with a key load, a table lookup, an expansion and a
   16-byte store.
4. **perft is still 1.5–2.4x behind**, and here the cause is the state, not
   generation. Copy + make costs 167 cycles per move with the keys against 44
   for board + bitboards. Bulk counting at the leaves hides some of it, but
   every interior node pays for it.

### Conclusion

The hybrid takes the best of both models. Frozen tables with branch-free
emission produce moves. Set algebra answers king safety. Legal generation is
within about 20% of a Stockfish-style generator, with half its mispredictions.
The next bottleneck is no longer branches but **instruction count in
emission** and **the size of the serialized state**. Both point to the same
target: fewer, wider keys. The key could be computed on the fly with PEXT
from the bitboards the hybrid already maintains, instead of being kept up to
date across 386 counters.

## Next iterations

- **Compute the serialization instead of maintaining it.** The benchmark shows
  the cost is in keeping the keys up to date. With two bitboards (white,
  black), a line's key can be computed on the fly with `PEXT`: two
  instructions per line, indexing `[pext(white)][pext(black)]` instead of
  base 3. This is how bitboards win, and the natural next step for mmap.
- **No verification at all.** A richer cell alphabet on line keys (slider
  matching the line vs other piece) would let the `KING` table confirm pins
  and checks without reading the board. The cost is base 5 instead of base 3:
  390,625 keys per full line, so the table grows and leaves L2.
- **King destinations in the table.** King moves are still tested one by one.
  An "attacked squares around the king" summary per ring would remove those
  queries.
- **Make/unmake instead of copy**, if the keys are kept: today every move copies
  800 bytes of keys.
- **Keys from bitboards.** The hybrid already maintains bitboards; computing
  line keys on the fly (PEXT of the white and black occupancy along the line)
  would drop the 800 bytes of keys from the state and the 124 extra cycles
  per make.
- **Smaller state.** With hot state mmap wins on pseudo-legal generation.
  With cold state it loses, because a position touches up to 13 cache lines
  of keys against 2 for bitboards.
