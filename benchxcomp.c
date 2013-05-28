/*
 * benchx -- a program to benchmark low-level drawing operations.
 *
 *  Copyright 2013 Harm Hanemaaijer <fgenfb@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#define bool int
#define false 0
#define true 1

#define MAX_RESULTS 1024

typedef struct {
    int nu_results;
    int test[MAX_RESULTS];
    int size[MAX_RESULTS];
    double ops_sec[MAX_RESULTS];
    int cpu[MAX_RESULTS];
} BenchResults;

#define NU_TEST_TYPES 18

static const char *test_name[NU_TEST_TYPES] = {
    "ScreenCopy", "AlignedScreenCopy", "FillRect", "PutImage", "ShmPutImage", "AlignedShmPutImage",
    "ShmPixmapToScreenCopy", "AlignedShmPixmapToScreenCopy", "PixmapCopy", "PixmapFillRect",
    "Point", "Line", "FillCircle", "Text8x13", "Text10x20", "XRenderShmImage", "XRenderShmPixmap",
    "XRenderShmPixmapAlpha"
};

static int lookup_test_name(const char *name) {
    for (int i = 0; i < NU_TEST_TYPES; i++)
        if (strcasecmp(name, test_name[i]) == 0)
            return i;
    return - 1;
}

static void bench_results_initialize(BenchResults *b) {
    b->nu_results = 0;
}

static void bench_results_add(BenchResults *b, int test, int size, double ops_sec) {
    if (b->nu_results == MAX_RESULTS) {
        printf("Results array overflow.\n");
        return;
    }
    b->test[b->nu_results] = test;
    b->size[b->nu_results] = size;
    b->ops_sec[b->nu_results] = ops_sec;
    b->cpu[b->nu_results] = - 1;
    b->nu_results++;
}

static void bench_results_add_full(BenchResults *b, int test, int size, double ops_sec, int cpu) {
    if (b->nu_results == MAX_RESULTS) {
        printf("Results array overflow.\n");
        return;
    }
    b->test[b->nu_results] = test;
    b->size[b->nu_results] = size;
    b->ops_sec[b->nu_results] = ops_sec;
    b->cpu[b->nu_results] = cpu;
    b->nu_results++;
}

static int bench_results_lookup(BenchResults *b, int test, int size) {
    for (int i = 0; i < b->nu_results; i++)
        if (b->test[i] == test && b->size[i] == size)
            return i;
    return - 1;
}

static void usage() {
    printf("benchxcomp -- compare benchx output files\n"
        "Usage:\n"
        "benchxcomp <filename1> <filename2>\n");
}

static void process_file(FILE *f, BenchResults *b) {
    bench_results_initialize(b);
    for (;;) {
        if (feof(f))
            break;
        char s[256];
        fgets(s, 256, f);
        char name[256];
        int width, height;
        double ops_sec, bandwidth;
        int cpu_user, cpu_sys, cpu_total;
        int n = sscanf(s, "%s (%d x %d): %lf ops/sec (%lf MB/s), CPU %d%% + %d%% = %d%%",
            name, &width, &height, &ops_sec, &bandwidth, &cpu_user, &cpu_sys, &cpu_total);
        if (n >= 5) {
            int test = lookup_test_name(name);
            if (test >= 0) {
                if (n == 8)
                    bench_results_add_full(b, test, width, ops_sec, cpu_total);
                else
                    bench_results_add(b, test, width, ops_sec);
            }
        }
    }
}

void compare_results(BenchResults *b1, BenchResults *b2) {
    for (int i = 0; i < b1->nu_results; i++) {
         int j = bench_results_lookup(b2, b1->test[i], b1->size[i]);
         if (j == - 1)
             continue;
         bool speed_up = false;
         bool slow_down = false;
         bool cpu_diff = false;
         if (b2->ops_sec[j] > b1->ops_sec[i] * 1.05)
             speed_up = true;
         if (b2->ops_sec[j] < b1->ops_sec[i] * (1 / 1.05))
             slow_down = true;
         if (abs(b1->cpu[i] - b2->cpu[j]) >= 10)
             cpu_diff = true;
         if (speed_up || slow_down || cpu_diff)
             printf("%s (%d x %d):", test_name[b1->test[i]], b1->size[i], b1->size[i]);
         else
             continue;
         if (speed_up)
             printf(" Speed up %.0lf%%", (b2->ops_sec[j] / b1->ops_sec[i] - 1.0) * 100.0);
         if (slow_down)
             printf(" Slow down %.0lf%%", (1.0 - b2->ops_sec[j] / b1->ops_sec[i]) * 100.0);
         if (cpu_diff) {
             if (speed_up || slow_down)
                 printf(",");
             if (b2->cpu[j] > b1->cpu[i])
                 printf(" CPU usage increase from");
             else
                 printf(" CPU usage decrease from");
             printf(" %d%% to %d%%", b1->cpu[i], b2->cpu[j]);
         }
         printf("\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage();
        return 0;
    }
    FILE *f1 = fopen(argv[1], "r");
    if (f1 == NULL) {
        printf("Could open file %s.\n", argv[1]);
        return 1;
    }
    FILE *f2 = fopen(argv[2], "r");
    if (f2 == NULL) {
        printf("Could open file %s.\n", argv[2]);
        return 1;
    }
    BenchResults *results1 = malloc(sizeof(BenchResults));
    BenchResults *results2 = malloc(sizeof(BenchResults));
    process_file(f1, results1);
    printf("Processed %d results in %s.\n", results1->nu_results, argv[1]);
    process_file(f2, results2);
    printf("Processed %d results in %s.\n", results2->nu_results, argv[2]);
    compare_results(results1, results2);
    fclose(f1);
    fclose(f2);
}
