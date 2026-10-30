/*
 * The detector's engine, on this machine, where its answers can be checked.
 *
 *   cc -O2 -I modules/detect tools/hosttest/pico_host.c modules/detect/pico.c
 *   ./a.out model.rv9m picture.ppm [boxed.ppm] [dump-dir]
 *
 * Prints what it found in ESP-DL's own format, so the lines can be put
 * beside the ones in Espressif's README; draws the boxes on a copy of the
 * picture; and, given a directory, writes every layer's output there as
 * t<index>.bin, for comparing a layer at a time with ESP-DL on the board.
 *
 * The picture is a binary PPM, given to the engine as RGB888 -- what
 * Espressif's example feeds it -- or, with PICO_565=1 in the environment,
 * first reduced to the RGB565 the camera gives.
 */
#include "pico.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(*size);
    if (fread(b, 1, *size, f) != (size_t)*size) { perror(path); exit(1); }
    fclose(f);
    return b;
}

static uint8_t *read_ppm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    int max;
    if (!f || fscanf(f, "P6 %d %d %d", w, h, &max) != 3 || max != 255) {
        fprintf(stderr, "%s: not a binary PPM\n", path);
        exit(1);
    }
    fgetc(f);
    uint8_t *px = malloc((size_t)*w * *h * 3);
    if (fread(px, 3, (size_t)*w * *h, f) != (size_t)*w * *h) exit(1);
    fclose(f);
    return px;
}

static void box(uint8_t *px, int w, int h, const pico_box_t *b)
{
    for (int t = 0; t < 3; t++) {
        for (int x = b->x1; x <= b->x2; x++)
            for (int y = 0; y < 2; y++) {
                int yy = y ? b->y2 - t : b->y1 + t;
                if (x >= 0 && x < w && yy >= 0 && yy < h)
                    memcpy(px + (yy * w + x) * 3, "\x00\xff\x00", 3);
            }
        for (int y = b->y1; y <= b->y2; y++)
            for (int x = 0; x < 2; x++) {
                int xx = x ? b->x2 - t : b->x1 + t;
                if (xx >= 0 && xx < w && y >= 0 && y < h)
                    memcpy(px + (y * w + xx) * 3, "\x00\xff\x00", 3);
            }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s model.rv9m picture.ppm [boxed.ppm] [dump-dir]\n",
                argv[0]);
        return 2;
    }
    long msize;
    void *model = slurp(argv[1], &msize);
    pico_t p;
    int rc = pico_open(&p, model, (uint32_t)msize);
    if (rc) { fprintf(stderr, "not a model (%d)\n", rc); return 1; }

    int w, h;
    uint8_t *px = read_ppm(argv[2], &w, &h);
    const void *in = px;
    int fmt = PICO_RGB888;
    if (getenv("PICO_565")) {
        uint16_t *p16 = malloc((size_t)w * h * 2);
        for (int i = 0; i < w * h; i++)
            p16[i] = (uint16_t)((px[3 * i] >> 3) << 11 | (px[3 * i + 1] >> 2) << 5 |
                                px[3 * i + 2] >> 3);
        in = p16;
        fmt = PICO_RGB565;
    }

    void *arena = calloc(1, pico_arena_bytes(&p));
    pico_box_t b[10];
    int n;
    if (argc > 4) {
        char name[512];
        pico_input(&p, arena, in, w, h, fmt);
        for (uint32_t i = 0; i < p.n_nodes; i++) {
            pico_node(&p, arena, i);
            uint32_t t = i + 1;          /* node i writes tensor i + 1 */
            int th, tw, tc, te;
            const int8_t *d = pico_tensor(&p, arena, t, &th, &tw, &tc, &te);
            snprintf(name, sizeof(name), "%s/t%03u.bin", argv[4], t);
            FILE *f = fopen(name, "wb");
            fwrite(d, 1, (size_t)th * tw * tc, f);
            fclose(f);
        }
        n = pico_post(&p, arena, w, h, b, 10);
    } else {
        n = pico_run(&p, arena, in, w, h, fmt, b, 10);
    }

    printf("outputs hash %08x\n", pico_hash(&p, arena));
    for (int i = 0; i < n; i++) {
        printf("[score: %f, x1: %d, y1: %d, x2: %d, y2: %d]\n",
               b[i].score / 65536.0, b[i].x1, b[i].y1, b[i].x2, b[i].y2);
        box(px, w, h, &b[i]);
    }
    if (argc > 3) {
        FILE *f = fopen(argv[3], "wb");
        fprintf(f, "P6 %d %d 255\n", w, h);
        fwrite(px, 3, (size_t)w * h, f);
        fclose(f);
    }
    return 0;
}
