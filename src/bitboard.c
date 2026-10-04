/*
 * Generatore bitboard nello stile di Stockfish, terzo termine di paragone.
 *
 *   - attacchi degli slider con PEXT (come Stockfish con BMI2), tabelle
 *     di attacco per pedoni, cavalli e re;
 *   - pedoni generati in blocco con shift di bitboard;
 *   - generazione legale come Stockfish: si calcolano scacchi e pezzi
 *     inchiodati, in caso di scacco le destinazioni si restringono a
 *     "cattura o interponi", il re verifica le case con attackers_to,
 *     l'en passant con un test completo. Nessun make, nessuna copia.
 *
 * Legge lo stato tramite le primitive di mmap.h (bb_type, bb_color,
 * side_to_move, ep_square, castle_rights, king_square) ed emette con emit().
 */
#include "bitboard.h"

Bits KNIGHT_ATT[64], KING_ATT[64], PAWN_ATT[2][64];
Bits ROOK_PSEUDO[64], BISHOP_PSEUDO[64];
Bits ROOK_MASK[64], BISHOP_MASK[64];
uint32_t ROOK_OFF[64], BISHOP_OFF[64];
Bits ROOK_TAB[0x19000], BISHOP_TAB[0x1480];
Bits BETWEEN[64][64], LINE[64][64];

static const int RD[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
static const int BD[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

static Bits slide(int sq, Bits occ, const int (*d)[2])
{
    Bits a = 0;
    for (int i = 0; i < 4; i++)
        for (int f = (sq & 7) + d[i][0], r = (sq >> 3) + d[i][1];
             f >= 0 && f < 8 && r >= 0 && r < 8; f += d[i][0], r += d[i][1]) {
            a |= 1ull << (r * 8 + f);
            if (occ >> (r * 8 + f) & 1)
                break;
        }
    return a;
}

static void init_slider(Bits *mask, uint32_t *off, Bits *tab, Bits *pseudo, const int (*d)[2])
{
    uint32_t o = 0;
    for (int sq = 0; sq < 64; sq++) {
        Bits edges = ((RANK_1 | RANK_8) & ~(RANK_1 << (8 * (sq >> 3)))) |
                   ((FILE_A | FILE_H) & ~(FILE_A << (sq & 7)));
        pseudo[sq] = slide(sq, 0, d);
        mask[sq] = pseudo[sq] & ~edges;
        off[sq] = o;
        Bits occ = 0;
        do {
            tab[o + pext(occ, mask[sq])] = slide(sq, occ, d);
            occ = (occ - mask[sq]) & mask[sq];
        } while (occ);
        o += 1u << __builtin_popcountll(mask[sq]);
    }
}

void bitboard_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    static const int NJ[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 },
                                  { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };
    for (int sq = 0; sq < 64; sq++) {
        int f = sq & 7, r = sq >> 3;
        for (int i = 0; i < 8; i++) {
            int x = f + NJ[i][0], y = r + NJ[i][1];
            if (x >= 0 && x < 8 && y >= 0 && y < 8)
                KNIGHT_ATT[sq] |= 1ull << (y * 8 + x);
        }
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if ((dx || dy) && f + dx >= 0 && f + dx < 8 && r + dy >= 0 && r + dy < 8)
                    KING_ATT[sq] |= 1ull << ((r + dy) * 8 + f + dx);
        for (int c = 0; c < 2; c++) {
            int y = r + (c == WHITE ? 1 : -1);
            if (y < 0 || y > 7)
                continue;
            if (f > 0)
                PAWN_ATT[c][sq] |= 1ull << (y * 8 + f - 1);
            if (f < 7)
                PAWN_ATT[c][sq] |= 1ull << (y * 8 + f + 1);
        }
    }
    init_slider(ROOK_MASK, ROOK_OFF, ROOK_TAB, ROOK_PSEUDO, RD);
    init_slider(BISHOP_MASK, BISHOP_OFF, BISHOP_TAB, BISHOP_PSEUDO, BD);

    for (int a = 0; a < 64; a++)
        for (int b = 0; b < 64; b++) {
            Bits ba = 1ull << a, bb = 1ull << b;
            if (a != b && (ROOK_PSEUDO[a] & bb)) {
                BETWEEN[a][b] = slide(a, bb, RD) & slide(b, ba, RD);
                LINE[a][b] = (ROOK_PSEUDO[a] & ROOK_PSEUDO[b]) | ba | bb;
            } else if (a != b && (BISHOP_PSEUDO[a] & bb)) {
                BETWEEN[a][b] = slide(a, bb, BD) & slide(b, ba, BD);
                LINE[a][b] = (BISHOP_PSEUDO[a] & BISHOP_PSEUDO[b]) | ba | bb;
            }
        }
}

int attacked_bb(const Pos *p, int sq, int us)
{
    return (attackers_to(p, sq, bb_type(p, 0)) & bb_color(p, us ^ 1)) != 0;
}

/* legal = 0: pseudo-legale (stesso insieme di mmap e classico).
   legal = 1: legale alla Stockfish. Costante: due funzioni specializzate. */
static inline __attribute__((always_inline)) int gen_(const Pos *p, Move *out, const int legal,
                                                      const int ph)
{
    int us = side_to_move(p), them = us ^ 1, n = 0, ksq = king_square(p, us);
    Bits occ = bb_type(p, 0), own = bb_color(p, us), enemy = bb_color(p, them);
    Bits target = ~own, pinned = 0, checkers = 0;

    if (legal && (ph & PH_INT)) {
        checkers = attackers_to(p, ksq, occ) & enemy;
        Bits snipers = ((ROOK_PSEUDO[ksq] & (bb_type(p, ROOK) | bb_type(p, QUEEN))) |
                      (BISHOP_PSEUDO[ksq] & (bb_type(p, BISHOP) | bb_type(p, QUEEN)))) &
                     enemy;
        while (snipers) {
            Bits b = BETWEEN[ksq][pop(&snipers)] & occ;
            if (b && !(b & (b - 1)) && (b & own))
                pinned |= b;
        }
        if (checkers & (checkers - 1))
            goto king; /* scacco doppio: muove solo il re */
        if (checkers)
            target = BETWEEN[ksq][__builtin_ctzll(checkers)] | checkers;
    }

#define OK(from, to) (!legal || !(pinned >> (from) & 1) || (LINE[from][ksq] >> (to) & 1))
#define ADD(from, to)                                                            \
    do {                                                                         \
        if (OK(from, to))                                                        \
            n = emit(out, n, from, to, EMPTY);                                   \
    } while (0)
#define ADD_PROMO(from, to)                                                      \
    do {                                                                         \
        if (OK(from, to)) {                                                      \
            int q = us == WHITE ? WQ : BQ;                                       \
            n = emit(out, n, from, to, q);                                       \
            n = emit(out, n, from, to, q - 1);                                   \
            n = emit(out, n, from, to, q - 2);                                   \
            n = emit(out, n, from, to, q - 3);                                   \
        }                                                                        \
    } while (0)
#define SH(b, d) ((d) > 0 ? (b) << (d) : (b) >> -(d))

    if (ph & PH_PAWN) {
        Bits pawns = bb_pieces(p, us, PAWN), empty = ~occ;
        Bits r7 = us == WHITE ? RANK_7 : RANK_2, r3 = us == WHITE ? RANK_3 : RANK_6;
        Bits p7 = pawns & r7, pn = pawns & ~r7, capt = enemy & target;
        int up = us == WHITE ? 8 : -8, dl = us == WHITE ? 7 : -9, dr = us == WHITE ? 9 : -7;

        Bits b1 = SH(pn, up) & empty, b2 = SH(b1 & r3, up) & empty & target;
        b1 &= target;
        while (b1) {
            int to = pop(&b1);
            ADD(to - up, to);
        }
        while (b2) {
            int to = pop(&b2);
            ADD(to - 2 * up, to);
        }
        Bits cl = SH(pn & ~FILE_A, dl) & capt, cr = SH(pn & ~FILE_H, dr) & capt;
        while (cl) {
            int to = pop(&cl);
            ADD(to - dl, to);
        }
        while (cr) {
            int to = pop(&cr);
            ADD(to - dr, to);
        }
        if (p7) {
            Bits pp = SH(p7, up) & empty & target;
            Bits pl = SH(p7 & ~FILE_A, dl) & capt, pr = SH(p7 & ~FILE_H, dr) & capt;
            while (pp) {
                int to = pop(&pp);
                ADD_PROMO(to - up, to);
            }
            while (pl) {
                int to = pop(&pl);
                ADD_PROMO(to - dl, to);
            }
            while (pr) {
                int to = pop(&pr);
                ADD_PROMO(to - dr, to);
            }
        }
        int ep = ep_square(p);
        if (ep != NO_SQ) {
            Bits att = PAWN_ATT[them][ep] & pn;
            while (att) {
                int from = pop(&att);
                if (legal) {
                    Bits cap = 1ull << (ep - up);
                    Bits o2 = (occ ^ (1ull << from) ^ cap) | (1ull << ep);
                    if (attackers_to(p, ksq, o2) & enemy & ~cap)
                        continue;
                }
                n = emit(out, n, from, ep, EMPTY);
            }
        }
    }

    for (int pt = KNIGHT; (ph & PH_PIECE) && pt <= QUEEN; pt++) {
        Bits b = bb_pieces(p, us, pt);
        while (b) {
            int from = pop(&b);
            Bits a = pt == KNIGHT   ? KNIGHT_ATT[from]
                   : pt == BISHOP ? bishop_att(from, occ)
                   : pt == ROOK   ? rook_att(from, occ)
                                  : rook_att(from, occ) | bishop_att(from, occ);
            a &= target;
            if (legal && (pinned >> from & 1))
                a &= LINE[from][ksq];
            while (a)
                n = emit(out, n, from, pop(&a), EMPTY);
        }
    }

king:
    if (ph & PH_KING) {
        Bits a = KING_ATT[ksq] & ~own;
        while (a) {
            int to = pop(&a);
            if (!legal || !(attackers_to(p, to, occ ^ (1ull << ksq)) & enemy))
                n = emit(out, n, ksq, to, EMPTY);
        }
        if (!legal || !checkers) {
            int cr = castle_rights(p) >> (2 * us), b = 56 * us;
            if ((cr & 1) && !(occ & (3ull << (b + 5))) &&
                (!legal || !((attackers_to(p, b + 5, occ) | attackers_to(p, b + 6, occ)) & enemy)))
                n = emit(out, n, ksq, b + 6, EMPTY);
            if ((cr & 2) && !(occ & (7ull << (b + 1))) &&
                (!legal || !((attackers_to(p, b + 3, occ) | attackers_to(p, b + 2, occ)) & enemy)))
                n = emit(out, n, ksq, b + 2, EMPTY);
        }
    }
    if (legal && ph == PH_INT) /* fase isolata: il risultato deve essere usato */
        n = (int)((checkers ^ pinned ^ target) & 1);
    return n;
#undef OK
#undef ADD
#undef ADD_PROMO
#undef SH
}

int gen_pseudo_bb(const Pos *p, Move *out) { return gen_(p, out, 0, PH_ALL); }
int gen_legal_sf(const Pos *p, Move *out) { return gen_(p, out, 1, PH_ALL); }
int sf_phase_int(const Pos *p, Move *out) { return gen_(p, out, 1, PH_INT); }
int sf_phase_pawn(const Pos *p, Move *out) { return gen_(p, out, 1, PH_PAWN); }
int sf_phase_piece(const Pos *p, Move *out) { return gen_(p, out, 1, PH_PIECE); }
int sf_phase_king(const Pos *p, Move *out) { return gen_(p, out, 1, PH_KING); }

DEFINE_GEN_LEGAL(gen_legal_bb, gen_pseudo_bb, attacked_bb, make_move, pos_copy)
