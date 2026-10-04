/*
 * mmapgen: "congela" la conoscenza scacchistica nel file mmap.bin.
 *
 * Per ogni semantica, colore, lunghezza di chiave, valore della chiave
 * (base 3) e posizione nella linea, calcola una volta per tutte la
 * maschera di destinazioni. E' l'unico punto del progetto in cui si
 * ragiona su "casa vuota / propria / nemica".
 */
#include "mmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int digit(int key, int i) { return key / POW3[i] % 3; }

/*
 * SEM_KING: il re (lato side) e' in pos. Per ciascun verso della linea
 * (d = 0 verso indici bassi, d = 1 verso indici alti) 4 byte:
 *   byte 0  il nemico da verificare: il primo pezzo se nemico (possibile
 *           scacco) oppure il secondo se il primo e' proprio (possibile
 *           inchiodatura)
 *   byte 1  case che parano lo scacco: quelle tra re e nemico + il nemico
 *   byte 2  casa x-ray: dietro al re sulla stessa linea (il re non puo'
 *           ritirarsi li' se lo scacco e' confermato)
 *   byte 3  il pezzo proprio candidato all'inchiodatura
 * Resta da verificare solo il tipo del nemico (slider compatibile).
 */
static uint64_t king_info(int side, int len, int key, int pos)
{
    int own = side + 1, foe = 2 - side;
    uint64_t e = 0;
    for (int d = 0; d < 2; d++) {
        int dir = d ? 1 : -1, f = -1, s = -1;
        uint32_t between = 0, x = 0;
        for (int i = pos + dir; i >= 0 && i < len; i += dir) {
            if (!digit(key, i)) {
                if (f < 0)
                    between |= 1u << i;
                continue;
            }
            if (f < 0)
                f = i;
            else {
                s = i;
                break;
            }
        }
        if (f < 0)
            continue;
        if (digit(key, f) == foe) {
            int xs = pos - dir;
            x = (1u << f) | (between | 1u << f) << 8;
            if (xs >= 0 && xs < len)
                x |= (1u << xs) << 16;
        } else if (digit(key, f) == own && s >= 0 && digit(key, s) == foe) {
            x = (1u << s) | (1u << f) << 24;
        }
        e |= (uint64_t)x << (32 * d);
    }
    return e;
}

static uint8_t semantic(int sem, int side, int len, int key, int pos)
{
    int own = side + 1, foe = 2 - side;
    uint8_t m = 0;

    switch (sem) {
    case SEM_SLIDE:
        /* la cella pos e' ignorata: vale sia per il pezzo che muove,
           sia per le query di attacco su case vuote */
        for (int dir = -1; dir <= 1; dir += 2)
            for (int i = pos + dir; i >= 0 && i < len; i += dir) {
                int d = digit(key, i);
                if (d == own)
                    break;
                m |= 1u << i;
                if (d == foe)
                    break;
            }
        break;
    case SEM_LEAP:
        for (int i = 0; i < len; i++)
            if (digit(key, i) != own)
                m |= 1u << i;
        break;
    case SEM_PUSH:
        for (int i = 0; i < len && digit(key, i) == 0; i++)
            m |= 1u << i;
        break;
    case SEM_CAPT:
        for (int i = 0; i < len; i++)
            if (digit(key, i) == foe)
                m |= 1u << i;
        break;
    case SEM_CASTLE:
        m = key == 0 ? 2 : 0;
        break;
    case SEM_HIT:
        for (int dir = -1; dir <= 1; dir += 2)
            for (int i = pos + dir; i >= 0 && i < len; i += dir) {
                int d = digit(key, i);
                if (d == foe)
                    m |= 1u << i;
                if (d)
                    break;
            }
        break;
    }
    return m;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "mmap.bin";

    geometry_init();

    uint8_t *table = calloc(TABLE_SIZE, 1);
    for (int sem = 0; sem < NSEM; sem++)
        for (int s = 0; s < 2; s++)
            for (int len = 1; len <= MAXLEN; len++)
                for (uint32_t key = 0; key < POW3[len]; key++)
                    for (int pos = 0; pos < MAXLEN; pos++) {
                        uint32_t i = key * MAXLEN + pos;
                        if (sem == SEM_KING) {
                            uint64_t e = king_info(s, len, key, pos);
                            memcpy(table + BASE[sem][s][len] + i * 8, &e, 8);
                        } else {
                            table[BASE[sem][s][len] + i] = semantic(sem, s, len, key, pos);
                        }
                    }

    MMapHeader h = { .size = TABLE_SIZE };
    memcpy(h.magic, MMAP_MAGIC, 8);

    FILE *f = fopen(path, "wb");
    if (!f || fwrite(&h, sizeof h, 1, f) != 1 ||
        fwrite(table, 1, TABLE_SIZE, f) != TABLE_SIZE || fclose(f)) {
        perror(path);
        return 1;
    }
    printf("%s: %u byte, %d chiavi, %d viste\n", path, TABLE_SIZE, NKEYS, NVIEWS);
    free(table);
    return 0;
}
