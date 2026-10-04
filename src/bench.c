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
#define _GNU_SOURCE /* syscall() per perf_event_open */
#include "mmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

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
    return (int)*x - (int)*y;
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
        int (*pseudo[])(const Pos *, Move *) = { gen_pseudo_flat, gen_pseudo_shuf, gen_pseudo_gather };
        for (size_t k = 0; k < sizeof pseudo / sizeof *pseudo; k++) {
            na = gen_pseudo(p, a);
            bad += !same(a, na, x, pseudo[k](p, x));
        }
        int (*legal[])(const Pos *, Move *) = { gen_legal_if, gen_legal_if_board, gen_legal_own,
                                                gen_legal_bb, gen_legal_sf, gen_legal_int,
                                                gen_legal_flat, gen_legal_shuf, gen_legal_gather,
                                                gen_legal_hyb, gen_legal_hyb_pdep };
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
                c += q.board[MV_TO(MOVES[k])];                                       \
            }                                                                    \
        return c;                                                                \
    }

/* Regime "figli": ogni posizione del set genera i suoi figli con make su una
   copia locale (stato caldo in L1, posizioni mai ripetute, come in una
   ricerca vera). Il costo di copia+make e' identico per tutti e viene
   sottratto misurando lo stesso ciclo senza generazione (b_child_none). */
static int child_none(const Pos *p, Move *out) { (void)out; return p->side; }

#define BENCH_CHILD(name, fn)                                                    \
    static uint64_t name(void)                                                   \
    {                                                                            \
        Move mv[512];                                                            \
        uint64_t c = 0;                                                          \
        for (int i = 0; i < NPOS; i++)                                           \
            for (int k = MOVE_OFF[i]; k < MOVE_OFF[i + 1]; k++) {                \
                Pos q;                                                           \
                pos_copy(&q, &POS[i]);                                           \
                make_move(&q, MOVES[k]);                                         \
                c += fn(&q, mv);                                                 \
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
BENCH_GEN(b_pseudo_flat, gen_pseudo_flat)
BENCH_GEN(b_pseudo_shuf, gen_pseudo_shuf)
BENCH_GEN(b_pseudo_gather, gen_pseudo_gather)
BENCH_GEN(b_legal_mmap, gen_legal)
BENCH_GEN(b_legal_if, gen_legal_if)
BENCH_GEN(b_legal_bb, gen_legal_bb)
BENCH_GEN(b_legal_sf, gen_legal_sf)
BENCH_GEN(b_legal_int, gen_legal_int)
BENCH_GEN(b_legal_flat, gen_legal_flat)
BENCH_GEN(b_legal_shuf, gen_legal_shuf)
BENCH_GEN(b_legal_gather, gen_legal_gather)
BENCH_GEN(b_legal_hyb, gen_legal_hyb)
BENCH_GEN(b_legal_hyb_pdep, gen_legal_hyb_pdep)
BENCH_GEN(b_hyb_int, hyb_phase_int)
BENCH_GEN(b_hyb_king, hyb_phase_king)
BENCH_GEN(b_mm_int, mm_phase_int)
BENCH_GEN(b_mm_collect, mm_phase_collect)
BENCH_GEN(b_mm_gen, mm_phase_gen)
BENCH_GEN(b_mm_king, mm_phase_king)
BENCH_GEN(b_sf_int, sf_phase_int)
BENCH_GEN(b_sf_pawn, sf_phase_pawn)
BENCH_GEN(b_sf_piece, sf_phase_piece)
BENCH_GEN(b_sf_king, sf_phase_king)
BENCH_GEN(b_legal_mmap_own, gen_legal_own)
BENCH_GEN(b_legal_if_board, gen_legal_if_board)
BENCH_ATT(b_att_mmap, attacked)
BENCH_ATT(b_att_if, attacked_if)
BENCH_ATT(b_att_bb, attacked_bb)
BENCH_KING(b_king_mmap, attacked)
BENCH_KING(b_king_if, attacked_if)
BENCH_KING(b_king_bb, attacked_bb)
BENCH_CHILD(b_child_none, child_none)
BENCH_CHILD(b_child_pseudo_bb, gen_pseudo_bb)
BENCH_CHILD(b_child_pseudo_flat, gen_pseudo_flat)
BENCH_CHILD(b_child_pseudo_shuf, gen_pseudo_shuf)
BENCH_CHILD(b_child_legal_sf, gen_legal_sf)
BENCH_CHILD(b_child_legal_flat, gen_legal_flat)
BENCH_CHILD(b_child_legal_shuf, gen_legal_shuf)
BENCH_CHILD(b_child_legal_hyb, gen_legal_hyb)
BENCH_CHILD(b_child_legal_hyb_pdep, gen_legal_hyb_pdep)
BENCH_MAKE(b_make_all, make_move, pos_copy)
BENCH_MAKE(b_make_keys, make_move_keys, pos_copy_keys)
BENCH_MAKE(b_make_bb, make_move_bb, pos_copy_bb)
BENCH_MAKE(b_make_board, make_move_board, pos_copy_board)
/* a parita' di stato: tutti copiano e aggiornano l'intera Pos */
DEFINE_PERFT(perft_mmap, gen_legal, make_move, pos_copy)
DEFINE_PERFT(perft_if, gen_legal_if, make_move, pos_copy)
DEFINE_PERFT(perft_bb, gen_legal_bb, make_move, pos_copy)
DEFINE_PERFT(perft_sf, gen_legal_sf, make_move, pos_copy)
DEFINE_PERFT(perft_int, gen_legal_int, make_move, pos_copy)
DEFINE_PERFT(perft_flat, gen_legal_flat, make_move, pos_copy)
/* stato proprio: ognuno copia e aggiorna solo cio' che legge */
DEFINE_PERFT(perft_mmap_own, gen_legal_own, make_move_keys, pos_copy_keys)
DEFINE_PERFT(perft_if_own, gen_legal_if_board, make_move_board, pos_copy_board)
DEFINE_PERFT(perft_sf_own, gen_legal_sf, make_move_bb, pos_copy_bb)
DEFINE_PERFT(perft_int_own, gen_legal_int, make_move_keys, pos_copy_keys)
DEFINE_PERFT(perft_flat_own, gen_legal_flat, make_move_keys, pos_copy_keys)
DEFINE_PERFT(perft_shuf_own, gen_legal_shuf, make_move_keys, pos_copy_keys)
DEFINE_PERFT(perft_gather_own, gen_legal_gather, make_move_keys, pos_copy_keys)
DEFINE_PERFT(perft_hyb_own, gen_legal_hyb, make_move, pos_copy)


#define MAXCASES 16

static uint64_t b_child_none(void);

/* ---- contatori hardware (perf_event_open, solo spazio utente) ----
 * CPU ibrida: i contatori sono aperti sulla PMU dei core P (cpu_core) e il
 * benchmark va fissato su un core P con taskset. */
enum { HW_INSTR, HW_CYCLES, HW_BRANCH, HW_MISS, NHW };
static int perf_fd[NHW] = { -1, -1, -1, -1 };

static void perf_init(void)
{
    static const uint64_t cfg[NHW] = { PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CPU_CYCLES,
                                       PERF_COUNT_HW_BRANCH_INSTRUCTIONS,
                                       PERF_COUNT_HW_BRANCH_MISSES };
    uint64_t type = 0;
    FILE *f = fopen("/sys/bus/event_source/devices/cpu_core/type", "r");
    if (f) {
        if (fscanf(f, "%lu", &type) != 1)
            type = 0;
        fclose(f);
    }
    for (int i = 0; i < NHW; i++) {
        struct perf_event_attr a;
        memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = PERF_TYPE_HARDWARE;
        a.config = cfg[i] | type << 32;
        a.disabled = i == 0;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        a.read_format = PERF_FORMAT_GROUP;
        perf_fd[i] = syscall(SYS_perf_event_open, &a, 0, -1, i ? perf_fd[0] : -1, 0);
        if (perf_fd[i] < 0) {
            fprintf(stderr, "contatori hardware non disponibili (perf_event_paranoid > 2?)\n");
            perf_fd[0] = -1;
            return;
        }
    }
}

static int perf_count(uint64_t (*fn)(void), int reps, uint64_t out[NHW])
{
    if (perf_fd[0] < 0)
        return 0;
    uint64_t buf[1 + NHW];
    fn(); /* riscaldamento: cache e predittore nello stato del caso misurato */
    ioctl(perf_fd[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(perf_fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    for (int r = 0; r < reps; r++)
        fn();
    ioctl(perf_fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    if (read(perf_fd[0], buf, sizeof buf) != sizeof buf)
        return 0;
    memcpy(out, buf + 1, sizeof buf - sizeof *buf);
    return 1;
}

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

typedef struct {
    const char *name;
    uint64_t (*fn)(void);
} Case;

/* I casi vengono misurati a round con ordine ruotato (ABC, BCA, CAB ...)
   per non regalare a nessuno cache calda o frequenza della CPU.
   Il primo caso e' il riferimento dei rapporti. Poi, per ogni caso, un
   passaggio con i contatori hardware (minimo su 3). Se moves != 0, check
   e' il numero di mosse prodotte: si stampa anche il throughput in bit
   utili (16 per mossa) per ciclo. */
static void measure_(const char *title, const Case *c, int nc, int rounds, const char *unit,
                     uint64_t units, int moves)
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
    int hw = perf_fd[0] >= 0;
    printf("  %-28s %9s %6s", "", "ns/" , "");
    if (hw)
        printf(" %9s %8s %5s %8s %8s%s", "istr.", "cicli", "IPC", "salti", "errati", moves ? "  bit/ciclo" : "");
    printf("\n");
    for (int k = 0; k < nc; k++) {
        qsort(t[k], rounds, sizeof(double), cmp_d);
        double med = t[k][rounds / 2];
        if (!k)
            med0 = med;
        printf("  %-28s %9.1f %5.2fx", c[k].name, med / units * 1e9, med / med0);
        /* passaggio contato lungo almeno ~16k posizioni: niente effetti di bordo */
        int reps = NPOS < 16384 ? (16384 + NPOS - 1) / NPOS : 1;
        uint64_t h[NHW], best[NHW] = { 0 };
        for (int r = 0; hw && r < 3; r++)
            if (perf_count(c[k].fn, reps, h) && (!best[HW_CYCLES] || h[HW_CYCLES] < best[HW_CYCLES]))
                memcpy(best, h, sizeof h);
        if (hw && best[HW_CYCLES]) {
            double u = (double)units * reps;
            printf(" %9.1f %8.1f %5.2f %8.1f %8.2f", best[HW_INSTR] / u, best[HW_CYCLES] / u,
                   (double)best[HW_INSTR] / best[HW_CYCLES], best[HW_BRANCH] / u, best[HW_MISS] / u);
            if (moves)
                printf("  %9.2f", check[k] * reps * 16.0 / best[HW_CYCLES]);
        }
        int base_row = c[0].fn == b_child_none; /* la prima riga e' la base, non un generatore */
        printf("%s\n", check[k] != check[0] && !base_row && !getenv("BREAKDOWN_ONLY")
                           ? "  RISULTATO DIVERSO!" : "");
    }
    printf("  (per %s; rapporto = mediana / mediana della prima riga)\n\n", unit);
}

#define measure(title, c, nc, rounds, unit, units) measure_(title, c, nc, rounds, unit, units, 0)
#define measure_moves(title, c, nc, rounds, unit, units) measure_(title, c, nc, rounds, unit, units, 1)

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
    perf_init();
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
    printf("verifica incrociata (5 pseudo, 12 legali, attacchi su 64 case x 2 lati): %s\n\n",
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
    READS("pseudo mmap piatto", b_pseudo_flat());
    READS("attacco re mmap", b_king_mmap());
    READS("attacco re classico", b_king_if());
    READS("attacco re bitboard", b_king_bb());
    READS("legali mmap", b_legal_mmap());
    READS("legali classico", b_legal_if());
    READS("legali bitboard gen.", b_legal_bb());
    READS("legali stockfish", b_legal_sf());
    READS("legali mmap interferenza", b_legal_int());
    READS("legali mmap interf. piatto", b_legal_flat());
    return 0;
#endif

    const Case pseudo_c[] = { { "mmap", b_pseudo_mmap }, { "classico IF", b_pseudo_if },
                              { "bitboard", b_pseudo_bb }, { "mmap piatto", b_pseudo_flat },
                              { "mmap pshufb", b_pseudo_shuf }, { "mmap gather+pshufb", b_pseudo_gather } };
    const Case king_c[] = { { "mmap", b_king_mmap }, { "classico IF", b_king_if },
                            { "bitboard", b_king_bb } };
    const Case att_c[] = { { "mmap", b_att_mmap }, { "classico IF", b_att_if },
                           { "bitboard", b_att_bb } };
    const Case legal_c[] = { { "mmap", b_legal_mmap }, { "classico IF", b_legal_if },
                             { "bitboard (filtro generico)", b_legal_bb },
                             { "bitboard stockfish", b_legal_sf },
                             { "mmap interferenza", b_legal_int },
                             { "mmap interf. piatto", b_legal_flat } };
    const Case own_c[] = { { "mmap (scacch.+chiavi)", b_legal_mmap_own },
                           { "classico IF (scacch.)", b_legal_if_board },
                           { "stockfish (scacch.+bb)", b_legal_sf },
                           { "mmap interf. (scacch.+chiavi)", b_legal_int },
                           { "mmap interf. piatto (idem)", b_legal_flat },
                           { "mmap interf. pshufb (idem)", b_legal_shuf },
                           { "mmap interf. gather (idem)", b_legal_gather },
                           { "ibrido (scacch.+bb+chiavi)", b_legal_hyb },
                           { "ibrido pdep (idem)", b_legal_hyb_pdep } };
    const Case child_c[] = { { "solo copia+make (base)", b_child_none },
                             { "pseudo bitboard", b_child_pseudo_bb },
                             { "pseudo mmap piatto", b_child_pseudo_flat },
                             { "pseudo mmap pshufb", b_child_pseudo_shuf },
                             { "legali stockfish", b_child_legal_sf },
                             { "legali mmap piatto", b_child_legal_flat },
                             { "legali mmap pshufb", b_child_legal_shuf },
                             { "legali ibrido", b_child_legal_hyb },
                             { "legali ibrido pdep", b_child_legal_hyb_pdep } };
    const Case make_c[] = { { "tutto lo stato", b_make_all }, { "scacchiera + chiavi", b_make_keys },
                            { "scacchiera + bitboard", b_make_bb },
                            { "solo scacchiera", b_make_board } };

    measure_moves("generazione pseudo-legale (a parita' di stato)", pseudo_c, 6, rounds, "pos", NPOS);
    measure("test d'attacco sulla casa del re", king_c, 3, rounds, "query", NPOS);
    measure("test d'attacco su tutte le 64 case", att_c, 3, rounds, "query", NPOS * 64ull);
    measure("generazione legale (a parita' di stato: copia+make dell'intera Pos)", legal_c, 6,
            rounds, "pos", NPOS);
    measure("costo dello stato: copia + make per mossa", make_c, 4, rounds, "mossa", legal);
    measure_moves("regime figli: stato caldo, posizioni mai ripetute (sottrarre la base)", child_c,
                  9, rounds, "figlio", legal);
    measure_moves("generazione legale, ognuno con il SOLO stato proprio", own_c, 9, rounds, "pos", NPOS);

    const Case brk_c[] = { { "mmap: tutto", b_legal_flat },
                           { "mmap: interferenze re", b_mm_int },
                           { "mmap: raccolta viste", b_mm_collect },
                           { "mmap: raccolta+emissione", b_mm_gen },
                           { "mmap: mosse del re", b_mm_king },
                           { "ibrido: tutto", b_legal_hyb },
                           { "ibrido: scacchi+inchiod.", b_hyb_int },
                           { "ibrido: mosse del re", b_hyb_king },
                           { "sf: tutto", b_legal_sf },
                           { "sf: scacchi+inchiodature", b_sf_int },
                           { "sf: pedoni", b_sf_pawn },
                           { "sf: pezzi N B R Q", b_sf_piece },
                           { "sf: re", b_sf_king } };
    measure("scomposizione della generazione legale (fasi isolate)", brk_c, 13, rounds, "pos",
            NPOS);
    if (getenv("BREAKDOWN_ONLY"))
        return 0;

    int pr = rounds < 5 ? rounds : 5;
    const char *pn1[] = { "mmap", "classico IF", "bitboard gen.", "stockfish", "mmap interf.",
                          "mmap piatto" };
    const PerftFn pf1[] = { perft_mmap, perft_if, perft_bb, perft_sf, perft_int, perft_flat };
    run_perft("perft a parita' di stato (intera Pos copiata e aggiornata)", pn1, pf1, 6, pr);
    const char *pn2[] = { "mmap", "classico IF", "stockfish", "mmap interf.", "mmap piatto",
                          "mmap pshufb", "mmap gather", "ibrido" };
    const PerftFn pf2[] = { perft_mmap_own, perft_if_own, perft_sf_own, perft_int_own,
                            perft_flat_own, perft_shuf_own, perft_gather_own, perft_hyb_own };
    run_perft("perft con il solo stato proprio di ciascun motore", pn2, pf2, 8, pr);
    return 0;
}
