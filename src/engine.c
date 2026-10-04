/*
 * Motore: mantiene la serializzazione della posizione (le chiavi) e
 * interroga la mmap. Il ciclo di generazione non conosce le regole:
 * legge chiavi, legge maschere, emette mosse.
 */
#include "bitboard.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

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

#ifdef __AVX2__
static __m128i SHUF[256];                                    /* maschera -> compattazione */
static int32_t VK[MAX_VIEWS], VO[MAX_VIEWS], VPR[MAX_VIEWS]; /* viste in forma SoA */
#endif

static void simd_init(void)
{
#ifdef __AVX2__
    for (int m = 0; m < 256; m++) {
        uint8_t b[16];
        memset(b, 0x80, sizeof b);
        for (int k = 0, j = 0; k < 8; k++)
            if (m >> k & 1) {
                b[2 * j] = 2 * k;
                b[2 * j + 1] = 2 * k + 1;
                j++;
            }
        memcpy(&SHUF[m], b, 16);
    }
    for (int v = 0; v < NVIEWS; v++) {
        VK[v] = VIEWS[v].key;
        VO[v] = VIEWS[v].base + VIEWS[v].pos;
        VPR[v] = VIEWS[v].promo * 256;
    }
#endif
}

static void hyb_init(void);

void engine_init(void)
{
    geometry_init();
    simd_init();
    hyb_init();
    bitboard_init();
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
        }
        if (pc) {
            p->by_type[0] ^= b;
            p->by_type[TYPE(pc)] ^= b;
        }
    }
    /* l'occupazione per colore serve a entrambi: mmap la usa per scorrere
       solo i pezzi propri */
    if (st && old != pc) {
        uint64_t b = 1ull << sq;
        if (old)
            p->by_color[COLOR[old]] ^= b;
        if (pc)
            p->by_color[COLOR[pc]] ^= b;
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
    int us = p->side, pc = p->board[MV_FROM(m)], t = TYPE(pc), g = p->ghost;

    if (g != NO_SQ) {
        if (st & ST_KEYS)
            cell_set(p, CAP(g), (us ^ 1) + 1, 0);
        p->ghost = NO_SQ;
        if (t == PAWN && MV_TO(m) == g)
            put_(p, g + (us == WHITE ? -8 : 8), EMPTY, st);
    }

    put_(p, MV_FROM(m), EMPTY, st);
    put_(p, MV_TO(m), MV_PROMO(m) ? MV_PROMO(m) : pc, st);

    if (t == KING) {
        p->ksq[us] = MV_TO(m);
        if (MV_TO(m) - MV_FROM(m) == 2) {
            put_(p, MV_FROM(m) + 1, p->board[MV_FROM(m) + 3], st);
            put_(p, MV_FROM(m) + 3, EMPTY, st);
        } else if (MV_FROM(m) - MV_TO(m) == 2) {
            put_(p, MV_FROM(m) - 1, p->board[MV_FROM(m) - 4], st);
            put_(p, MV_FROM(m) - 4, EMPTY, st);
        }
    }

    set_rights_(p, p->rights & RMASK[MV_FROM(m)] & RMASK[MV_TO(m)], st);

    if (t == PAWN && (MV_TO(m) - MV_FROM(m) == 16 || MV_FROM(m) - MV_TO(m) == 16)) {
        p->ghost = (MV_FROM(m) + MV_TO(m)) / 2;
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

/* ---- ciclo appiattito ----
 *
 * Due passate lineari al posto di 4 cicli annidati:
 *   1. raccolta: si scorrono solo i pezzi propri (ctz sull'occupazione) e
 *      si accodano gli indici delle loro viste. Si copiano sempre 4 indici
 *      e si avanza del numero reale: nessun ciclo interno.
 *   2. emissione: un ciclo sulle viste; ogni vista ha le mosse pronte e
 *      la maschera (espansa per le promozioni) decide quali tenere, con
 *      una compattazione a 8 passi fissi, senza rami.
 */

#ifndef FLAT_MODE
#define FLAT_MODE 2
#endif

#define COLLECT(us, lists, extra)                                                \
    for (uint64_t own = bb_color(p, us); own; own &= own - 1) {                  \
        int sq = __builtin_ctzll(own);                                           \
        VList vl = lists[us][piece_at(p, sq)][sq];                               \
        COUNT(READS_GEOM);                                                       \
        for (int k = 0; k < 4; k++)                                              \
            act[na + k] = vl.first + k;                                          \
        extra;                                                                   \
        na += vl.n;                                                              \
    }

/* gen_pseudo_flat e le varianti SIMD sono definite dopo il generatore legale,
   con cui condividono l'emissione (emit_view / run_views). */

/* ---- legalita' per interferenza ----
 *
 * La mmap, letta dalla casa del re, indica per ogni linea quali pezzi
 * "interferiscono": il possibile autore di uno scacco o di
 * un'inchiodatura e il pezzo proprio inchiodato. Si verifica solo il
 * tipo di quei pochi nemici; tutti gli altri pezzi non fanno ne' make ne'
 * test: la loro maschera di destinazioni viene filtrata dalle case
 * consentite (parata dello scacco, linea dell'inchiodatura) senza rami.
 * Restano due verifiche puntuali: le case del re e l'en passant.
 */

static inline uint64_t cells_bb(const View *v, unsigned m)
{
    uint64_t b = 0;
    for (; m; m &= m - 1)
        b |= 1ull << v->to[__builtin_ctz(m)];
    return b;
}

/* interferenze sul re: scacchi, evasioni, x-ray, pezzi inchiodati */
static inline __attribute__((always_inline)) int king_interference(
    const Pos *p, int us, int ksq, uint64_t *evasion_, uint64_t *xray_, uint64_t *pinned_,
    uint64_t *pin_line)
{
    int nchk = 0;
    uint64_t evasion = ~0ull, xray = 0, pinned = 0;

    VList vl = KL[us][ksq];
    for (const View *v = &VIEWS[vl.first], *e = v + vl.n; v < e; v++) {
        uint64_t info = mm64_at(v->base + (key_at(p, v->key) * MAXLEN + v->pos) * 8);
        for (; info; info >>= 32) {
            uint32_t x = (uint32_t)info;
            if (!(x & 0xFF))
                continue;
            int es = v->to[__builtin_ctz(x & 0xFF)];
            if (!(v->attackers >> piece_at(p, es) & 1))
                continue; /* interferenza apparente: non e' uno slider della linea */
            if (x >> 24) {
                int ps = v->to[__builtin_ctz(x >> 24)];
                pinned |= 1ull << ps;
                pin_line[ps] = KEYBB[v->key];
            } else {
                evasion = nchk++ ? 0 : cells_bb(v, x >> 8 & 0xFF);
                xray |= cells_bb(v, x >> 16 & 0xFF);
            }
        }
    }
    vl = KR[us][ksq];
    for (const View *v = &VIEWS[vl.first], *e = v + vl.n; v < e; v++) {
        unsigned m = mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos);
        for (; m; m &= m - 1) {
            int cs = v->to[__builtin_ctz(m)];
            if (v->attackers >> piece_at(p, cs) & 1)
                evasion = nchk++ ? 0 : 1ull << cs;
        }
    }
    *evasion_ = evasion;
    *xray_ = xray;
    *pinned_ = pinned;
    return nchk;
}

static inline __attribute__((always_inline)) int ep_ok(const Pos *p, int from, int to, int us,
                                                       int ksq)
{
    Pos q;
    pos_copy_keys(&q, p);
    make_move_keys(&q, mk_move(from, to, EMPTY));
    return !attacked(&q, ksq, us);
}

/* en passant con i bitboard, come Stockfish: si tolgono i due pedoni e si
   guarda se qualcosa (tranne il pedone catturato) colpisce il re */
static inline int ep_ok_bb(const Pos *p, int from, int to, int us, int ksq)
{
    Bits cap = 1ull << (to + (us == WHITE ? -8 : 8));
    Bits occ = (bb_type(p, 0) ^ (1ull << from) ^ cap) | (1ull << to);
    return !(attackers_to(p, ksq, occ) & bb_color(p, us ^ 1) & ~cap);
}

static inline __attribute__((always_inline)) int king_moves(const Pos *p, Move *out, int n, int us,
                                                            int ksq, int nchk, uint64_t xray)
{
    VList vl = GV[us][piece_at(p, ksq)][ksq];
    for (const View *v = &VIEWS[vl.first], *e = v + vl.n; v < e; v++) {
        unsigned m = mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos);
        for (; m; m &= m - 1) {
            int to = v->to[__builtin_ctz(m)];
            if (v->sem == SEM_CASTLE) {
                if (!nchk && !attacked(p, (ksq + to) / 2, us) && !attacked(p, to, us))
                    n = emit(out, n, ksq, to, EMPTY);
            } else if (!(xray >> to & 1) && !attacked(p, to, us)) {
                n = emit(out, n, ksq, to, EMPTY);
            }
        }
    }
    return n;
}

int gen_legal_int(const Pos *p, Move *out)
{
    int us = side_to_move(p), n = 0, ksq = king_square(p, us), g = ep_square(p);
    uint64_t evasion, xray, pinned, pin_line[64];
    int nchk = king_interference(p, us, ksq, &evasion, &xray, &pinned, pin_line);

    /* il ciclo unico, ora legale: la maschera dice "dove", allow dice "se" */
    if (nchk < 2)
        for (int sq = 0; sq < 64; sq++) {
            VList gl = GVL[us][piece_at(p, sq)][sq];
            uint64_t allow = evasion & (pinned >> sq & 1 ? pin_line[sq] : ~0ull);
            for (const View *v = &VIEWS[gl.first], *e = v + gl.n; v < e; v++) {
                unsigned m = mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos);
                while (m) {
                    int to = v->to[__builtin_ctz(m)];
                    m &= m - 1;
                    if (v->cap && to == g) { /* en passant: verifica completa */
                        if (ep_ok(p, sq, to, us, ksq))
                            n = emit(out, n, sq, to, EMPTY);
                        continue;
                    }
                    int ok = allow >> to & 1;
                    for (int i = 0; i < NPROMO[v->promo]; i++) {
                        out[n] = mk_move(sq, to, PROMO[us][v->promo][i]);
                        n += ok;
                    }
                }
            }
        }

    /* il re: le sue case si verificano una per una (come attackers_to in Stockfish) */
    return king_moves(p, out, n, us, ksq, nchk, xray);
}

/* Strategie di emissione delle viste raccolte:
 *   EM_SCALAR  compattazione scalare (FLAT_MODE)
 *   EM_SHUF    compattazione con pshufb: nessun salto ne' ciclo per vista
 *   EM_GATHER  come EM_SHUF, ma chiave, tabella ed espansione vengono lette
 *              per 8 viste alla volta con vpgatherdd */
enum { EM_SCALAR, EM_SHUF, EM_GATHER };

static inline unsigned view_mask(const Pos *p, const View *v)
{
    return EXPAND[v->promo][mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos)];
}

static inline __attribute__((always_inline)) int emit_view(const Pos *p, Move *out, int n,
                                                           const View *v, unsigned m, uint64_t a,
                                                           int g, int us, int ksq,
                                                           const int legal, const int em,
                                                           const int epm)
{
    if (legal && v->cap && g != NO_SQ) { /* en passant: fuori dal flusso, verifica completa */
        for (int k = 0; k < 8; k++)
            if ((m >> k & 1) && MV_TO(v->mv[k]) == g) {
                m &= ~(1u << k);
                if (epm ? ep_ok_bb(p, MV_FROM(v->mv[k]), g, us, ksq)
                        : ep_ok(p, MV_FROM(v->mv[k]), g, us, ksq))
                    n = emit(out, n, MV_FROM(v->mv[k]), g, EMPTY);
            }
    }
    if (em == EM_SCALAR) {
        if (FLAT_MODE >= 1 && !m)
            return n;
        for (int k = 0; k < (FLAT_MODE >= 2 ? v->nmv : 8); k++) {
            out[n] = v->mv[k];
            n += m >> k & a >> MV_TO(v->mv[k]) & 1;
        }
        return n;
    }
#ifdef __AVX2__
    if (legal && a != ~0ull) /* scacco o pezzo inchiodato: celle consentite in un'istruzione
                                (le celle di ogni chiave sono in ordine crescente di casa) */
    {
        unsigned keep = (unsigned)pext(a, KEYBB[v->key]);
        if (KEYDESC[v->key])
            keep = (keep >> 1 | keep << 1) & 3;
        m &= EXPAND[v->promo][keep];
    }
    __m128i mv = _mm_loadu_si128((const __m128i *)v->mv);
    _mm_storeu_si128((__m128i *)(out + n), _mm_shuffle_epi8(mv, SHUF[m]));
    return n + __builtin_popcount(m);
#else
    return n;
#endif
}

static inline __attribute__((always_inline)) int run_views(const Pos *p, Move *out, int n,
                                                           uint16_t *act, uint64_t *allow, int na,
                                                           int g, int us, int ksq,
                                                           const int legal, const int em,
                                                           const int epm)
{
#ifdef __AVX2__
    if (em == EM_GATHER) {
        for (int k = 0; k < 8; k++) { /* coda: indici validi, maschera azzerata sotto */
            act[na + k] = 0;
            if (legal)
                allow[na + k] = 0;
        }
        const __m256i ff = _mm256_set1_epi32(0xFF);
        const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        for (int i = 0; i < na; i += 8) {
            __m256i vi = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(act + i)));
            __m256i k = _mm256_i32gather_epi32(VK, vi, 4);
            __m256i o = _mm256_i32gather_epi32(VO, vi, 4);
            __m256i pr = _mm256_i32gather_epi32(VPR, vi, 4);
            __m256i kv = _mm256_and_si256(_mm256_i32gather_epi32((const int *)p->key, k, 2),
                                          _mm256_set1_epi32(0xFFFF));
            __m256i idx = _mm256_add_epi32(o, _mm256_slli_epi32(kv, 3));
            __m256i m = _mm256_and_si256(_mm256_i32gather_epi32((const int *)MM, idx, 1), ff);
            m = _mm256_and_si256(
                _mm256_i32gather_epi32((const int *)EXPAND, _mm256_add_epi32(pr, m), 1), ff);
            m = _mm256_and_si256(m, _mm256_cmpgt_epi32(_mm256_set1_epi32(na - i), lanes));
            uint32_t ms[8];
            _mm256_storeu_si256((__m256i *)ms, m);
            for (int l = 0; l < 8; l++)
                n = emit_view(p, out, n, &VIEWS[act[i + l]], ms[l], legal ? allow[i + l] : ~0ull,
                              g, us, ksq, legal, EM_SHUF, epm);
        }
        return n;
    }
#endif
    for (int i = 0; i < na; i++) {
        const View *v = &VIEWS[act[i]];
        COUNT(READS_GEOM);
        n = emit_view(p, out, n, v, view_mask(p, v), legal ? allow[i] : ~0ull, g, us, ksq, legal,
                      em, epm);
    }
    return n;
}

static inline __attribute__((always_inline)) int legal_flat_(const Pos *p, Move *out, const int ph,
                                                             const int em)
{
    int us = side_to_move(p), n = 0, na = 0, ksq = king_square(p, us), g = ep_square(p);
    uint64_t evasion, xray, pinned, pin_line[64], allow[16 * 4 + 8];
    uint16_t act[16 * 4 + 8];
    int nchk = 0;
    evasion = ~0ull, xray = pinned = 0;
    if (ph & PH_INT) {
        nchk = king_interference(p, us, ksq, &evasion, &xray, &pinned, pin_line);
        if (ph == PH_INT) /* fase isolata: il risultato deve essere usato */
            n = (int)(evasion ^ xray ^ pinned) & 1;
    }

    if ((ph & PH_PIECE) && nchk < 2) {
        COLLECT(us, GVL, {
            uint64_t a = evasion & (pinned >> sq & 1 ? pin_line[sq] : ~0ull);
            for (int k = 0; k < 4; k++)
                allow[na + k] = a;
        });
        if (!(ph & PH_EMIT))
            return n + na + act[na ? na - 1 : 0];
        n = run_views(p, out, n, act, allow, na, g, us, ksq, 1, em, 0);
    }
    return ph & PH_KING ? king_moves(p, out, n, us, ksq, nchk, xray) : n;
}

static inline __attribute__((always_inline)) int pseudo_(const Pos *p, Move *out, const int em)
{
    int s = side_to_move(p), na = 0;
    uint16_t act[16 * 4 + 8];
    COLLECT(s, GV, (void)0);
    return run_views(p, out, 0, act, NULL, na, NO_SQ, s, 0, 0, em, 0);
}

int gen_pseudo_flat(const Pos *p, Move *out) { return pseudo_(p, out, EM_SCALAR); }
int gen_legal_flat(const Pos *p, Move *out) { return legal_flat_(p, out, PH_ALL, EM_SCALAR); }
int mm_phase_int(const Pos *p, Move *out) { return legal_flat_(p, out, PH_INT, EM_SCALAR); }
int mm_phase_collect(const Pos *p, Move *out) { return legal_flat_(p, out, PH_PIECE, EM_SCALAR); }
int mm_phase_gen(const Pos *p, Move *out) { return legal_flat_(p, out, PH_PIECE | PH_EMIT, EM_SCALAR); }
int mm_phase_king(const Pos *p, Move *out) { return legal_flat_(p, out, PH_KING, EM_SCALAR); }

/* ---- versione ibrida ----
 *
 * Le mosse dei pezzi escono dalla mmap (emissione pshufb, senza rami).
 * La sicurezza del re e' una domanda su insiemi, e si risponde come
 * Stockfish:
 *   1. prima i tipi: gli sniper sono gli slider nemici allineati al re su
 *      scacchiera vuota (una AND per famiglia); di solito nessuno;
 *   2. poi l'interferenza, solo per loro: BETWEEN & occupate conta i pezzi
 *      in mezzo (0 = scacco, 1 proprio = inchiodatura, 2+ = niente);
 *   3. cavalli e pedoni che danno scacco: intersezioni con le tabelle.
 * Le destinazioni del re si verificano con attackers_to sull'occupazione
 * senza il re (gli x-ray si risolvono da soli), oppure, nella variante
 * _pdep, con le viste di attacco della mmap convertite in bitboard.
 */

static uint8_t VACLS[MAX_VIEWS]; /* vista di attacco -> famiglia di attaccanti */
enum { AC_ORTH, AC_DIAG, AC_KNIGHT, AC_KING, AC_PAWN, NAC };

static void hyb_init(void)
{
    for (int v = 0; v < NVIEWS; v++) {
        uint16_t a = VIEWS[v].attackers;
        int t = a ? TYPE(__builtin_ctz(a)) : 0;
        VACLS[v] = t == ROOK ? AC_ORTH : t == BISHOP ? AC_DIAG : t == KNIGHT ? AC_KNIGHT
                 : t == KING ? AC_KING : AC_PAWN;
    }
}

/* attacco sulla casa sq: ogni vista di attacco da' una maschera, PDEP la
   porta sulle case reali, una AND la confronta con la famiglia giusta */
static inline int attacked_pdep(const Pos *p, int sq, int us, const Bits *cls)
{
    VList vl = AV[us][sq];
    Bits hit = 0;
    for (int i = vl.first, e = vl.first + vl.n; i < e; i++) {
        const View *v = &VIEWS[i];
        unsigned m = mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos);
        hit |= pdep(m, KEYBB[v->key]) & cls[VACLS[i]];
    }
    return hit != 0;
}

static inline __attribute__((always_inline)) int legal_hyb_(const Pos *p, Move *out, const int ph,
                                                            const int kmode)
{
    int us = side_to_move(p), them = us ^ 1, n = 0, na = 0, ksq = king_square(p, us), g = ep_square(p);
    Bits occ = bb_type(p, 0), own = bb_color(p, us), enemy = bb_color(p, them);
    Bits orth = (bb_type(p, ROOK) | bb_type(p, QUEEN)) & enemy;
    Bits diag = (bb_type(p, BISHOP) | bb_type(p, QUEEN)) & enemy;
    Bits checkers = 0, pinned = 0, xray = 0, pin_line[64], allow[16 * 4 + 8];
    uint16_t act[16 * 4 + 8];

    if (ph & PH_INT) {
        checkers = ((KNIGHT_ATT[ksq] & bb_type(p, KNIGHT)) | (PAWN_ATT[us][ksq] & bb_type(p, PAWN))) &
                   enemy;
        Bits snipers = (ROOK_PSEUDO[ksq] & orth) | (BISHOP_PSEUDO[ksq] & diag);
        while (snipers) {
            int s = pop(&snipers);
            Bits b = BETWEEN[ksq][s] & occ;
            if (!b) {
                checkers |= 1ull << s;
                xray |= LINE[s][ksq] & KING_ATT[ksq] & ~BETWEEN[s][ksq] & ~(1ull << s);
            } else if (!(b & (b - 1)) && (b & own)) {
                pinned |= b;
                pin_line[__builtin_ctzll(b)] = LINE[ksq][s];
            }
        }
        if (ph == PH_INT) /* fase isolata: il risultato deve essere usato */
            return (int)((checkers ^ pinned ^ xray) & 1);
    }
    int nchk = __builtin_popcountll(checkers);

    if ((ph & PH_PIECE) && !(checkers | pinned) && g == NO_SQ) {
        /* caso comune: niente scacco, niente inchiodature, niente en passant.
           Ogni mossa pseudo-legale dei pezzi e' legale: emissione pura. */
        COLLECT(us, GVL, (void)0);
        n = run_views(p, out, n, act, NULL, na, g, us, ksq, 0, EM_SHUF, 1);
    } else if ((ph & PH_PIECE) && nchk < 2) {
        Bits evasion = checkers ? BETWEEN[ksq][__builtin_ctzll(checkers)] | checkers : ~0ull;
        COLLECT(us, GVL, {
            uint64_t a = evasion & (pinned >> sq & 1 ? pin_line[sq] : ~0ull);
            for (int k = 0; k < 4; k++)
                allow[na + k] = a;
        });
        n = run_views(p, out, n, act, allow, na, g, us, ksq, 1, EM_SHUF, 1);
    }

    if (ph & PH_KING) {
        Bits cls[NAC] = { orth, diag, bb_type(p, KNIGHT) & enemy, bb_type(p, KING) & enemy,
                          bb_type(p, PAWN) & enemy };
        Bits occ_nok = occ ^ (1ull << ksq);
        VList vl = GV[us][piece_at(p, ksq)][ksq];
        for (const View *v = &VIEWS[vl.first], *e = v + vl.n; v < e; v++) {
            unsigned m = mm_at(v->base + key_at(p, v->key) * MAXLEN + v->pos);
            if (v->sem == SEM_CASTLE) {
                if (nchk || !m)
                    continue;
                int to = v->to[1], mid = (ksq + to) / 2;
                int safe = kmode ? !attacked_pdep(p, mid, us, cls) && !attacked_pdep(p, to, us, cls)
                                 : !((attackers_to(p, mid, occ) | attackers_to(p, to, occ)) & enemy);
                out[n] = mk_move(ksq, to, EMPTY);
                n += safe;
                continue;
            }
            for (Bits t = pdep(m, KEYBB[v->key]); t; t &= t - 1) {
                int to = __builtin_ctzll(t);
                int safe = kmode ? !(xray >> to & 1) && !attacked_pdep(p, to, us, cls)
                                 : !(attackers_to(p, to, occ_nok) & enemy);
                out[n] = mk_move(ksq, to, EMPTY);
                n += safe;
            }
        }
    }
    return n;
}

int gen_legal_hyb(const Pos *p, Move *out) { return legal_hyb_(p, out, PH_ALL, 0); }
int gen_legal_hyb_pdep(const Pos *p, Move *out) { return legal_hyb_(p, out, PH_ALL, 1); }
int hyb_phase_int(const Pos *p, Move *out) { return legal_hyb_(p, out, PH_INT, 0); }
int hyb_phase_king(const Pos *p, Move *out) { return legal_hyb_(p, out, PH_KING, 0); }

#ifdef __AVX2__
int gen_pseudo_shuf(const Pos *p, Move *out) { return pseudo_(p, out, EM_SHUF); }
int gen_pseudo_gather(const Pos *p, Move *out) { return pseudo_(p, out, EM_GATHER); }
int gen_legal_shuf(const Pos *p, Move *out) { return legal_flat_(p, out, PH_ALL, EM_SHUF); }
int gen_legal_gather(const Pos *p, Move *out) { return legal_flat_(p, out, PH_ALL, EM_GATHER); }
#else
int gen_pseudo_shuf(const Pos *p, Move *out) { return gen_pseudo_flat(p, out); }
int gen_pseudo_gather(const Pos *p, Move *out) { return gen_pseudo_flat(p, out); }
int gen_legal_shuf(const Pos *p, Move *out) { return gen_legal_flat(p, out); }
int gen_legal_gather(const Pos *p, Move *out) { return gen_legal_flat(p, out); }
#endif

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
    buf[0] = 'a' + (MV_FROM(m) & 7);
    buf[1] = '1' + (MV_FROM(m) >> 3);
    buf[2] = 'a' + (MV_TO(m) & 7);
    buf[3] = '1' + (MV_TO(m) >> 3);
    buf[4] = MV_PROMO(m) ? "  nbrq"[TYPE(MV_PROMO(m))] : 0;
    buf[5] = 0;
}
