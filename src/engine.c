/*
 * Motore: mantiene la serializzazione della posizione (le chiavi) e
 * interroga la mmap. Il ciclo di generazione non conosce le regole:
 * legge chiavi, legge maschere, emette mosse.
 */
#include "mmap.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const uint8_t *MM;

static const uint8_t DIGIT[NPIECES] = { 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2 };
static const uint8_t PROMO[2][2][4] = {
    { { EMPTY }, { WQ, WR, WB, WN } },
    { { EMPTY }, { BQ, BR, BB, BN } },
};
static const int NPROMO[2] = { 1, 4 };
static uint8_t RMASK[64];

int mmap_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        perror(path);
        return -1;
    }
    const uint8_t *p = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    const MMapHeader *h = (const MMapHeader *)p;
    if ((size_t)st.st_size < sizeof *h || memcmp(h->magic, MMAP_MAGIC, 8) ||
        h->size != TABLE_SIZE || (size_t)st.st_size != sizeof *h + h->size) {
        fprintf(stderr, "%s: file non valido o generato da una geometria diversa\n", path);
        return -1;
    }
    MM = p + sizeof *h;
    return 0;
}

void engine_init(void)
{
    geometry_init();
    memset(RMASK, 0xF, sizeof RMASK);
    RMASK[4] = 0xC;  RMASK[7] = 0xE;  RMASK[0] = 0xD;
    RMASK[60] = 0x3; RMASK[63] = 0xB; RMASK[56] = 0x7;
}

/* ---- serializzazione incrementale ---- */

static inline void cell_set(Pos *p, int c, int from, int to)
{
    int d = to - from;
    for (int i = 0; i < NMEMB[c]; i++)
        p->key[MEMB[c][i].key] += d * MEMB[c][i].w;
}

/* Quali sezioni dello stato aggiornare: costanti a tempo di compilazione,
   ogni variante di make_move ottiene il proprio codice specializzato. */
enum { ST_KEYS = 1, ST_BB = 2, ST_ALL = 3 };

static const uint8_t COLOR[NPIECES] = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1 };

static inline __attribute__((always_inline)) void put_(Pos *p, int sq, int pc, const int st)
{
    int old = p->board[sq], o = DIGIT[old], n = DIGIT[pc];
    p->board[sq] = pc;
    if ((st & ST_BB) && old != pc) {
        uint64_t b = 1ull << sq;
        if (old) {
            p->by_type[0] ^= b;
            p->by_type[TYPE(old)] ^= b;
            p->by_color[COLOR[old]] ^= b;
        }
        if (pc) {
            p->by_type[0] ^= b;
            p->by_type[TYPE(pc)] ^= b;
            p->by_color[COLOR[pc]] ^= b;
        }
    }
    if ((st & ST_KEYS) && o != n) {
        cell_set(p, sq, o, n);
        cell_set(p, CAP(sq), o, n);
    }
}

static void put(Pos *p, int sq, int pc) { put_(p, sq, pc, ST_ALL); }

static inline __attribute__((always_inline)) void set_rights_(Pos *p, int rights, const int st)
{
    if (st & ST_KEYS)
        for (int r = 0; r < 4; r++) {
            int was = !(p->rights >> r & 1), now = !(rights >> r & 1);
            if (was != now)
                cell_set(p, RIGHT(r), was, now);
        }
    p->rights = rights;
}

static void set_rights(Pos *p, int rights) { set_rights_(p, rights, ST_ALL); }

/* ---- il ciclo unico ---- */

int gen_pseudo(const Pos *p, Move *out)
{
    int n = 0, s = side_to_move(p);
    for (int sq = 0; sq < 64; sq++) {
        VList vl = GV[s][piece_at(p, sq)][sq];
        COUNT(READS_GEOM);
        for (const View *v = &VIEWS[vl.first], *e = v + vl.n; v < e; v++) {
            COUNT(READS_GEOM);
            unsigned m = mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos);
            while (m) {
                int b = __builtin_ctz(m);
                m &= m - 1;
                for (int i = 0; i < NPROMO[v->promo]; i++)
                    n = emit(out, n, sq, v->to[b], PROMO[s][v->promo][i]);
            }
        }
    }
    return n;
}

int attacked(const Pos *p, int sq, int us)
{
    VList vl = AV[us][sq];
    COUNT(READS_GEOM);
    for (const View *v = &VIEWS[vl.first], *e = v + vl.n; v < e; v++) {
        COUNT(READS_GEOM);
        unsigned m = mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos);
        while (m) {
            int b = __builtin_ctz(m);
            m &= m - 1;
            if (v->attackers >> piece_at(p, v->to[b]) & 1)
                return 1;
        }
    }
    return 0;
}

static inline __attribute__((always_inline)) void make_(Pos *p, Move m, const int st)
{
    int us = p->side, pc = p->board[m.from], t = TYPE(pc), g = p->ghost;

    if (g != NO_SQ) {
        if (st & ST_KEYS)
            cell_set(p, CAP(g), (us ^ 1) + 1, 0);
        p->ghost = NO_SQ;
        if (t == PAWN && m.to == g)
            put_(p, g + (us == WHITE ? -8 : 8), EMPTY, st);
    }

    put_(p, m.from, EMPTY, st);
    put_(p, m.to, m.promo ? m.promo : pc, st);

    if (t == KING) {
        p->ksq[us] = m.to;
        if (m.to - m.from == 2) {
            put_(p, m.from + 1, p->board[m.from + 3], st);
            put_(p, m.from + 3, EMPTY, st);
        } else if (m.from - m.to == 2) {
            put_(p, m.from - 1, p->board[m.from - 4], st);
            put_(p, m.from - 4, EMPTY, st);
        }
    }

    set_rights_(p, p->rights & RMASK[m.from] & RMASK[m.to], st);

    if (t == PAWN && (m.to - m.from == 16 || m.from - m.to == 16)) {
        p->ghost = (m.from + m.to) / 2;
        if (st & ST_KEYS)
            cell_set(p, CAP(p->ghost), 0, us + 1);
    }

    p->side ^= 1;
}

void make_move(Pos *p, Move m) { make_(p, m, ST_ALL); }
void make_move_board(Pos *p, Move m) { make_(p, m, 0); }
void make_move_bb(Pos *p, Move m) { make_(p, m, ST_BB); }
void make_move_keys(Pos *p, Move m) { make_(p, m, ST_KEYS); }

DEFINE_GEN_LEGAL(gen_legal, gen_pseudo, attacked, make_move, pos_copy)
DEFINE_GEN_LEGAL(gen_legal_own, gen_pseudo, attacked, make_move_keys, pos_copy_keys)

/* ---- FEN e stampa ---- */

int pos_from_fen(Pos *p, const char *fen)
{
    static const char *PCS = ".PNBRQKpnbrqk";
    memset(p, 0, sizeof *p);
    p->ghost = NO_SQ;
    p->rights = 0xF;

    int sq = 56;
    for (; *fen && *fen != ' '; fen++) {
        if (*fen == '/')
            sq -= 16;
        else if (isdigit((unsigned char)*fen))
            sq += *fen - '0';
        else {
            const char *c = strchr(PCS, *fen);
            if (!c || sq < 0 || sq > 63)
                return -1;
            int pc = c - PCS;
            put(p, sq, pc);
            if (pc == WK || pc == BK)
                p->ksq[pc == BK] = sq;
            sq++;
        }
    }
    while (*fen == ' ')
        fen++;
    p->side = *fen == 'b' ? BLACK : WHITE;
    while (*fen && *fen != ' ')
        fen++;
    while (*fen == ' ')
        fen++;

    int rights = 0;
    for (; *fen && *fen != ' '; fen++)
        rights |= *fen == 'K' ? 1 : *fen == 'Q' ? 2 : *fen == 'k' ? 4 : *fen == 'q' ? 8 : 0;
    set_rights(p, rights);
    while (*fen == ' ')
        fen++;

    if (fen[0] >= 'a' && fen[0] <= 'h' && fen[1] >= '1' && fen[1] <= '8') {
        p->ghost = (fen[1] - '1') * 8 + fen[0] - 'a';
        cell_set(p, CAP(p->ghost), 0, (p->side ^ 1) + 1);
    }
    return 0;
}

void move_str(Move m, char *buf)
{
    buf[0] = 'a' + (m.from & 7);
    buf[1] = '1' + (m.from >> 3);
    buf[2] = 'a' + (m.to & 7);
    buf[3] = '1' + (m.to >> 3);
    buf[4] = m.promo ? "  nbrq"[TYPE(m.promo)] : 0;
    buf[5] = 0;
}
