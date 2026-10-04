/*
 * Generatore classico, termine di paragone per il benchmark.
 *
 * E' il modello "mappa classica + IF": le destinazioni su scacchiera vuota
 * sono precalcolate in array (salti di cavallo e re, raggi per direzione),
 * poi un algoritmo decide caso per caso se la casa e' vuota, propria o
 * nemica, se il raggio si ferma, se en passant e arrocco sono possibili.
 *
 * Legge lo stato SOLO tramite le primitive di mmap.h (piece_at,
 * side_to_move, ep_square, castle_rights, king_square) e scrive le mosse
 * con emit(): stessa struttura Pos, stesse primitive del generatore mmap.
 */
#include "mmap.h"

static uint8_t NT[64][8], NN[64];       /* salti di cavallo */
static uint8_t KT[64][8], KN[64];       /* passi di re */
static uint8_t RAY[64][8][7], RLEN[64][8]; /* 0..3 ortogonali, 4..7 diagonali */

static const int DIRS[8][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 },
                                { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
static const int NJ[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 },
                              { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };

static int inside(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

void classic_init(void)
{
    for (int sq = 0; sq < 64; sq++) {
        int f = sq & 7, r = sq >> 3;
        for (int i = 0; i < 8; i++) {
            if (inside(f + NJ[i][0], r + NJ[i][1]))
                NT[sq][NN[sq]++] = (r + NJ[i][1]) * 8 + f + NJ[i][0];
            if (inside(f + DIRS[i][0], r + DIRS[i][1]))
                KT[sq][KN[sq]++] = (r + DIRS[i][1]) * 8 + f + DIRS[i][0];
            int x = f + DIRS[i][0], y = r + DIRS[i][1];
            for (; inside(x, y); x += DIRS[i][0], y += DIRS[i][1])
                RAY[sq][i][RLEN[sq][i]++] = y * 8 + x;
        }
    }
}

static inline int is_own(int pc, int s) { return pc != EMPTY && (pc >= BP) == s; }
static inline int is_enemy(int pc, int s) { return pc != EMPTY && (pc >= BP) != s; }

static inline int pawn_to(Move *out, int n, int from, int to, int s, int promo)
{
    if (promo) {
        int q = s == WHITE ? WQ : BQ;
        n = emit(out, n, from, to, q);
        n = emit(out, n, from, to, q - 1);
        n = emit(out, n, from, to, q - 2);
        return emit(out, n, from, to, q - 3);
    }
    return emit(out, n, from, to, EMPTY);
}

static inline int slide(const Pos *p, Move *out, int n, int sq, int d0, int d1, int s)
{
    for (int d = d0; d < d1; d++)
        for (int i = 0; i < RLEN[sq][d]; i++) {
            COUNT(READS_GEOM);
            int t = RAY[sq][d][i], q = piece_at(p, t);
            if (q == EMPTY) {
                n = emit(out, n, sq, t, EMPTY);
                continue;
            }
            if (is_enemy(q, s))
                n = emit(out, n, sq, t, EMPTY);
            break;
        }
    return n;
}

static inline int leap(const Pos *p, Move *out, int n, int sq, const uint8_t *to, int cnt, int s)
{
    for (int i = 0; i < cnt; i++, COUNT(READS_GEOM))
        if (!is_own(piece_at(p, to[i]), s))
            n = emit(out, n, sq, to[i], EMPTY);
    return n;
}

int gen_pseudo_if(const Pos *p, Move *out)
{
    int n = 0, s = side_to_move(p);
    for (int sq = 0; sq < 64; sq++) {
        int pc = piece_at(p, sq);
        if (!is_own(pc, s))
            continue;
        switch (TYPE(pc)) {
        case PAWN: {
            int dir = s == WHITE ? 8 : -8, r = sq >> 3, f = sq & 7;
            int start = s == WHITE ? 1 : 6, promo = r == (s == WHITE ? 6 : 1);
            int ep = ep_square(p), t = sq + dir;
            if (piece_at(p, t) == EMPTY) {
                n = pawn_to(out, n, sq, t, s, promo);
                if (r == start && piece_at(p, t + dir) == EMPTY)
                    n = emit(out, n, sq, t + dir, EMPTY);
            }
            if (f > 0 && (is_enemy(piece_at(p, t - 1), s) || t - 1 == ep))
                n = pawn_to(out, n, sq, t - 1, s, promo);
            if (f < 7 && (is_enemy(piece_at(p, t + 1), s) || t + 1 == ep))
                n = pawn_to(out, n, sq, t + 1, s, promo);
            break;
        }
        case KNIGHT:
            n = leap(p, out, n, sq, NT[sq], NN[sq], s);
            break;
        case BISHOP:
            n = slide(p, out, n, sq, 4, 8, s);
            break;
        case ROOK:
            n = slide(p, out, n, sq, 0, 4, s);
            break;
        case QUEEN:
            n = slide(p, out, n, sq, 0, 8, s);
            break;
        case KING: {
            n = leap(p, out, n, sq, KT[sq], KN[sq], s);
            int cr = castle_rights(p) >> (2 * s), b = 56 * s;
            if (sq == b + 4) {
                if ((cr & 1) && piece_at(p, b + 5) == EMPTY && piece_at(p, b + 6) == EMPTY)
                    n = emit(out, n, sq, b + 6, EMPTY);
                if ((cr & 2) && piece_at(p, b + 3) == EMPTY && piece_at(p, b + 2) == EMPTY &&
                    piece_at(p, b + 1) == EMPTY)
                    n = emit(out, n, sq, b + 2, EMPTY);
            }
            break;
        }
        }
    }
    return n;
}

int attacked_if(const Pos *p, int sq, int us)
{
    int e = us == WHITE ? 6 : 0, f = sq & 7, r = sq >> 3;

    /* pedoni nemici: stanno "davanti" a sq dal punto di vista di us */
    int pr = us == WHITE ? r + 1 : r - 1, ep = WP + e;
    if (pr >= 0 && pr < 8) {
        if (f > 0 && piece_at(p, pr * 8 + f - 1) == ep)
            return 1;
        if (f < 7 && piece_at(p, pr * 8 + f + 1) == ep)
            return 1;
    }
    for (int i = 0; i < NN[sq]; i++, COUNT(READS_GEOM))
        if (piece_at(p, NT[sq][i]) == WN + e)
            return 1;
    for (int i = 0; i < KN[sq]; i++, COUNT(READS_GEOM))
        if (piece_at(p, KT[sq][i]) == WK + e)
            return 1;
    for (int d = 0; d < 8; d++) {
        int a = d < 4 ? WR + e : WB + e;
        for (int i = 0; i < RLEN[sq][d]; i++) {
            COUNT(READS_GEOM);
            int q = piece_at(p, RAY[sq][d][i]);
            if (q == EMPTY)
                continue;
            if (q == a || q == WQ + e)
                return 1;
            break;
        }
    }
    return 0;
}

DEFINE_GEN_LEGAL(gen_legal_if, gen_pseudo_if, attacked_if, make_move, pos_copy)
DEFINE_GEN_LEGAL(gen_legal_if_board, gen_pseudo_if, attacked_if, make_move_board, pos_copy_board)
