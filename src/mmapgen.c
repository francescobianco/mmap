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
                    for (int pos = 0; pos < MAXLEN; pos++)
                        table[BASE[sem][s][len] + key * MAXLEN + pos] =
                            semantic(sem, s, len, key, pos);

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
