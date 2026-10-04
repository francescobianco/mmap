/*
 * Tabelle di attacco bitboard e primitive inline, condivise dal generatore
 * stile Stockfish (bitboard.c) e dalla versione ibrida di mmap (engine.c).
 */
#ifndef BITBOARD_H
#define BITBOARD_H

#include "mmap.h"

#ifdef __BMI2__
#include <immintrin.h>
#endif

typedef uint64_t Bits;

#define FILE_A 0x0101010101010101ull
#define FILE_H (FILE_A << 7)
#define RANK_1 0xFFull
#define RANK_8 (RANK_1 << 56)
#define RANK_2 (RANK_1 << 8)
#define RANK_3 (RANK_1 << 16)
#define RANK_6 (RANK_1 << 40)
#define RANK_7 (RANK_1 << 48)

extern Bits KNIGHT_ATT[64], KING_ATT[64], PAWN_ATT[2][64];
extern Bits ROOK_PSEUDO[64], BISHOP_PSEUDO[64];
extern Bits ROOK_MASK[64], BISHOP_MASK[64];
extern uint32_t ROOK_OFF[64], BISHOP_OFF[64];
extern Bits ROOK_TAB[0x19000], BISHOP_TAB[0x1480];
extern Bits BETWEEN[64][64], LINE[64][64];

static inline Bits pext(Bits x, Bits m)
{
#ifdef __BMI2__
    return _pext_u64(x, m);
#else
    Bits r = 0;
    for (Bits bit = 1; m; m &= m - 1, bit <<= 1)
        if (x & m & -m)
            r |= bit;
    return r;
#endif
}

static inline int pop(Bits *b)
{
    int s = __builtin_ctzll(*b);
    *b &= *b - 1;
    return s;
}

static inline Bits pdep(Bits x, Bits m)
{
#ifdef __BMI2__
    return _pdep_u64(x, m);
#else
    Bits r = 0;
    for (Bits bit = 1; m; m &= m - 1, bit <<= 1)
        if (x & bit)
            r |= m & -m;
    return r;
#endif
}

static inline Bits rook_att(int sq, Bits occ)
{
    COUNT(READS_GEOM);
    return ROOK_TAB[ROOK_OFF[sq] + pext(occ, ROOK_MASK[sq])];
}

static inline Bits bishop_att(int sq, Bits occ)
{
    COUNT(READS_GEOM);
    return BISHOP_TAB[BISHOP_OFF[sq] + pext(occ, BISHOP_MASK[sq])];
}

static inline Bits attackers_to(const Pos *p, int sq, Bits occ)
{
    return (PAWN_ATT[BLACK][sq] & bb_pieces(p, WHITE, PAWN)) |
           (PAWN_ATT[WHITE][sq] & bb_pieces(p, BLACK, PAWN)) |
           (KNIGHT_ATT[sq] & bb_type(p, KNIGHT)) | (KING_ATT[sq] & bb_type(p, KING)) |
           (rook_att(sq, occ) & (bb_type(p, ROOK) | bb_type(p, QUEEN))) |
           (bishop_att(sq, occ) & (bb_type(p, BISHOP) | bb_type(p, QUEEN)));
}

#endif
