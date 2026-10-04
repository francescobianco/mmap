/*
 * mmap - generatore di mosse scacchistiche guidato da una mappa congelata.
 *
 * Idea: la posizione non viene "interpretata" dal generatore. Viene
 * serializzata in un insieme di chiavi (una per ogni "linea" di case:
 * traverse, colonne, diagonali, anelli di cavallo/re, raggi di pedone...)
 * mantenute incrementalmente. Ogni chiave, letta in base 3
 * (0 = vuota, 1 = bianco, 2 = nero), indicizza la mmap, che restituisce
 * direttamente la maschera delle case di destinazione.
 * Nel ciclo di generazione non c'e' nessun test di occupazione.
 */
#ifndef MMAP_H
#define MMAP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { EMPTY, WP, WN, WB, WR, WQ, WK, BP, BN, BB, BR, BQ, BK, NPIECES };
enum { PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { WHITE, BLACK };

/* Semantiche: come una sequenza di celle si traduce in destinazioni. */
enum {
    SEM_SLIDE,  /* linea: dal punto pos scorre nei due versi fino al blocco */
    SEM_LEAP,   /* anello: ogni cella non occupata da un pezzo proprio */
    SEM_PUSH,   /* raggio: celle vuote consecutive dall'inizio */
    SEM_CAPT,   /* anello: solo celle occupate dal nemico */
    SEM_CASTLE, /* raggio: destinazione (bit 1) solo se tutto e' vuoto */
    SEM_HIT,    /* linea: solo il primo pezzo nemico nei due versi (attacchi) */
    NSEM
};

/*
 * Spazio delle celle osservabili:
 *   0..63    occupazione reale delle case
 *   64..127  strato "cattura": occupazione reale + fantasma en passant
 *   128..131 diritti di arrocco (0 = disponibile, 1 = perso)
 */
#define NCELLS    132
#define CAP(sq)   (64 + (sq))
#define RIGHT(r)  (128 + (r))
#define NO_SQ     64

#define MAXLEN    8
#define MAX_KEYS  400
#define MAX_MEMB  32
#define MAX_VIEWS 4096

#define MMAP_MAGIC "MMAPCHS1"

typedef struct {
    uint8_t len;
    uint8_t cell[MAXLEN];
} Key;

typedef struct {
    uint16_t key;
    uint16_t w;     /* peso 3^i della cella dentro la chiave */
} Member;

/* Una vista: "questo pezzo, su questa casa, legge questa chiave". */
typedef struct {
    uint32_t base;      /* offset nella mmap per (semantica, colore, lunghezza) */
    uint16_t key;
    uint8_t pos;
    uint8_t promo;      /* 1 = ogni destinazione genera 4 promozioni */
    uint16_t attackers; /* solo viste di attacco: pezzi che colpiscono */
    uint8_t to[MAXLEN]; /* bit della maschera -> casa */
} View;

typedef struct {
    uint16_t first;
    uint8_t n;
} VList;

typedef struct {
    char magic[8];
    uint32_t size;
    uint32_t pad;
} MMapHeader;

extern Key KEYS[MAX_KEYS];
extern int NKEYS;
extern Member MEMB[NCELLS][MAX_MEMB];
extern uint8_t NMEMB[NCELLS];
extern View VIEWS[MAX_VIEWS];
extern int NVIEWS;
extern VList GV[2][NPIECES][64]; /* viste di generazione [lato][pezzo][casa] */
extern VList AV[2][64];          /* viste di attacco    [lato][casa]        */
extern uint32_t BASE[NSEM][2][MAXLEN + 1];
extern uint32_t TABLE_SIZE;
extern const uint32_t POW3[MAXLEN + 1];

void geometry_init(void);

/* engine.c */
typedef struct {
    uint8_t from, to, promo;
} Move;

/*
 * Stato della partita, in tre sezioni:
 *   1. scacchiera e flag       (letta da tutti; unico stato del classico)
 *   2. bitboard stile Stockfish (byType[0] = tutti i pezzi, byType[1..6], byColor)
 *   3. chiavi della mmap
 * Nel confronto "a parita' di stato" make_move le mantiene tutte e tre;
 * nel confronto "stato proprio" ogni motore copia e aggiorna solo cio' che usa.
 */
typedef struct {
    uint8_t board[64];
    uint8_t side, ghost, rights;
    uint8_t ksq[2];
    uint8_t pad[3];
    uint64_t by_type[7];
    uint64_t by_color[2];
    uint16_t key[MAX_KEYS];
} Pos;

#define POS_BOARD_BYTES offsetof(Pos, by_type)
#define POS_BB_BYTES    offsetof(Pos, key)

/*
 * Primitive di lettura dello stato, condivise da tutti i generatori.
 * Sono static inline: dopo l'ottimizzazione spariscono in un singolo
 * accesso a memoria, quindi non aggiungono alcun layer di costo.
 */
extern const uint8_t *MM;

#define TYPE(pc) (((pc) - 1) % 6 + 1)

/* Con -DCOUNT_READS (solo la build di analisi) le primitive contano gli
   accessi (READS_GEOM: letture delle tabelle geometriche precalcolate); nella build normale COUNT() non genera codice. */
#ifdef COUNT_READS
extern uint64_t READS_BOARD, READS_KEY, READS_TABLE, READS_GEOM, READS_BB;
#define COUNT(c) ((c)++)
#else
#define COUNT(c) ((void)0)
#endif

static inline int piece_at(const Pos *p, int sq) { COUNT(READS_BOARD); return p->board[sq]; }
static inline unsigned key_at(const Pos *p, int k) { COUNT(READS_KEY); return p->key[k]; }
static inline unsigned mm_at(uint32_t i) { COUNT(READS_TABLE); return MM[i]; }
static inline uint64_t bb_type(const Pos *p, int t) { COUNT(READS_BB); return p->by_type[t]; }
static inline uint64_t bb_color(const Pos *p, int c) { COUNT(READS_BB); return p->by_color[c]; }
static inline uint64_t bb_pieces(const Pos *p, int c, int t) { return bb_type(p, t) & bb_color(p, c); }
static inline int side_to_move(const Pos *p) { return p->side; }
static inline int ep_square(const Pos *p) { return p->ghost; }
static inline int castle_rights(const Pos *p) { return p->rights; }
static inline int king_square(const Pos *p, int s) { return p->ksq[s]; }

static inline void pos_copy(Pos *d, const Pos *s) { *d = *s; }
static inline void pos_copy_board(Pos *d, const Pos *s) { memcpy(d, s, POS_BOARD_BYTES); }
static inline void pos_copy_bb(Pos *d, const Pos *s) { memcpy(d, s, POS_BB_BYTES); }
static inline void pos_copy_keys(Pos *d, const Pos *s)
{
    memcpy(d, s, POS_BOARD_BYTES);
    memcpy(d->key, s->key, sizeof s->key);
}

static inline int emit(Move *out, int n, int from, int to, int promo)
{
    out[n] = (Move){ (uint8_t)from, (uint8_t)to, (uint8_t)promo };
    return n + 1;
}

/*
 * Filtro di legalita' identico per tutti i generatori: cambia solo quale
 * generatore pseudo-legale e quale test d'attacco vengono istanziati.
 */
#define DEFINE_GEN_LEGAL(name, gen, att, make, copy)                                       \
    int name(const Pos *p, Move *out)                                            \
    {                                                                            \
        Move ps[512];                                                            \
        int n = gen(p, ps), k = 0, us = side_to_move(p);                         \
        for (int i = 0; i < n; i++) {                                            \
            Move m = ps[i];                                                      \
            if (TYPE(piece_at(p, m.from)) == KING &&                             \
                (m.to - m.from == 2 || m.from - m.to == 2) &&                    \
                (att(p, m.from, us) || att(p, (m.from + m.to) / 2, us)))         \
                continue;                                                        \
            Pos q;                                                               \
            copy(&q, p);                                                         \
            make(&q, m);                                                         \
            if (!att(&q, king_square(&q, us), us))                               \
                out[k++] = m;                                                    \
        }                                                                        \
        return k;                                                                \
    }

int mmap_load(const char *path);
void engine_init(void);
int pos_from_fen(Pos *p, const char *fen);
int gen_pseudo(const Pos *p, Move *out);
int gen_legal(const Pos *p, Move *out);
int attacked(const Pos *p, int sq, int us);
void make_move(Pos *p, Move m);       /* aggiorna tutto lo stato */
void make_move_board(Pos *p, Move m); /* solo scacchiera */
void make_move_bb(Pos *p, Move m);    /* scacchiera + bitboard */
void make_move_keys(Pos *p, Move m);  /* scacchiera + chiavi */
int gen_legal_own(const Pos *p, Move *out); /* mmap con il solo stato proprio */
void move_str(Move m, char *buf);

/* classic.c: generatore tradizionale a IF sulla stessa struttura Pos */
void classic_init(void);
int gen_pseudo_if(const Pos *p, Move *out);
int gen_legal_if(const Pos *p, Move *out);
int gen_legal_if_board(const Pos *p, Move *out); /* make/copy senza chiavi */
int attacked_if(const Pos *p, int sq, int us);

/* bitboard.c: generatore bitboard stile Stockfish (PEXT, pin, scacchi) */
void bitboard_init(void);
int gen_pseudo_bb(const Pos *p, Move *out);
int gen_legal_bb(const Pos *p, Move *out);    /* filtro generico, come gli altri */
int gen_legal_sf(const Pos *p, Move *out);    /* legale alla Stockfish, senza make */
int attacked_bb(const Pos *p, int sq, int us);

#endif
