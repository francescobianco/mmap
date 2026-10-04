/*
 * bench: confronto a tre tra il generatore mmap, il generatore classico
 * a IF e un generatore bitboard stile Stockfish.
 *
 * Condizioni al contorno identiche:
 *   - stessa struttura di stato (Pos), stesse posizioni, stessa memoria;
 *   - stesse primitive di lettura (piece_at, key_at, bb_type, ...) e
 *     stessa emit();
 *   - stesso filtro di legalita' (DEFINE_GEN_LEGAL) e stesso make_move
 *     nelle righe "a parita' di stato";
 *   - stesse opzioni di compilazione, chiamate dirette, nessun puntatore
 *     a funzione nei cicli misurati.
 *
 *   bench [posizioni] [round]
 */
#include "mmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef COUNT_READS
uint64_t READS_BOARD, READS_KEY, READS_TABLE, READS_GEOM, READS_BB;
#endif

static const char *FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
};
#define NFENS (int)(sizeof FENS / sizeof *FENS)

/* ---- insieme di posizioni ---- */

static Pos *POS;
static int NPOS, CAPACITY;
static uint32_t rng = 0x2545F491;

static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

static void collect(const Pos *p, int depth)
{
    if (NPOS < CAPACITY && (rnd() & 15) == 0)
        POS[NPOS++] = *p;
    if (!depth)
        return;
    Move ms[256];
    int n = gen_legal(p, ms);
    for (int i = 0; i < n; i++) {
        Pos q = *p;
        make_move(&q, ms[i]);
        collect(&q, depth - 1);
    }
}

/* ---- verifica incrociata ---- */

static int cmp_move(const void *a, const void *b)
{
    const Move *x = a, *y = b;
    return (x->from - y->from) * 4096 + (x->to - y->to) * 16 + (x->promo - y->promo);
}

static int same(Move *a, int na, Move *b, int nb)
{
    if (na != nb)
        return 0;
    qsort(a, na, sizeof *a, cmp_move);
    qsort(b, nb, sizeof *b, cmp_move);
    return !memcmp(a, b, na * sizeof *a);
}

static int crosscheck(void)
{
    int bad = 0;
    for (int i = 0; i < NPOS; i++) {
        const Pos *p = &POS[i];
        Move a[512], x[512];
        int na = gen_pseudo(p, a);
        bad += !same(a, na, x, gen_pseudo_if(p, x));
        na = gen_pseudo(p, a);
        bad += !same(a, na, x, gen_pseudo_bb(p, x));
        int (*legal[])(const Pos *, Move *) = { gen_legal_if, gen_legal_if_board, gen_legal_own,
                                                gen_legal_bb, gen_legal_sf };
        for (size_t k = 0; k < sizeof legal / sizeof *legal; k++) {
            na = gen_legal(p, a);
            bad += !same(a, na, x, legal[k](p, x));
        }
        for (int sq = 0; sq < 64; sq++)
            for (int s = 0; s < 2; s++) {
                int r = attacked(p, sq, s);
                bad += (r != attacked_if(p, sq, s)) + (r != attacked_bb(p, sq, s));
            }
    }
    return bad;
}

/* ---- cicli misurati: chiamate dirette, generate da macro ---- */

#define BENCH_GEN(name, fn)                                                      \
    static uint64_t name(void)                                                   \
    {                                                                            \
        Move mv[512];                                                            \
        uint64_t c = 0;                                                          \
        for (int i = 0; i < NPOS; i++)                                           \
            c += fn(&POS[i], mv);                                                \
        return c;                                                                \
    }

#define BENCH_ATT(name, fn)                                                      \
    static uint64_t name(void)                                                   \
    {                                                                            \
        uint64_t c = 0;                                                          \
        for (int i = 0; i < NPOS; i++)                                           \
            for (int sq = 0; sq < 64; sq++)                                      \
                c += fn(&POS[i], sq, side_to_move(&POS[i]));                     \
        return c;                                                                \
    }

#define BENCH_KING(name, fn)                                                     \
    static uint64_t name(void)                                                   \
    {                                                                            \
        uint64_t c = 0;                                                          \
        for (int i = 0; i < NPOS; i++) {                                         \
            int s = side_to_move(&POS[i]);                                       \
            c += fn(&POS[i], king_square(&POS[i], s), s);                        \
        }                                                                        \
        return c;                                                                \
    }

/* copia + make di tutte le mosse legali: isola il costo dello stato */
static Move *MOVES;
static int *MOVE_OFF;

#define BENCH_MAKE(name, make, copy)                                             \
    static uint64_t name(void)                                                   \
    {                                                                            \
        uint64_t c = 0;                                                          \
        for (int i = 0; i < NPOS; i++)                                           \
            for (int k = MOVE_OFF[i]; k < MOVE_OFF[i + 1]; k++) {                \
                Pos q;                                                           \
                copy(&q, &POS[i]);                                               \
                make(&q, MOVES[k]);                                              \
                c += q.board[MOVES[k].to];                                       \
            }                                                                    \
        return c;                                                                \
    }

#define DEFINE_PERFT(name, legal, make, copy)                                    \
    static uint64_t name(const Pos *p, int depth)                                \
    {                                                                            \
        Move ms[256];                                                            \
        int n = legal(p, ms);                                                    \
        if (depth <= 1)                                                          \
            return n;                                                            \
        uint64_t t = 0;                                                          \
        for (int i = 0; i < n; i++) {                                            \
            Pos q;                                                               \
            copy(&q, p);                                                         \
            make(&q, ms[i]);                                                     \
            t += name(&q, depth - 1);                                            \
        }                                                                        \
        return t;                                                                \
    }

BENCH_GEN(b_pseudo_mmap, gen_pseudo)
BENCH_GEN(b_pseudo_if, gen_pseudo_if)
BENCH_GEN(b_pseudo_bb, gen_pseudo_bb)
BENCH_GEN(b_legal_mmap, gen_legal)
BENCH_GEN(b_legal_if, gen_legal_if)
BENCH_GEN(b_legal_bb, gen_legal_bb)
BENCH_GEN(b_legal_sf, gen_legal_sf)
BENCH_GEN(b_legal_mmap_own, gen_legal_own)
BENCH_GEN(b_legal_if_board, gen_legal_if_board)
BENCH_ATT(b_att_mmap, attacked)
BENCH_ATT(b_att_if, attacked_if)
BENCH_ATT(b_att_bb, attacked_bb)
BENCH_KING(b_king_mmap, attacked)
BENCH_KING(b_king_if, attacked_if)
BENCH_KING(b_king_bb, attacked_bb)
BENCH_MAKE(b_make_all, make_move, pos_copy)
BENCH_MAKE(b_make_keys, make_move_keys, pos_copy_keys)
BENCH_MAKE(b_make_bb, make_move_bb, pos_copy_bb)
BENCH_MAKE(b_make_board, make_move_board, pos_copy_board)
/* a parita' di stato: tutti copiano e aggiornano l'intera Pos */
DEFINE_PERFT(perft_mmap, gen_legal, make_move, pos_copy)
DEFINE_PERFT(perft_if, gen_legal_if, make_move, pos_copy)
DEFINE_PERFT(perft_bb, gen_legal_bb, make_move, pos_copy)
DEFINE_PERFT(perft_sf, gen_legal_sf, make_move, pos_copy)
/* stato proprio: ognuno copia e aggiorna solo cio' che legge */
DEFINE_PERFT(perft_mmap_own, gen_legal_own, make_move_keys, pos_copy_keys)
DEFINE_PERFT(perft_if_own, gen_legal_if_board, make_move_board, pos_copy_board)
DEFINE_PERFT(perft_sf_own, gen_legal_sf, make_move_bb, pos_copy_bb)

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#define MAXCASES 8

typedef struct {
    const char *name;
    uint64_t (*fn)(void);
} Case;

/* I casi vengono misurati a round con ordine ruotato (ABC, BCA, CAB ...)
   per non regalare a nessuno cache calda o frequenza della CPU.
   Il primo caso e' il riferimento dei rapporti. */
static void measure(const char *title, const Case *c, int nc, int rounds, const char *unit,
                    uint64_t units)
{
    static double t[MAXCASES][64];
    uint64_t check[MAXCASES];
    printf("== %s ==\n", title);
    for (int r = 0; r < rounds; r++)
        for (int k = 0; k < nc; k++) {
            int w = (k + r) % nc;
            double t0 = now();
            check[w] = c[w].fn();
            t[w][r] = now() - t0;
        }
    double med0 = 0;
    for (int k = 0; k < nc; k++) {
        qsort(t[k], rounds, sizeof(double), cmp_d);
        double med = t[k][rounds / 2];
        if (!k)
            med0 = med;
        printf("  %-28s min %8.1f  mediana %8.1f ns/%-5s  %5.2fx%s\n", c[k].name,
               t[k][0] / units * 1e9, med / units * 1e9, unit, med / med0,
               check[k] != check[0] ? "  RISULTATO DIVERSO!" : "");
    }
    printf("\n");
}

typedef uint64_t (*PerftFn)(const Pos *, int);

static void run_perft(const char *title, const char **names, const PerftFn *fn, int nf, int rounds)
{
    static const struct { int fen, depth; } P[] = { { 0, 5 }, { 1, 4 }, { 2, 6 } };
    printf("== %s ==\n  %-9s", title, "");
    for (int k = 0; k < nf; k++)
        printf(" %22s", names[k]);
    printf("\n");
    for (size_t i = 0; i < sizeof P / sizeof *P; i++) {
        Pos p;
        pos_from_fen(&p, FENS[P[i].fen]);
        double best[MAXCASES];
        uint64_t nodes[MAXCASES];
        for (int k = 0; k < nf; k++)
            best[k] = 1e9;
        for (int r = 0; r < rounds; r++)
            for (int k = 0; k < nf; k++) {
                int w = (k + r) % nf;
                double t0 = now();
                nodes[w] = fn[w](&p, P[i].depth);
                double dt = now() - t0;
                if (dt < best[w])
                    best[w] = dt;
            }
        printf("  pos%d d%d  ", P[i].fen + 1, P[i].depth);
        for (int k = 0; k < nf; k++)
            printf(" %6.1f Mnps (%4.2fx)  ", nodes[k] / best[k] / 1e6, best[k] / best[0]);
        int same_nodes = 1;
        for (int k = 1; k < nf; k++)
            same_nodes &= nodes[k] == nodes[0];
        printf("%s\n", same_nodes ? "" : "  NODI DIVERSI!");
    }
    printf("  (rapporto = tempo / tempo della prima colonna; > 1 = piu' lento)\n\n");
}

int main(int argc, char **argv)
{
    int want = argc > 1 ? atoi(argv[1]) : 2048;
    int rounds = argc > 2 ? atoi(argv[2]) : 15;
    if (rounds > 64)
        rounds = 64;

    engine_init();
    classic_init();
    bitboard_init();
    const char *path = getenv("MMAP_FILE") ? getenv("MMAP_FILE") : "mmap.bin";
    if (mmap_load(path))
        return 1;

    CAPACITY = 1 << 16;
    POS = malloc(CAPACITY * sizeof *POS);
    for (int i = 0; i < NFENS; i++) {
        Pos p;
        pos_from_fen(&p, FENS[i]);
        collect(&p, 3);
    }
    for (int i = NPOS - 1; i > 0; i--) { /* mescola: niente pattern ripetuti */
        int j = rnd() % (i + 1);
        Pos t = POS[i];
        POS[i] = POS[j];
        POS[j] = t;
    }
    if (want < NPOS)
        NPOS = want;

    uint64_t pseudo = 0, legal = 0;
    Move mv[512];
    MOVES = malloc(NPOS * 256 * sizeof *MOVES);
    MOVE_OFF = malloc((NPOS + 1) * sizeof *MOVE_OFF);
    for (int i = 0; i < NPOS; i++) {
        pseudo += gen_pseudo(&POS[i], mv);
        MOVE_OFF[i] = legal;
        legal += gen_legal(&POS[i], MOVES + legal);
    }
    MOVE_OFF[NPOS] = legal;
    printf("posizioni: %d   sizeof(Pos) = %zu: scacchiera %zu + bitboard %zu + chiavi %zu byte\n",
           NPOS, sizeof(Pos), (size_t)POS_BOARD_BYTES, (size_t)(POS_BB_BYTES - POS_BOARD_BYTES),
           sizeof(Pos) - POS_BB_BYTES);
    printf("mosse medie: %.1f pseudo-legali, %.1f legali   mmap: %u byte\n",
           (double)pseudo / NPOS, (double)legal / NPOS, TABLE_SIZE);

    int bad = crosscheck();
    printf("verifica incrociata (pseudo, 6 legali, attacchi su 64 case x 2 lati): %s\n\n",
           bad ? "DIFFERENZE TROVATE" : "identici");
    if (bad)
        return 1;

#ifdef COUNT_READS
    printf("== letture per posizione ==\n");
    printf("  %-24s %8s %8s %8s %8s %8s\n", "", "board", "key", "mmap", "bitboard", "geometria");
#define READS(label, expr)                                                       \
    do {                                                                         \
        READS_BOARD = READS_KEY = READS_TABLE = READS_GEOM = READS_BB = 0;       \
        expr;                                                                    \
        printf("  %-24s %8.1f %8.1f %8.1f %8.1f %8.1f\n", label,                  \
               (double)READS_BOARD / NPOS, (double)READS_KEY / NPOS,             \
               (double)READS_TABLE / NPOS, (double)READS_BB / NPOS,              \
               (double)READS_GEOM / NPOS);                                       \
    } while (0)
    READS("pseudo mmap", b_pseudo_mmap());
    READS("pseudo classico", b_pseudo_if());
    READS("pseudo bitboard", b_pseudo_bb());
    READS("attacco re mmap", b_king_mmap());
    READS("attacco re classico", b_king_if());
    READS("attacco re bitboard", b_king_bb());
    READS("legali mmap", b_legal_mmap());
    READS("legali classico", b_legal_if());
    READS("legali bitboard gen.", b_legal_bb());
    READS("legali stockfish", b_legal_sf());
    return 0;
#endif

    const Case pseudo_c[] = { { "mmap", b_pseudo_mmap }, { "classico IF", b_pseudo_if },
                              { "bitboard", b_pseudo_bb } };
    const Case king_c[] = { { "mmap", b_king_mmap }, { "classico IF", b_king_if },
                            { "bitboard", b_king_bb } };
    const Case att_c[] = { { "mmap", b_att_mmap }, { "classico IF", b_att_if },
                           { "bitboard", b_att_bb } };
    const Case legal_c[] = { { "mmap", b_legal_mmap }, { "classico IF", b_legal_if },
                             { "bitboard (filtro generico)", b_legal_bb },
                             { "bitboard stockfish", b_legal_sf } };
    const Case own_c[] = { { "mmap (scacch.+chiavi)", b_legal_mmap_own },
                           { "classico IF (scacch.)", b_legal_if_board },
                           { "stockfish (scacch.+bb)", b_legal_sf } };
    const Case make_c[] = { { "tutto lo stato", b_make_all }, { "scacchiera + chiavi", b_make_keys },
                            { "scacchiera + bitboard", b_make_bb },
                            { "solo scacchiera", b_make_board } };

    measure("generazione pseudo-legale (a parita' di stato)", pseudo_c, 3, rounds, "pos", NPOS);
    measure("test d'attacco sulla casa del re", king_c, 3, rounds, "query", NPOS);
    measure("test d'attacco su tutte le 64 case", att_c, 3, rounds, "query", NPOS * 64ull);
    measure("generazione legale (a parita' di stato: copia+make dell'intera Pos)", legal_c, 4,
            rounds, "pos", NPOS);
    measure("costo dello stato: copia + make per mossa", make_c, 4, rounds, "mossa", legal);
    measure("generazione legale, ognuno con il SOLO stato proprio", own_c, 3, rounds, "pos", NPOS);

    int pr = rounds < 5 ? rounds : 5;
    const char *pn1[] = { "mmap", "classico IF", "bitboard gen.", "stockfish" };
    const PerftFn pf1[] = { perft_mmap, perft_if, perft_bb, perft_sf };
    run_perft("perft a parita' di stato (intera Pos copiata e aggiornata)", pn1, pf1, 4, pr);
    const char *pn2[] = { "mmap", "classico IF", "stockfish" };
    const PerftFn pf2[] = { perft_mmap_own, perft_if_own, perft_sf_own };
    run_perft("perft con il solo stato proprio di ciascun motore", pn2, pf2, 3, pr);
    return 0;
}
