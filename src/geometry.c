/*
 * Geometria della scacchiera: costruisce le chiavi (sequenze di celle)
 * e le viste (quale pezzo su quale casa legge quale chiave).
 * E' deterministica: generatore della mmap e motore la ricalcolano
 * identica, cosi' gli offset puntano alle stesse righe della tabella.
 */
#include "mmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Key KEYS[MAX_KEYS];
int NKEYS;
Member MEMB[NCELLS][MAX_MEMB];
uint8_t NMEMB[NCELLS];
View VIEWS[MAX_VIEWS];
int NVIEWS;
VList GV[2][NPIECES][64];
VList AV[2][64];
VList GVL[2][NPIECES][64];
VList KL[2][64];
VList KR[2][64];
uint64_t KEYBB[MAX_KEYS];
uint8_t KEYDESC[MAX_KEYS];
uint8_t EXPAND[3][256];
uint32_t BASE[NSEM][2][MAXLEN + 1];
uint32_t TABLE_SIZE;
const uint32_t POW3[MAXLEN + 1] = { 1, 3, 9, 27, 81, 243, 729, 2187, 6561 };

static int line_key[64][4], line_pos[64][4];
static int knight_key[64], king_key[64];
static int push_key[2][64], capt_key[2][64];
static int castle_key[4];

static const int LDIR[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { -1, 1 } };
static const int NJUMP[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 },
                                 { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };
static const int KJUMP[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 },
                                 { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };

static int on_board(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

static int new_key(const int *cells, int n)
{
    if (NKEYS >= MAX_KEYS) {
        fprintf(stderr, "MAX_KEYS too small\n");
        exit(1);
    }
    int k = NKEYS++;
    KEYS[k].len = n;
    for (int i = 0, w = 1; i < n; i++, w *= 3) {
        int c = cells[i];
        KEYS[k].cell[i] = c;
        MEMB[c][NMEMB[c]++] = (Member){ k, w };
    }
    return k;
}

static int ring_key(int sq, const int (*jump)[2])
{
    int cells[8], n = 0, f = sq & 7, r = sq >> 3;
    for (int i = 0; i < 8; i++)
        if (on_board(f + jump[i][0], r + jump[i][1]))
            cells[n++] = (r + jump[i][1]) * 8 + f + jump[i][0];
    /* celle in ordine crescente di casa: pdep(maschera, KEYBB) da' il bitboard */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cells[j - 1] > cells[j]; j--) {
            int t = cells[j];
            cells[j] = cells[j - 1];
            cells[j - 1] = t;
        }
    return new_key(cells, n);
}

static void build_keys(void)
{
    /* traverse, colonne, diagonali, antidiagonali */
    for (int d = 0; d < 4; d++)
        for (int sq = 0; sq < 64; sq++) {
            int df = LDIR[d][0], dr = LDIR[d][1], f = sq & 7, r = sq >> 3;
            if (on_board(f - df, r - dr))
                continue; /* non e' l'inizio di una linea */
            int cells[8], n = 0;
            for (int x = f, y = r; on_board(x, y); x += df, y += dr)
                cells[n++] = y * 8 + x;
            int k = new_key(cells, n);
            for (int i = 0; i < n; i++) {
                line_key[cells[i]][d] = k;
                line_pos[cells[i]][d] = i;
            }
        }

    for (int sq = 0; sq < 64; sq++) {
        knight_key[sq] = ring_key(sq, NJUMP);
        king_key[sq] = ring_key(sq, KJUMP);
    }

    for (int s = 0; s < 2; s++)
        for (int sq = 0; sq < 64; sq++) {
            int dir = s == WHITE ? 1 : -1, f = sq & 7, r = sq >> 3;
            int cells[2], n;
            push_key[s][sq] = capt_key[s][sq] = -1;
            if (!on_board(f, r + dir))
                continue;
            if (r != 0 && r != 7) {
                n = 0;
                cells[n++] = sq + 8 * dir;
                if (r == (s == WHITE ? 1 : 6))
                    cells[n++] = sq + 16 * dir;
                push_key[s][sq] = new_key(cells, n);
            }
            n = 0;
            for (int df = -1; df <= 1; df += 2)
                if (on_board(f + df, r + dir))
                    cells[n++] = CAP((r + dir) * 8 + f + df);
            capt_key[s][sq] = new_key(cells, n);
        }

    /* arrocchi: case che devono essere vuote + cella virtuale del diritto */
    static const int C[4][5] = {
        { 3, 5, 6, RIGHT(0) },              /* bianco corto: f1 g1 */
        { 4, 3, 2, 1, RIGHT(1) },           /* bianco lungo: d1 c1 b1 */
        { 3, 61, 62, RIGHT(2) },            /* nero corto: f8 g8 */
        { 4, 59, 58, 57, RIGHT(3) },        /* nero lungo: d8 c8 b8 */
    };
    for (int i = 0; i < 4; i++)
        castle_key[i] = new_key(&C[i][1], C[i][0]);
}

static void build_bases(void)
{
    uint32_t off = 0;
    for (int sem = 0; sem < NSEM; sem++)
        for (int s = 0; s < 2; s++)
            for (int len = 1; len <= MAXLEN; len++) {
                BASE[sem][s][len] = off;
                off += POW3[len] * MAXLEN * ESIZE(sem);
            }
    TABLE_SIZE = off + 16; /* margine: le gather leggono 4 byte per voce */
}

static void add_view(VList *vl, int sem, int side, int key, int pos, int promo,
                     uint16_t attackers)
{
    if (NVIEWS >= MAX_VIEWS) {
        fprintf(stderr, "MAX_VIEWS too small\n");
        exit(1);
    }
    if (vl->n == 0)
        vl->first = NVIEWS;
    View *v = &VIEWS[NVIEWS++];
    vl->n++;
    v->base = BASE[sem][side][KEYS[key].len];
    v->key = key;
    v->pos = pos;
    v->promo = promo;
    v->attackers = attackers;
    v->sem = sem;
    v->cap = sem == SEM_CAPT && KEYS[key].len && KEYS[key].cell[0] >= 64 && KEYS[key].cell[0] < 128;
    for (int i = 0; i < KEYS[key].len; i++) {
        int c = KEYS[key].cell[i];
        v->to[i] = c < 64 ? c : c < 128 ? c - 64 : 0;
    }
}

static void add_lines(VList *vl, int sem, int side, int sq, int d0, int d1, uint16_t att)
{
    for (int d = d0; d <= d1; d++)
        add_view(vl, sem, side, line_key[sq][d], line_pos[sq][d], 0, att);
}

static void build_views(void)
{
    for (int s = 0; s < 2; s++)
        for (int pc = 1; pc < NPIECES; pc++) {
            if ((pc >= BP) != s)
                continue; /* i pezzi avversari restano senza viste */
            int t = (pc - 1) % 6 + 1;
            for (int sq = 0; sq < 64; sq++) {
                VList *vl = &GV[s][pc][sq];
                int r = sq >> 3;
                switch (t) {
                case PAWN:
                    if (push_key[s][sq] < 0)
                        break;
                    int promo = r == (s == WHITE ? 6 : 1);
                    add_view(vl, SEM_PUSH, s, push_key[s][sq], 0, promo, 0);
                    add_view(vl, SEM_CAPT, s, capt_key[s][sq], 0, promo, 0);
                    break;
                case KNIGHT:
                    add_view(vl, SEM_LEAP, s, knight_key[sq], 0, 0, 0);
                    break;
                case BISHOP:
                    add_lines(vl, SEM_SLIDE, s, sq, 2, 3, 0);
                    break;
                case ROOK:
                    add_lines(vl, SEM_SLIDE, s, sq, 0, 1, 0);
                    break;
                case QUEEN:
                    add_lines(vl, SEM_SLIDE, s, sq, 0, 3, 0);
                    break;
                case KING:
                    add_view(vl, SEM_LEAP, s, king_key[sq], 0, 0, 0);
                    if (sq == (s == WHITE ? 4 : 60)) {
                        add_view(vl, SEM_CASTLE, s, castle_key[2 * s], 0, 0, 0);
                        add_view(vl, SEM_CASTLE, s, castle_key[2 * s + 1], 0, 0, 0);
                    }
                    break;
                }
            }
        }

    /* liste per la generazione legale: identiche, ma il re e' gestito a parte */
    for (int s = 0; s < 2; s++)
        for (int pc = 1; pc < NPIECES; pc++)
            if (TYPE(pc) != KING)
                memcpy(GVL[s][pc], GV[s][pc], sizeof GV[s][pc]);

    /* viste di attacco: la casa guarda "come un super-pezzo" del lato us */
    for (int us = 0; us < 2; us++) {
        int e = us == WHITE ? 6 : 0; /* offset dei pezzi nemici */
#define BIT(t) (uint16_t)(1u << ((t) + e))
        for (int sq = 0; sq < 64; sq++) {
            VList *vl = &AV[us][sq];
            add_lines(vl, SEM_HIT, us, sq, 0, 1, BIT(ROOK) | BIT(QUEEN));
            add_lines(vl, SEM_HIT, us, sq, 2, 3, BIT(BISHOP) | BIT(QUEEN));
            add_view(vl, SEM_CAPT, us, knight_key[sq], 0, 0, BIT(KNIGHT));
            add_view(vl, SEM_CAPT, us, king_key[sq], 0, 0, BIT(KING));
            if (capt_key[us][sq] >= 0)
                add_view(vl, SEM_CAPT, us, capt_key[us][sq], 0, 0, BIT(PAWN));
        }
        /* viste del re: interferenze sulle 4 linee, scacchi di cavallo e pedone */
        for (int sq = 0; sq < 64; sq++) {
            add_lines(&KL[us][sq], SEM_KING, us, sq, 0, 1, BIT(ROOK) | BIT(QUEEN));
            add_lines(&KL[us][sq], SEM_KING, us, sq, 2, 3, BIT(BISHOP) | BIT(QUEEN));
            add_view(&KR[us][sq], SEM_CAPT, us, knight_key[sq], 0, 0, BIT(KNIGHT));
            if (capt_key[us][sq] >= 0)
                add_view(&KR[us][sq], SEM_CAPT, us, capt_key[us][sq], 0, 0, BIT(PAWN));
        }
#undef BIT
    }

    for (int k = 0; k < NKEYS; k++) {
        int asc = 1, desc = 1;
        for (int i = 0; i < KEYS[k].len; i++) {
            if (KEYS[k].cell[i] < 128)
                KEYBB[k] |= 1ull << (KEYS[k].cell[i] & 63);
            if (i) {
                asc &= KEYS[k].cell[i] > KEYS[k].cell[i - 1];
                desc &= KEYS[k].cell[i] < KEYS[k].cell[i - 1];
            }
        }
        /* pext(x, KEYBB) restituisce le celle in ordine di casa: va bene per
           le chiavi crescenti; le uniche decrescenti sono le spinte doppie
           del nero (2 celle), per cui basta scambiare i due bit */
        KEYDESC[k] = KEYS[k].len > 1 && desc;
        if (KEYS[k].len > 1 && !asc && !(desc && KEYS[k].len == 2) && KEYS[k].cell[0] < 128 &&
            KEYS[k].cell[KEYS[k].len - 1] < 128) {
            fprintf(stderr, "key %d: unsupported cell order\n", k);
            exit(1);
        }
    }

    /* mosse pronte nelle viste di generazione: il ciclo appiattito le copia
       e tiene solo quelle il cui bit e' acceso */
    for (int s = 0; s < 2; s++)
        for (int pc = 1; pc < NPIECES; pc++)
            for (int sq = 0; sq < 64; sq++) {
                VList vl = GV[s][pc][sq];
                for (int i = 0; i < vl.n; i++) {
                    View *v = &VIEWS[vl.first + i];
                    int q = s == WHITE ? WQ : BQ;
                    for (int k = 0; k < 8; k++)
                        v->mv[k] = mk_move(sq, sq, EMPTY);
                    v->nmv = v->promo ? 4 * KEYS[v->key].len : KEYS[v->key].len;
                    for (int c = 0; c < KEYS[v->key].len; c++) {
                        if (!v->promo) {
                            v->mv[c] = mk_move(sq, v->to[c], EMPTY);
                            continue;
                        }
                        for (int j = 0; j < 4; j++)
                            v->mv[4 * c + j] = mk_move(sq, v->to[c], q - j);
                    }
                }
            }
    for (int m = 0; m < 256; m++) {
        EXPAND[0][m] = m;
        EXPAND[1][m] = (m & 1 ? 0x0F : 0) | (m & 2 ? 0xF0 : 0);
    }
}

void geometry_init(void)
{
    build_keys();
    build_bases();
    build_views();
}
