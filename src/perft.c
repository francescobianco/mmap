/*
 * perft: conta i nodi dell'albero delle mosse legali e li confronta con
 * i valori di riferimento. E' la prova che la conoscenza congelata nella
 * mmap e' completa.
 *
 *   perft test                 suite di posizioni note
 *   perft <prof> [fen]         conteggio
 *   perft divide <prof> [fen]  conteggio per mossa radice
 */
#include "mmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STARTPOS "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

static uint64_t perft(const Pos *p, int depth)
{
    Move ms[256];
    int n = gen_legal(p, ms);
    if (depth <= 1)
        return depth == 1 ? (uint64_t)n : 1;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        Pos q = *p;
        make_move(&q, ms[i]);
        total += perft(&q, depth - 1);
    }
    return total;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int run(const char *fen, int depth, uint64_t expect, int divide)
{
    Pos p;
    if (pos_from_fen(&p, fen)) {
        fprintf(stderr, "FEN non valida: %s\n", fen);
        return 1;
    }
    double t0 = now();
    uint64_t nodes = 0;
    if (divide) {
        Move ms[256];
        char buf[8];
        int n = gen_legal(&p, ms);
        for (int i = 0; i < n; i++) {
            Pos q = p;
            make_move(&q, ms[i]);
            uint64_t c = perft(&q, depth - 1);
            move_str(ms[i], buf);
            printf("%s: %llu\n", buf, (unsigned long long)c);
            nodes += c;
        }
    } else {
        nodes = perft(&p, depth);
    }
    double dt = now() - t0;
    int ok = !expect || nodes == expect;
    printf("%-4s d%d %12llu  %6.2fs %7.1f Mnps  %s\n", expect ? (ok ? "OK" : "FAIL") : "",
           depth, (unsigned long long)nodes, dt, nodes / dt / 1e6, fen);
    return !ok;
}

int main(int argc, char **argv)
{
    engine_init();
    const char *path = getenv("MMAP_FILE") ? getenv("MMAP_FILE") : "mmap.bin";
    if (mmap_load(path))
        return 1;

    if (argc > 1 && !strcmp(argv[1], "test")) {
        static const struct { const char *fen; int depth; uint64_t nodes; } T[] = {
            { STARTPOS, 5, 4865609 },
            { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603 },
            { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 6, 11030083 },
            { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333 },
            { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487 },
            { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594 },
        };
        int fails = 0;
        for (size_t i = 0; i < sizeof T / sizeof *T; i++)
            fails += run(T[i].fen, T[i].depth, T[i].nodes, 0);
        printf("%s\n", fails ? "QUALCOSA NON VA" : "tutte le posizioni corrispondono");
        return fails != 0;
    }

    int divide = argc > 1 && !strcmp(argv[1], "divide");
    int a = 1 + divide;
    int depth = argc > a ? atoi(argv[a]) : 5;
    const char *fen = argc > a + 1 ? argv[a + 1] : STARTPOS;
    return run(fen, depth, 0, divide);
}
