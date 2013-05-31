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

/*
 * This program is loosely based on benchimagemark, which has the following
 * copyright notice:
 * 
 * Copyright (C) 2009 Mandriva SA
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

/*
 * The size of the area on the root window into which will be drawn.
 * At the moment the width must be equal to the width.
 */
#define DEFAULT_AREA_WIDTH 600
#define DEFAULT_AREA_HEIGHT 600

/* The default running time in seconds for each subtest. */
#define DEFAULT_TEST_DURATION 5

#define _POSIX_SOURCE
#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE_EXTENDED
#define _ISOC99_SOURCE
/* The following is for getpagesize(). */
#define _BSD_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>
#include <glob.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xcomposite.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

#include <errno.h>
#include <string.h>

#include <assert.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>

#include "font.h"

#define bool int
#define true 1
#define false 0

static Display *display = NULL;
static Window root_window = None;
static Visual *visual = NULL, *visual_alpha = NULL;
static unsigned int depth;
static unsigned int bpp;
static bool visual_is_BGR = false;
static int screen_width, screen_height;
static Window window;
static GC window_gc = None;

static int feature_composite = False;
static int feature_render = False;
static int feature_shm_pixmap = False;
static int feature_shm = False;

static XGCValues gcvalues;
static GC pixmap1_gc = None, pixmap2_gc = None, pixmap2_alpha_gc = None;

static XImage *ximage = NULL;
static XImage *shmximage_ximage = NULL, *shmximage_ximage_alpha;
static XImage *shmximage_pixmap = NULL, *shmximage_pixmap_alpha = NULL;

static Pixmap pixmap1 = None;
static Pixmap pixmap2 = None, pixmap2_alpha = None;
static Pixmap shmpixmap = None, shmpixmap_alpha = None;

static uint8_t *data = NULL;
static uint8_t *shmdata_ximage = NULL, *shmdata_ximage_alpha = NULL;
static uint8_t *shmdata_pixmap = NULL, *shmdata_pixmap_alpha = NULL;

static Picture pixmap2_pict, pixmap2_alpha_pict;
static Picture shmpixmap_pict;
static Picture shmpixmap_alpha_pict;
static Picture window_pict;

static int X_pid;
static int test_duration = DEFAULT_TEST_DURATION;
static int window_mode = 0;
static int area_width = DEFAULT_AREA_WIDTH;
static int area_height = DEFAULT_AREA_HEIGHT;
static XFontStruct *X_font_8x13, *X_font_10x20;

/*
 * Get pid of process by name.
 */

pid_t find_pid(const char *process_name) {
    pid_t pid = -1;
    glob_t pglob;
    char *procname, *readbuf;
    int buflen = strlen(process_name) + 2;
    unsigned i;

    /* Get a list of all comm files. man 5 proc */
    if (glob("/proc/*/comm", 0, NULL, &pglob) != 0)
        return pid;

    /* The comm files include trailing newlines, so... */
    procname = malloc(buflen);
    strcpy(procname, process_name);
    procname[buflen - 2] = '\n';
    procname[buflen - 1] = 0;

    /* readbuff will hold the contents of the comm files. */
    readbuf = malloc(buflen);

    for (i = 0; i < pglob.gl_pathc; ++i) {
        FILE *comm;
        char *ret;

        /* Read the contents of the file. */
        if ((comm = fopen(pglob.gl_pathv[i], "r")) == NULL)
            continue;
        ret = fgets(readbuf, buflen, comm);
        fclose(comm);
        if (ret == NULL)
            continue;

        /*
           If comm matches our process name, extract the process ID from the
           path, convert it to a pid_t, and return it.
         */
        if (strcmp(readbuf, procname) == 0) {
            pid = (pid_t) atoi(pglob.gl_pathv[i] + strlen("/proc/"));
            break;
        }
    }

    /* Clean up. */
    free(procname);
    free(readbuf);
    globfree(&pglob);

    return pid;
}

/*
 * CPU usage calculation module.
 */

struct pstat {
    uint64_t utime_ticks;
    int64_t cutime_ticks;
    uint64_t stime_ticks;
    int64_t cstime_ticks;
    uint64_t vsize;             // virtual memory size in bytes
    uint64_t rss;               //Resident  Set  Size in bytes

    uint64_t cpu_total_time;
};

/*
 * read /proc data into the passed struct pstat
 * returns 0 on success, -1 on error
*/
int get_usage(const pid_t pid, struct pstat *result) {
    //convert  pid to string
    char pid_s[20];
    snprintf(pid_s, sizeof(pid_s), "%d", pid);
    char stat_filepath[30] = "/proc/";
    strncat(stat_filepath, pid_s,
        sizeof(stat_filepath) - strlen(stat_filepath) - 1);
    strncat(stat_filepath, "/stat", sizeof(stat_filepath) -
        strlen(stat_filepath) - 1);

    FILE *fpstat = fopen(stat_filepath, "r");
    if (fpstat == NULL) {
        perror("FOPEN ERROR ");
        return -1;
    }

    FILE *fstat = fopen("/proc/stat", "r");
    if (fstat == NULL) {
        perror("FOPEN ERROR ");
        fclose(fstat);
        return -1;
    }
    //read values from /proc/pid/stat
    bzero(result, sizeof(struct pstat));
    int64_t rss;
    if (fscanf(fpstat, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu"
            "%lu %ld %ld %*d %*d %*d %*d %*u %lu %ld",
            &result->utime_ticks, &result->stime_ticks,
            &result->cutime_ticks, &result->cstime_ticks, &result->vsize,
            &rss) == EOF) {
        fclose(fpstat);
        return -1;
    }
    fclose(fpstat);
    result->rss = rss * getpagesize();

    //read+calc cpu total time from /proc/stat
    uint64_t cpu_time[10];
    bzero(cpu_time, sizeof(cpu_time));
    if (fscanf(fstat, "%*s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
            &cpu_time[0], &cpu_time[1], &cpu_time[2], &cpu_time[3],
            &cpu_time[4], &cpu_time[5], &cpu_time[6], &cpu_time[7],
            &cpu_time[8], &cpu_time[9]) == EOF) {
        fclose(fstat);
        return -1;
    }

    fclose(fstat);

    for (int i = 0; i < 10; i++)
        result->cpu_total_time += cpu_time[i];

    return 0;
}

/*
* calculates the elapsed CPU usage between 2 measuring points. in percent
*/
void calc_cpu_usage_pct(const struct pstat *cur_usage,
    const struct pstat *last_usage, double *ucpu_usage, double *scpu_usage) {
    const uint64_t total_time_diff = cur_usage->cpu_total_time -
        last_usage->cpu_total_time;

    *ucpu_usage = 100 * (((cur_usage->utime_ticks + cur_usage->cutime_ticks)
            - (last_usage->utime_ticks + last_usage->cutime_ticks))
        / (double)total_time_diff);

    *scpu_usage = 100 * ((((cur_usage->stime_ticks + cur_usage->cstime_ticks)
                - (last_usage->stime_ticks + last_usage->cstime_ticks))) /
        (double)total_time_diff);
}

static void flush_xevent(Display * d) {
    /* flush events */
    for (;;) {
        XEvent e;

        /* check X event */
        if (!XPending(d)) {
            break;
        }

        XNextEvent(d, &e);
//    printf("event %d\n", e.type);
    }
}

/* Return 32bpp pixel value corresponding to RGB values for the used visual. */

static uint32_t pixel_32bpp_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (!visual_is_BGR)
        return b | (g << 8) | (r << 16) | (a << 24);
    else
        return r | (g << 8) | (b << 16) | (a << 24);
}

/*
 * Text drawing module.
 */

static int nu_lines = 0;

static void draw_text(int x, int y, const char *s) {
    /* Set foreground color to white. */
    XGCValues values;
    values.foreground = 0x00FFFFFF;
    XChangeGC(display, window_gc, GCForeground, &values);
    /* Draw the text. */
    int len = strlen(s);
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n')
            break;
        if (x + i * 8 + 8 > screen_width)
            break;
        for (int cy = 0; cy < 8; cy++)
            for (int cx = 0; cx < 8; cx++)
                if (fontdata_8x8[(int)s[i] * 8 + cy] & (0x80 >> cx))
                    XDrawPoint(display, window, window_gc, x + i * 8 + cx,
                        y + cy);
    }
}

static void print_text_graphical(const char *s) {
    /* Don't output graphical text when running in window mode. */
    if (window_mode)
        return;
    int max_lines = screen_height / 10;
    if (nu_lines == max_lines) {
        /* Scroll. */
        XCopyArea(display, window, window, window_gc,
            area_width + 16, 10,
            screen_width - (area_width + 16), (max_lines - 1) * 10,
            area_width + 16, 0);
        XGCValues values;
        values.foreground = 0;
        XChangeGC(display, window_gc, GCForeground, &values);
        XFillRectangle(display, window, window_gc,
            area_width + 16, (max_lines - 1) * 10,
            screen_width - (area_width + 16), 10);
        nu_lines--;
    }
    draw_text(area_width + 16, nu_lines * 10, s);
    nu_lines++;
}

#define NU_TESTS 22
#define NU_CORE_TESTS 12
#define NU_TEST_NAMES 24
#define TEST_SCREENCOPY 0
#define TEST_ALIGNEDSCREENCOPY 1
#define TEST_SCREENCOPYDOWNWARDS 2
#define TEST_SCREENCOPYRIGHTWARDS 3
#define TEST_FILLRECT 4
#define TEST_PUTIMAGE 5
#define TEST_SHMPUTIMAGE 6
#define TEST_ALIGNEDSHMPUTIMAGE 7
#define TEST_SHMPIXMAPTOSCREENCOPY 8
#define TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY 9
#define TEST_PIXMAPCOPY 10
#define TEST_PIXMAPFILLRECT 11
#define TEST_POINT 12
#define TEST_LINE 13
#define TEST_FILLCIRCLE 14
#define TEST_TEXT8X13 15
#define TEST_TEXT10X20 16
#define TEST_XRENDERSHMIMAGE 17
#define TEST_XRENDERSHMIMAGEALPHA 18
#define TEST_XRENDERSHMPIXMAP 19
#define TEST_XRENDERSHMPIXMAPALPHA 20
#define TEST_XRENDERSHMPIXMAPALPHATOPIXMAP 21
#define TEST_CORE 22
#define TEST_ALL 23

static const char *test_name[] = {
    "ScreenCopy", "AlignedScreenCopy", "ScreenCopyDownwards", "ScreenCopyRightwards",
    "FillRect", "PutImage", "ShmPutImage",
    "AlignedShmPutImage", "ShmPixmapToScreenCopy", "AlignedShmPixmapToScreenCopy",
    "PixmapCopy", "PixmapFillRect", "Point", "Line", "FillCircle", "Text8x13",
    "Text10x20", "XRenderShmImage", "XRenderShmImageAlpha", "XRenderShmPixmap",
    "XRenderShmPixmapAlpha", "XRenderShmPixmapAlphaToPixmap",
    "Core", "All"
};

static const char *image_string_text = {
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
};

static void test_iteration(int test, int i, int w, int h) {
    switch (test) {
    case TEST_SCREENCOPY:
        XCopyArea(display,
            window, window, window_gc, i & 7, 1, w, h, (i / 8) & 7, 0);
        break;
    case TEST_ALIGNEDSCREENCOPY:
        XCopyArea(display, window, window, window_gc, i & 7, 1, w, h, i & 7, 0);
        break;
    case TEST_SCREENCOPYDOWNWARDS:
        XCopyArea(display,
            window, window, window_gc, i & 7, 0, w, h, (i / 8) & 7, 1);
        break;
    case TEST_SCREENCOPYRIGHTWARDS:
        XCopyArea(display,
            window, window, window_gc, i & 3, 0, w, h, (i & 3) + 1 + ((i & 12) >> 2), 0);
        break;
    case TEST_FILLRECT:
        XFillRectangle(display, window, window_gc, i & 7, ((i / 8) & 7), w, h);
        break;
    case TEST_PUTIMAGE:
        XPutImage(display,
            window, window_gc, ximage, 0, 0, (i & 7), ((i / 8) & 7), w, h);
        break;
    case TEST_SHMPUTIMAGE:
        XShmPutImage(display,
            window,
            window_gc,
            shmximage_ximage, 0, 0, (i & 7), ((i / 8) & 7), w, h, False);
        break;
    case TEST_ALIGNEDSHMPUTIMAGE:
        XShmPutImage(display,
            window,
            window_gc,
            shmximage_ximage, 0, 0, (i & 1) * 8, ((i / 8) & 7), w, h, False);
        break;
    case TEST_SHMPIXMAPTOSCREENCOPY:
        XCopyArea(display,
            shmpixmap, window, window_gc, 0, 0, w, h, i & 7, ((i / 8) & 7));
        break;
    case TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY:
        XCopyArea(display,
            shmpixmap, window,
            window_gc, 0, 0, w, h, (i & 1) * 8, ((i / 8) & 7));
        break;
    case TEST_PIXMAPCOPY:
        XCopyArea(display,
            pixmap1, pixmap2, pixmap2_gc, 0, 0, w, h, i & 7, ((i / 8) & 7));
        break;
    case TEST_PIXMAPFILLRECT:
        XFillRectangle(display, pixmap2,
            pixmap2_gc, i & 7, ((i / 8) & 7), w, h);
        break;
    case TEST_POINT:
        XDrawPoint(display, window, window_gc, i & 7, ((i / 8) & 7));
        break;
    case TEST_LINE:
        XDrawLine(display, window, window_gc,
            (i & 1) * w + (i & 7), ((i & 2) >> 1) * h + ((i / 8) & 7),
            ((i & 1) ^ 1) * w + (i & 7),
            (((i & 2) >> 1) ^ 1) * h + ((i / 8) & 7));
        break;
    case TEST_FILLCIRCLE:
        XFillArc(display, window, window_gc,
            i & 7, (i / 8) & 7, w, h, 0, 360 * 64);
        break;
    case TEST_TEXT8X13:
        for (int j = 0; j < h; j++)
            XDrawImageString(display, window, window_gc,
                i & 7, X_font_8x13->ascent + j * 13, image_string_text, w);
        break;
    case TEST_TEXT10X20:
        for (int j = 0; j < h; j++)
            XDrawImageString(display, window, window_gc,
                i & 7, X_font_10x20->ascent + j * 20, image_string_text, w);
        break;
    case TEST_XRENDERSHMIMAGE:
        /* First copy the image to the pixmap. */
        XShmPutImage(display,
            pixmap2, pixmap2_gc, shmximage_ximage, 0, 0, 0, 0, w, h, False);
        /* Then copy the pixmap to the screen using XRender. */
        XRenderComposite(display,
            PictOpSrc,
            pixmap2_pict,
            None, window_pict, 0, 0, 0, 0, i & 7, ((i / 8) & 7), w, h);
        break;
    case TEST_XRENDERSHMIMAGEALPHA :
        /* First copy the image to the pixmap. */
        XShmPutImage(display,
            pixmap2_alpha, pixmap2_alpha_gc, shmximage_ximage_alpha, 0, 0, 0, 0,
            w, h, False);
        /* Then copy the pixmap to the screen using XRender. */
        XRenderComposite(display,
            PictOpOver,
            pixmap2_alpha_pict,
            None, window_pict, 0, 0, 0, 0, i & 7, ((i / 8) & 7), w, h);
        break;
    case TEST_XRENDERSHMPIXMAP:
        XRenderComposite(display,
            PictOpSrc,
            shmpixmap_pict,
            None, window_pict, 0, 0, 0, 0, i & 7, ((i / 8) & 7), w, h);
        break;
    case TEST_XRENDERSHMPIXMAPALPHA:
        XRenderComposite(display,
            PictOpOver,
            shmpixmap_alpha_pict,
            None, window_pict, 0, 0, 0, 0, i & 7, ((i / 8) & 7), w, h);
        break;
    case TEST_XRENDERSHMPIXMAPALPHATOPIXMAP:
        XRenderComposite(display,
            PictOpOver,
            shmpixmap_alpha_pict,
            None, pixmap2_pict, 0, 0, 0, 0, i & 7, ((i / 8) & 7), w, h);
        break;
    }
}

void do_test(int test, int subtest, int w, int h) {
    int nu_iterations = 1024;
    int operation_count;
    int area = w * h;
    /*
     * Set the number iterations to queue before calling
     * XFlush and checking the duration.
     * This does not define the running time; test_duration does.
     */
    switch (test) {
    case TEST_SCREENCOPY:
    case TEST_ALIGNEDSCREENCOPY:
    case TEST_SCREENCOPYDOWNWARDS:
        nu_iterations = 64;
        if (area < 10000)
            nu_iterations = 1024;
        if (area < 1000)
            nu_iterations = 8192;
        break;
    case TEST_SCREENCOPYRIGHTWARDS:
        nu_iterations = 16;
        if (area < 100000)
            nu_iterations = 64;
        if (area < 10000)
            nu_iterations = 256;
        if (area < 1000)
            nu_iterations = 1024;
        if (area < 100)
            nu_iterations = 4096;
        break;
    case TEST_FILLRECT:
    case TEST_PIXMAPFILLRECT:
        nu_iterations = 128;
        if (area < 100000)
            nu_iterations = 512;
        if (area < 10000)
            nu_iterations = 8192;
        if (area < 1000)
            nu_iterations = 32768;
        break;
    case TEST_PUTIMAGE:
        nu_iterations = 64;
        if (area < 10000)
            nu_iterations = 512;
        if (area < 1000)
            nu_iterations = 2048;
        break;
    case TEST_SHMPUTIMAGE:
    case TEST_ALIGNEDSHMPUTIMAGE:
    case TEST_SHMPIXMAPTOSCREENCOPY:
    case TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY:
    case TEST_PIXMAPCOPY:
    case TEST_XRENDERSHMIMAGE:
    case TEST_XRENDERSHMIMAGEALPHA:
    case TEST_XRENDERSHMPIXMAP:
    case TEST_XRENDERSHMPIXMAPALPHA:
    case TEST_XRENDERSHMPIXMAPALPHATOPIXMAP:
        nu_iterations = 128;
        if (area < 100000)
            nu_iterations = 512;
        if (area < 10000)
            nu_iterations = 8192;
        if (area < 1000)
            nu_iterations = 32768;
        break;
    case TEST_POINT:
        nu_iterations = 32768;
        break;
    case TEST_LINE:
        nu_iterations = 1024;
        if (w < 300)
            nu_iterations = 2048;
        if (w < 100)
            nu_iterations = 8192;
        if (w < 10)
            nu_iterations = 32768;
        break;
    case TEST_FILLCIRCLE:
        nu_iterations = 64;
        if (area < 10000)
            nu_iterations = 1024;
        if (area < 1000)
            nu_iterations = 8192;
        break;
    case TEST_TEXT8X13:
    case TEST_TEXT10X20:
        nu_iterations = 128;
        if (w < 30)
            nu_iterations = 512;
        if (w < 10)
            nu_iterations = 2048;
        break;
    }

    unsigned int c = rand();
    for (int i = 0; i < area_width * area_height * (bpp / 8); i++) {
        *((unsigned char *)data + i) = c & 0xFF;
        c += 0x7E7E7E7E;
        if ((i & 255) == 255)
            c = rand();
    }
    XPutImage(display,
        window, window_gc, ximage, 0, 0, 0, 0, area_width, area_height);
    if (test == TEST_SHMPUTIMAGE || test == TEST_ALIGNEDSHMPUTIMAGE ||
    test == TEST_XRENDERSHMIMAGE) {
        unsigned int c = rand();
        for (int i = 0; i < area_width * area_height * (bpp / 8); i++) {
            *((unsigned char *)shmdata_ximage + i) = c & 0xFF;
            c += 0x7E7E7E7E;
            if ((i & 255) == 255)
                c = rand();
        }
    }
    if (test == TEST_XRENDERSHMIMAGEALPHA) {
        unsigned int c = rand();
        for (int i = 0; i < area_width * area_height * (bpp / 8); i++) {
            *((unsigned char *)shmdata_ximage_alpha + i) = c & 0xFF;
            c += 0x7E7E7E7E;
            if ((i & 255) == 255)
                c = rand();
        }
    }
    if (test == TEST_SHMPIXMAPTOSCREENCOPY
    || test == TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY
    || test == TEST_XRENDERSHMPIXMAP) {
        unsigned int c = rand();
        for (int i = 0; i < area_width * area_height * (bpp / 8); i++) {
            *((unsigned char *)shmdata_pixmap + i) = rand() & 0xFF;
            c += 0x7E7E7E7E;
            if ((i & 255) == 255)
                c = rand();
        }
    }
    if (test == TEST_XRENDERSHMPIXMAPALPHA
    || test == TEST_XRENDERSHMPIXMAPALPHATOPIXMAP) {
        unsigned int c = rand();
        for (int i = 0; i < area_width * area_height * (bpp / 8); i++) {
            *((unsigned char *)shmdata_pixmap_alpha + i) = rand() & 0xFF;
            c += 0x7E7E7E7E;
            if ((i & 255) == 255)
                c = rand();
        }
    }
    XGCValues values;
    if (test == TEST_PIXMAPCOPY) {
        values.foreground = rand() & 0xFFFFFF;
        XChangeGC(display, pixmap1_gc, GCForeground, &values);
        XFillRectangle(display, pixmap1, pixmap1_gc, 0, 0, area_width, area_height);
    }
    /* Draw a dot to indicate which test is being run. */
    values.foreground = 0xFFFFFFFF;
    XChangeGC(display, window_gc, GCForeground, &values);
    XFillRectangle(display, window,
        window_gc, test * 16 + 8, area_height + 8, 8, 8);
    /* Set a random foreground color for tests that require it. */
    if (test == TEST_FILLRECT || test == TEST_POINT || test == TEST_LINE
    || test == TEST_FILLCIRCLE) {
        values.foreground = rand();
        XChangeGC(display, window_gc, GCForeground, &values);
    }
    if (test == TEST_TEXT8X13 || test == TEST_TEXT10X20) {
        values.foreground = 0xFFFFFF;
        values.background = 0;
        XChangeGC(display, window_gc, GCForeground | GCBackground, &values);
    }
    if (test == TEST_PIXMAPFILLRECT) {
        values.foreground = rand();
        XChangeGC(display, pixmap2_gc, GCForeground, &values);
    }
    if (test == TEST_LINE) {
        values.line_width = 1;
        XChangeGC(display, window_gc, GCLineWidth, &values);
    }
    if (test == TEST_FILLCIRCLE) {
        values.arc_mode = ArcChord;
        XChangeGC(display, window_gc, GCArcMode, &values);
    }
    if (test == TEST_TEXT8X13) {
        XSetFont(display, window_gc, X_font_8x13->fid);
    }
    if (test == TEST_TEXT10X20) {
        XSetFont(display, window_gc, X_font_10x20->fid);
    }

    /* Warm-up caches etc. */
    for (int i = 0; i < 8; i++)
        test_iteration(test, i, w, h);
    XSync(display, False);

    struct pstat usage_before, usage_after;
    /* Initialize CPU usage statistics for the X server. */
    int no_usage;
    if (X_pid == -1)
        no_usage = 1;
    else
        no_usage = get_usage(X_pid, &usage_before);

    struct timespec begin;
    struct timespec end;
    struct timespec duration;
    /* Make sure no pending request is remaining. */
    flush_xevent(display);
    XFlush(display);
    XSync(display, False);
    clock_gettime(CLOCK_REALTIME, &begin);
    operation_count = 0;
    for (;;) {
        struct timespec current;
        for (int i = 0; i < nu_iterations; i++) {
            test_iteration(test, i, w, h);
            operation_count++;
        }
        XFlush(display);
        clock_gettime(CLOCK_REALTIME, &current);
        if (current.tv_sec > begin.tv_sec + test_duration ||
        (current.tv_sec == begin.tv_sec + test_duration &&
        current.tv_nsec >= begin.tv_nsec))
            break;
    }
    /* Make sure no pending request is remaining. */
    XSync(display, False);
    clock_gettime(CLOCK_REALTIME, &end);
    double begin_t =
        (double)begin.tv_sec + (double)begin.tv_nsec / 1000000000.0;
    double end_t = (double)end.tv_sec + (double)end.tv_nsec / 1000000000.0;
    double dt = end_t - begin_t;
    char s[256];
    int pixels;
    if (test == TEST_LINE)
        pixels = w;
    else if (test == TEST_FILLCIRCLE)
        pixels = 0.5 * M_PI * 0.25 * w * w;
    else if (test == TEST_TEXT8X13) {
        pixels = 8 * 13 * w * h;
        w *= 8;
        h *= 13;
    } else if (test == TEST_TEXT10X20) {
        pixels = 10 * 20 * w * h;
        w *= 10;
        h *= 20;
    } else
        pixels = w * h;
    if (!no_usage) {
        double ucpu_usage, scpu_usage;
        /* Get CPU usage statistics for the X server. */
        get_usage(X_pid, &usage_after);
        calc_cpu_usage_pct(&usage_after, &usage_before, &ucpu_usage,
            &scpu_usage);
        sprintf(s,
            "%s (%d x %d): %.2f ops/sec (%.2f MB/s), CPU %d%% + %d%% = %d%%",
            test_name[test], w, h, operation_count / dt,
            (operation_count / dt) * pixels * (bpp / 8) / (1024 * 1024),
            (int)ucpu_usage, (int)scpu_usage, (int)(ucpu_usage + scpu_usage));
    } else
        sprintf(s, "%s (%d x %d): %.2f ops/sec (%.2f MB/s)", test_name[test],
            w, h, operation_count / dt,
            (operation_count / dt) * pixels * (bpp / 8) / (1024 * 1024));
    printf("%s\n", s);
    fflush(stdout);
    print_text_graphical(s);
}

int check_test_available(int test) {
    if ((test == TEST_SHMPUTIMAGE || test == TEST_ALIGNEDSHMPUTIMAGE)
        && !feature_shm) {
        printf("Cannot run test %s because SHM is not supported.\n",
            test_name[test]);
        return 0;
    }
    if ((test == TEST_SHMPIXMAPTOSCREENCOPY
    || test == TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY)
    && !feature_shm_pixmap) {
        printf("Cannot run test %s because SHM pixmap is not supported.\n",
            test_name[test]);
        return 0;
    }
    if (test == TEST_TEXT8X13) {
        return X_font_8x13 != NULL;
    }
    if (test == TEST_TEXT10X20) {
        return X_font_10x20 != NULL;
    }
    if ((test == TEST_XRENDERSHMIMAGE || test == TEST_XRENDERSHMIMAGEALPHA ||
    test == TEST_XRENDERSHMPIXMAP ||
    test == TEST_XRENDERSHMPIXMAPALPHA || test == TEST_XRENDERSHMPIXMAPALPHATOPIXMAP)
    && !feature_render) {
        printf("Cannot run test %s because XRender is not supported.\n",
            test_name[test]);
        return 0;
    }
    if ((test == TEST_XRENDERSHMIMAGE|| test == TEST_XRENDERSHMIMAGEALPHA)
    && !feature_shm) {
        printf("Cannot run test %s because SHM is not supported.\n",
            test_name[test]);
        return 0;
    }
    if ((test == TEST_XRENDERSHMPIXMAP || test == TEST_XRENDERSHMPIXMAPALPHA
    || test == TEST_XRENDERSHMPIXMAPALPHATOPIXMAP) && !feature_shm_pixmap) {
        printf("Cannot run test %s because SHM pixmap is not supported.\n",
            test_name[test]);
        return 0;
    }
    if ((test == TEST_XRENDERSHMPIXMAPALPHA || test == TEST_XRENDERSHMPIXMAPALPHATOPIXMAP
    || test == TEST_XRENDERSHMIMAGEALPHA)
    && visual_alpha == NULL) {
        printf("Cannot run test %s because there is no 32bpp visual with alpha.\n",
            test_name[test]);
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    int i;
    int screen;
    Colormap colormap;
    XSetWindowAttributes winattr;

    int count;
    XVisualInfo *xvinfos;
    XVisualInfo xvinfo_template;

    XRenderPictFormat pf;
    XRenderPictFormat *pict_format = NULL, *pict_format_alpha = NULL;
    XRenderPictFormat *pict_visualformat = NULL, *pict_visualformat_alpha =
        NULL;

    XRenderPictureAttributes pict_attr;

    XShmSegmentInfo shminfo_ximage, shminfo_ximage_alpha;
    XShmSegmentInfo shminfo_pixmap, shminfo_pixmap_alpha;

    int major;
    int minor;

    int prio;
    int sched;
    struct sched_param param;
    pid_t pid;
    bool option_noalpha = false;
    bool option_noxrender = false;

    int argi = 1;
    if (argc == 1) {
        printf(
            "benchx -- a low-level X window system drawing primitive benchmark\n\n"
            "Usage: benchx <options> <testname>\nOptions:\n"
            "    --duration <seconds>\n"
            "        Specifies the duration of each subtest in seconds. Default %d.\n"
            "    --window\n"
            "        Draw on a window instead of on the root window.\n"
            "    --size <pixels>\n"
            "        Specifies the size of the area to be drawn into in pixels (n x n).\n"
            "        A larger size will allow subtests with a larger area to be performed.\n"
            "        The default is %d.\n"
            "    --noalpha\n"
            "        Do not attempt to set up support for the XRenderShmImageAlpha,\n"
            "        XRenderShmPixmapAlpha and XRenderShmPixmapAlphaToPixmap tests.\n"
            "    --noxrender\n"
            "        Do not attempt to set up support for XRender related tests. This\n"
            "        implies --noalpha.\n"
            "Tests:\n",
            DEFAULT_TEST_DURATION, DEFAULT_AREA_WIDTH);
        for (int i = 0; i < NU_TEST_NAMES; i++)
            printf("    %s\n", test_name[i]);
        return 0;
    }
    for (;;) {
        if (argi >= argc) {
            printf("No test name specified.\n");
            return 1;
        }
        if (strcasecmp(argv[argi], "--duration") == 0 && argi + 1 < argc) {
            test_duration = atoi(argv[argi + 1]);
            if (test_duration < 1 || test_duration >= 100) {
                printf("Invalid test duration of %d seconds.\n");
                return 1;
            }
            argi += 2;
            continue;
        }
        if (strcasecmp(argv[argi], "--window") == 0) {
            window_mode = 1;
            argi++;
            continue;
        }
        if (strcasecmp(argv[argi], "--size") == 0 && argi + 1 < argc) {
            int size = atoi(argv[argi + 1]);
            if (size < 100 || size > 4096) {
                printf("Invalid size of drawing area (%d)\n", size);
                return 1;
            }
            area_width = size;
            area_height = size;
            argi += 2;
            continue;
        }
        if (strcasecmp(argv[argi], "--noalpha") == 0) {
            option_noalpha = true;
            argi++;
            continue;
        }
        if (strcasecmp(argv[argi], "--noxrender") == 0) {
            option_noxrender = true;
            option_noalpha = true;
            argi++;
            continue;
        }
        break;
    }
    if (!window_mode)
        fprintf(stderr,
            "If you don't see any graphical output, the root window may be obscured.\n"
            "Results will not be meaningful. Consider using the --window option or run on a\n"
            "bare X server.\n");

    pid = getpid();

    /* open display */
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "Can't open display '%s'\n", XDisplayName(NULL));
        return 1;
    }

    assert(XBitmapPad(display) == 32);

    /*
     * Check for Composite, Render, MIT-SHM extension,
     * TODO: check version
     */

    if (!XCompositeQueryVersion(display, &major, &minor)) {
        fprintf(stderr, "No Composite extension available\n");
    } else {
        fprintf(stderr, "Composite: %d.%d\n", major, minor);
        feature_composite = True;
    }

    if (!XRenderQueryVersion(display, &major, &minor)) {
        fprintf(stderr, "No Render extension available\n");
    } else {
        fprintf(stderr, "Render: %d.%d\n", major, minor);
        feature_render = True;
//    fprintf(stderr, "Render extension is disabled in benchx.\n");
//    feature_render = False;
    }

    if (!XShmQueryVersion(display, &major, &minor, &feature_shm_pixmap)) {
        fprintf(stderr, "no MIT-SHM extension available\n");
    } else {
        fprintf(stderr, "MIT-SHM: %d.%d\n", major, minor);
        feature_shm = True;
        if (!feature_shm_pixmap) {
            fprintf(stderr, "No support for shared memory pixmap\n");
        } else {
            if (XShmPixmapFormat(display) != ZPixmap) {
                fprintf(stderr, "ShmPixmapFormat not ZPixmap\n");       /* should never happen */
            }
        }
    }

    screen = DefaultScreen(display);
    root_window = RootWindow(display, screen);
    depth = DefaultDepth(display, screen);
    if (depth != 32 && depth != 24 && depth != 16) {
        fprintf(stderr, "Unsupported depth. Depth must be 32, 24 or 16.\n");
        return 1;
    }
    bpp = depth;
    /* Hack, we assume the internal bits per pixel is 32 for depth 24. */
    if (depth == 24)
        bpp = 32;
    XWindowAttributes root_window_attr;
    XGetWindowAttributes(display, root_window, &root_window_attr);
    screen_width = root_window_attr.width;
    screen_height = root_window_attr.height;

    if (area_width + 16 > screen_width || area_height + 32 > screen_height) {
        fprintf(stderr,
            "Drawing area too large (area size %d x %d, screen size %d x %d).\n",
            area_width, area_height, screen_width, screen_height);
        return 1;
    }

    if (feature_render && option_noxrender) {
        fprintf(stderr, "XRender feature disabled with option --noxrender.\n");
        feature_render = False;
    }

    if (feature_render) {
        /* lookup a ARGB picture format */
        if (bpp == 16)
            pict_format = NULL;
        else
            pict_format = XRenderFindStandardFormat(display, PictStandardRGB24);
        if (pict_format == NULL) {
            if (bpp == 16) {
                fprintf(stderr, "Can't find standard format for RGB16\n");
                pf.type = PictTypeDirect;
                pf.depth = 16;

                pf.direct.redMask = 0x1F;
                pf.direct.greenMask = 0x3F;
                pf.direct.blueMask = 0x1F;

                pf.direct.red = 11;
                pf.direct.green = 5;
                pf.direct.blue = 0;

                pict_format = XRenderFindFormat(display,
                    (PictFormatType |
                        PictFormatDepth |
                        PictFormatRedMask | PictFormatRed |
                        PictFormatGreenMask | PictFormatGreen |
                        PictFormatBlueMask | PictFormatBlue), &pf, 0);
            } else {
                fprintf(stderr, "Can't find standard format for RGB24\n");

                /* lookup an other RGB24 picture format */
                pf.type = PictTypeDirect;
                pf.depth = 24;

//          pf.direct.alphaMask = 0xff;    
                pf.direct.redMask = 0xff;
                pf.direct.greenMask = 0xff;
                pf.direct.blueMask = 0xff;

//          pf.direct.alpha = 24;    
                pf.direct.red = 16;
                pf.direct.green = 8;
                pf.direct.blue = 0;

                pict_format = XRenderFindFormat(display, (PictFormatType | PictFormatDepth | PictFormatRedMask | PictFormatRed | PictFormatGreenMask | PictFormatGreen | PictFormatBlueMask | PictFormatBlue    /* | */
                        /* PictFormatAlphaMask | PictFormatAlpha */ ),
                    &pf, 0);
            }

            if (pict_format == NULL) {
                fprintf(stderr, "Can't find XRender picture format.\n");
                return 1;
            }
        }

        if (bpp == 32) {
            /* try to lookup a RGB visual */
            count = 0;
            xvinfo_template.screen = screen;
            xvinfo_template.depth = 24;
            xvinfo_template.bits_per_rgb = 8;

            xvinfos = XGetVisualInfo(display,
                (VisualScreenMask |
                    VisualDepthMask |
                    VisualBitsPerRGBMask), &xvinfo_template, &count);
        } else {
            visual = DefaultVisual(display, screen);
            xvinfos = malloc(sizeof(XVisualInfo) * 1);
            xvinfos[0].visual = visual;
            count = 1;
        }
        if (xvinfos == NULL) {
            fprintf(stderr, "No visual matching criteria\n");
        } else {
            for (i = 0; i < count; i++) {
                pict_visualformat =
                    XRenderFindVisualFormat(display, xvinfos[i].visual);
                if (pict_visualformat != NULL
                    && pict_visualformat->id == pict_format->id) {

                    visual = xvinfos[i].visual;
                    break;
                }
            }
        }

        /* Look up a picture and visual format for 32bpp with alpha. */
        if (bpp == 32 && option_noalpha)
            visual_alpha = NULL;
        else if (bpp == 32) {
            pict_format_alpha =
                XRenderFindStandardFormat(display, PictStandardARGB32);
            if (pict_format_alpha == NULL) {
                fprintf(stderr, "Can't find standard format for ARGB32\n");

                /* lookup an other ARGB32 picture format */
                pf.type = PictTypeDirect;
                pf.depth = 32;

                pf.direct.alphaMask = 0xff;
                pf.direct.redMask = 0xff;
                pf.direct.greenMask = 0xff;
                pf.direct.blueMask = 0xff;

                pf.direct.alpha = 24;
                pf.direct.red = 16;
                pf.direct.green = 8;
                pf.direct.blue = 0;

                pict_format_alpha = XRenderFindFormat(display,
                    (PictFormatType |
                        PictFormatDepth |
                        PictFormatRedMask | PictFormatRed |
                        PictFormatGreenMask | PictFormatGreen |
                        PictFormatBlueMask | PictFormatBlue |
                        PictFormatAlphaMask | PictFormatAlpha), &pf, 0);
            }

            if (pict_format_alpha == NULL) {
                fprintf(stderr,
                    "Can't find XRender picture format for 32bpp with alpha.\n");
                visual_alpha = NULL;
            } else {

                /* Try to lookup a RGB visual with depth 32. */
                count = 0;
                xvinfo_template.screen = screen;
                xvinfo_template.depth = 32;
                xvinfo_template.bits_per_rgb = 8;

                xvinfos = XGetVisualInfo(display,
                    (VisualScreenMask |
                        VisualDepthMask |
                        VisualBitsPerRGBMask), &xvinfo_template, &count);

                if (xvinfos == NULL) {
                    fprintf(stderr, "No visual matching criteria\n");
                } else {
                    for (i = 0; i < count; i++) {
                        pict_visualformat_alpha =
                            XRenderFindVisualFormat(display, xvinfos[i].visual);
                        if (pict_visualformat_alpha != NULL
                            && pict_visualformat_alpha->id ==
                            pict_format_alpha->id) {
                            visual_alpha = xvinfos[i].visual;
                            break;
                        }
                    }
                }
            }                   /* pict_format_alpha != NULL */
        } /* bpp == 32 */
    }

    if (visual == NULL) {
        /* No feature_render or found no visual with feature render. */
        bool try_BGR = false;
find_visual:
        if (bpp == 32) {
            /* try to lookup a RGB visual */
            count = 0;
            xvinfo_template.screen = screen;
            xvinfo_template.depth = 24;
            xvinfo_template.class = TrueColor;
            if (try_BGR) {
                xvinfo_template.red_mask = 0x0000ff;
                xvinfo_template.green_mask = 0x00ff00;
                xvinfo_template.blue_mask = 0xff0000;
            }
            else {
                xvinfo_template.red_mask = 0xff0000;
                xvinfo_template.green_mask = 0x00ff00;
                xvinfo_template.blue_mask = 0x0000ff;
            }
            xvinfo_template.bits_per_rgb = 8;

            xvinfos = XGetVisualInfo(display,
                (VisualScreenMask |
                    VisualDepthMask |
                    VisualClassMask |
                    VisualRedMaskMask |
                    VisualGreenMaskMask |
                    VisualBlueMaskMask |
                    VisualBitsPerRGBMask), &xvinfo_template, &count);
        } else {
            visual = DefaultVisual(display, screen);
            xvinfos = malloc(sizeof(XVisualInfo) * 1);
            xvinfos[0].visual = visual;
            xvinfos[0].depth = depth;
            count = 1;
        }
        if (xvinfos == NULL) {
            fprintf(stderr, "No visual matching criteria\n");
        } else {
            for (i = 0; i < count; i++) {
                if (xvinfos[i].depth == depth) {
                    visual = xvinfos[i].visual;
                    if (try_BGR)
                        visual_is_BGR = true;
                    break;
                }
            }
        }
        if (bpp == 32 && visual == NULL && !try_BGR) {
            try_BGR = true;
            goto find_visual;
        }
    }

    if (visual == NULL) {
        printf("Couldn't find a suitable visual.\n");
        return 1;
    }
    if (bpp == 32 && visual_alpha == NULL && !option_noalpha) {
        fprintf(stderr, "Couldn't find 32bpp visual with alpha.\n");
    }

    /* Create a window if necessary. */
    if (!window_mode)
        window = root_window;
    else {
        XSetWindowAttributes winattr;
        /* create the window */
        winattr.background_pixel = XBlackPixel(display, screen);
        winattr.border_pixel = XBlackPixel(display, screen);
        winattr.colormap = colormap;
        winattr.event_mask =
            ExposureMask |
            VisibilityChangeMask | StructureNotifyMask | ResizeRedirectMask;
        winattr.override_redirect = True;

        window = XCreateWindow(
            display, root_window, 0, 0, area_width + 16, area_height + 32,
            0,                  /* border_width */
            depth,              /* depth */
            InputOutput,        /* class */
            visual,             /* visual */
            (CWBackPixel | CWBorderPixel | CWEventMask | CWColormap | CWOverrideRedirect),      /* valuemask */
            &winattr);
    }

    /* create/get the colormap associated to the visual */
    colormap = XCreateColormap(display, window, visual, AllocNone);

    XInstallColormap(display, colormap);

    /* create basic XImage used for XPutImage() */
    data = (uint8_t *) malloc(area_width * area_height * (bpp / 8));
    if (data == NULL) {
        perror("malloc()");
        return 1;
    }

    ximage = XCreateImage(display, visual, depth,
        ZPixmap,                  /* format */
        0, (char *)data,
        area_width, area_height,
        32,                       /* bitmap pad: 32bits */
        area_width * (bpp / 8));  /* bytes per line */
    if (ximage == NULL) {
        fprintf(stderr, "Can't create XImage\n");
        return 1;
    }

    /*
     * create SHM XImage used for XShmPutImage(),
     * and associated shared memory segment
     */
    if (feature_shm) {
        shminfo_ximage.shmid = -1;
        shminfo_ximage.shmaddr = (char *)-1;
        shminfo_ximage.readOnly = False;

        shmximage_ximage = XShmCreateImage(display,
            visual,
            depth, ZPixmap, NULL, &shminfo_ximage, area_width, area_height);

        if (shmximage_ximage == NULL) {
            fprintf(stderr, "Can't create XImage for SHM\n");
            return 1;
        }

        shminfo_ximage.shmid =
            shmget(IPC_PRIVATE,
            shmximage_ximage->bytes_per_line * shmximage_ximage->height,
            IPC_CREAT | 0600);

        if (shminfo_ximage.shmid == -1) {
            perror("shmget()");
            return 1;
        }

        shminfo_ximage.shmaddr = shmximage_ximage->data =
            shmat(shminfo_ximage.shmid, NULL, 0);

        shmctl(shminfo_ximage.shmid, IPC_RMID, 0);

        if (shminfo_ximage.shmaddr == (void *)-1 ||
            shminfo_ximage.shmaddr == NULL) {
            perror("shmat()");
            return 1;
        }

        shmdata_ximage = shmximage_ximage->data;

        XShmAttach(display, &shminfo_ximage);

        /* For the XRenderShmImageAlpha test, create a SHM image of depth 32. */
        if (bpp == 32 && feature_render && visual_alpha != NULL) {
            shminfo_ximage_alpha.shmid = -1;
            shminfo_ximage_alpha.shmaddr = (char *)-1;
            shminfo_ximage_alpha.readOnly = False;

            shmximage_ximage_alpha = XShmCreateImage(display,
                visual_alpha,
                32,
                ZPixmap, NULL, &shminfo_ximage_alpha, area_width, area_height);

            if (shmximage_ximage_alpha == NULL) {
                fprintf(stderr,
                    "Can't create SHM XImage wth alpha\n");
                return 1;
            }

            shminfo_ximage_alpha.shmid =
                shmget(IPC_PRIVATE,
                    shmximage_ximage_alpha->bytes_per_line * shmximage_ximage_alpha->height,
                    IPC_CREAT | 0600);

            if (shminfo_ximage_alpha.shmid == -1) {
                perror("shmget()");
                return 1;
            }

            shminfo_ximage_alpha.shmaddr = shmximage_ximage_alpha->data =
                shmat(shminfo_ximage_alpha.shmid, NULL, 0);

            shmctl(shminfo_ximage_alpha.shmid, IPC_RMID, 0);

        if (shminfo_ximage_alpha.shmaddr == (void *)-1 ||
            shminfo_ximage_alpha.shmaddr == NULL) {
            perror("shmat()");
            return 1;
        }

        shmdata_ximage_alpha = shmximage_ximage_alpha->data;

        XShmAttach(display, &shminfo_ximage_alpha);
        }
    }

    /*
     * create the shared memory segment for the shared pixmap
     * the XImage created is just for convienience
     */
    if (feature_shm_pixmap) {
        /* for the XShmPixmap */
        shminfo_pixmap.shmid = -1;
        shminfo_pixmap.shmaddr = (char *)-1;
        shminfo_pixmap.readOnly = False;

        shmximage_pixmap = XShmCreateImage(display,
            visual,
            depth, ZPixmap, NULL, &shminfo_pixmap, area_width, area_height);

        if (shmximage_pixmap == NULL) {
            fprintf(stderr, "Can't create XImage for SHM Pixmap\n");
            return 1;
        }

        shminfo_pixmap.shmid =
            shmget(IPC_PRIVATE,
            shmximage_pixmap->bytes_per_line * shmximage_pixmap->height,
            IPC_CREAT | 0600);

        if (shminfo_pixmap.shmid == -1) {
            perror("shmget()");
            return 1;
        }

        shminfo_pixmap.shmaddr = shmximage_pixmap->data =
            shmat(shminfo_pixmap.shmid, NULL, 0);

        shmctl(shminfo_pixmap.shmid, IPC_RMID, 0);

        if (shminfo_pixmap.shmaddr == (void *)-1 ||
            shminfo_pixmap.shmaddr == NULL) {
            perror("shmat()");
            return 1;
        }

        XShmAttach(display, &shminfo_pixmap);

        shmdata_pixmap = shmximage_pixmap->data;

        /* Now do the same for the shared memory pixmap with alpha. */
        if (bpp == 32 && feature_render && visual_alpha != NULL) {
            /* for the XShmPixmap */
            shminfo_pixmap_alpha.shmid = -1;
            shminfo_pixmap_alpha.shmaddr = (char *)-1;
            shminfo_pixmap_alpha.readOnly = False;

            shmximage_pixmap_alpha = XShmCreateImage(display,
                visual_alpha,
                32,
                ZPixmap, NULL, &shminfo_pixmap_alpha, area_width, area_height);

            if (shmximage_pixmap_alpha == NULL) {
                fprintf(stderr,
                    "Can't create XImage for SHM Pixmap wth alpha\n");
                return 1;
            }

            shminfo_pixmap_alpha.shmid =
                shmget(IPC_PRIVATE,
                shmximage_pixmap_alpha->bytes_per_line *
                shmximage_pixmap_alpha->height, IPC_CREAT | 0600);

            if (shminfo_pixmap.shmid == -1) {
                perror("shmget()");
                return 1;
            }

            shminfo_pixmap_alpha.shmaddr = shmximage_pixmap_alpha->data =
                shmat(shminfo_pixmap_alpha.shmid, NULL, 0);

            shmctl(shminfo_pixmap_alpha.shmid, IPC_RMID, 0);

            if (shminfo_pixmap_alpha.shmaddr == (void *)-1 ||
                shminfo_pixmap_alpha.shmaddr == NULL) {
                perror("shmat()");
                return 1;
            }

            XShmAttach(display, &shminfo_pixmap_alpha);

            shmdata_pixmap_alpha = shmximage_pixmap_alpha->data;
        }
    }

    /* create the graphic context */
    gcvalues.foreground = None;
    gcvalues.background = None;
    gcvalues.function = GXcopy;
    gcvalues.plane_mask = XAllPlanes();
    gcvalues.clip_mask = None;
    gcvalues.graphics_exposures = False;        /* No NoExpose */
    window_gc = XCreateGC(display, window,
        (GCBackground |
            GCForeground |
            GCFunction |
            GCPlaneMask | GCClipMask | GCGraphicsExposures), &gcvalues);

    /* create pixmaps */
    pixmap1 = XCreatePixmap(display, window, area_width, area_height, depth);
    pixmap2 = XCreatePixmap(display, window, area_width, area_height, depth);
    if (bpp == 32 && feature_shm && feature_render && visual_alpha != NULL)
        pixmap2_alpha = XCreatePixmap(display, window, area_width, area_height, 32);
    if (bpp == 32)
        gcvalues.foreground = 0x00FFFFFF;
    else
        gcvalues.foreground = 0x0000FFFF;
    gcvalues.background = 0;
    pixmap1_gc = XCreateGC(display, pixmap1,
        (GCBackground |
            GCForeground |
            GCFunction |
            GCPlaneMask | GCClipMask | GCGraphicsExposures), &gcvalues);
    pixmap2_gc = XCreateGC(display, pixmap2,
        (GCBackground |
            GCForeground |
            GCFunction |
            GCPlaneMask | GCClipMask | GCGraphicsExposures), &gcvalues);
    if (bpp == 32 && feature_shm && feature_render && visual_alpha != NULL)
        pixmap2_alpha_gc = XCreateGC(display, pixmap2_alpha,
            (GCBackground |
            GCForeground |
            GCFunction |
            GCPlaneMask | GCClipMask | GCGraphicsExposures), &gcvalues);

    if (feature_shm_pixmap) {
        shmpixmap = XShmCreatePixmap(display,
            window,
            shminfo_pixmap.shmaddr,
            &shminfo_pixmap, area_width, area_height, depth);
        if (bpp == 32 && feature_render && visual_alpha != NULL)
            shmpixmap_alpha = XShmCreatePixmap(display,
                window,
                shminfo_pixmap_alpha.shmaddr,
                &shminfo_pixmap_alpha, area_width, area_height, 32);
    }

    if (feature_render) {
        /* create XRender Picture objects */
        pict_attr.component_alpha = False;

        pixmap2_pict = XRenderCreatePicture(display,
            pixmap2, pict_format, CPComponentAlpha, &pict_attr);

        window_pict = XRenderCreatePicture(display,
            window, pict_format, CPComponentAlpha, &pict_attr);

        if (feature_shm_pixmap) {
            shmpixmap_pict = XRenderCreatePicture(display,
                shmpixmap, pict_format, CPComponentAlpha, &pict_attr);
            if (bpp == 32 && visual_alpha != NULL) {
                pict_attr.component_alpha = True;
                shmpixmap_alpha_pict = XRenderCreatePicture(display,
                    shmpixmap_alpha,
                    pict_format_alpha, CPComponentAlpha, &pict_attr);
            }
        }
        if (bpp == 32 && feature_shm && visual_alpha != NULL) {
            pict_attr.component_alpha = True;
            pixmap2_alpha_pict = XRenderCreatePicture(display,
                pixmap2_alpha, pict_format_alpha, CPComponentAlpha, &pict_attr);
        }
    }

    if (window_mode) {
        /* map the window and wait for it */
        XMapWindow(display, window);

        /* wait for map */
        for (;;) {
            XEvent e;

            /* check X event */
            XNextEvent(display, &e);

//        printf("event %d\n", e.type);

            if (e.type == MapNotify) {
                break;
            }
        }
    }

    /*
     * get high priority
     *
     */
    int root_notice = 0;
    errno = 0;
    prio = getpriority(PRIO_PROCESS, pid);
    if (errno != 0) {
//    fprintf(stderr, "Can't get priority, harmless: %s\n", strerror(errno));
        root_notice = 1;
        prio = 0;
    }

    prio -= 20;

    if (setpriority(PRIO_PROCESS, pid, prio) == -1) {
//    fprintf(stderr, "Can't set priority, harmless: %s\n", strerror(errno));
        root_notice = 1;
    }

    /*
     * get very high priority
     *
     * any other than SCHED_OTHER could be bad.
     * in fact anything different/higher than the scheduler/priority
     * used by the X server will probably give bad result
     */

    memset(&param, 0, sizeof(struct sched_param));
    sched = sched_getscheduler(pid);
    if (sched == -1) {
//    fprintf(stderr, "Can't get scheduler, harmless: %s\n", strerror(errno));
        root_notice = 1;
    }

    if (sched != SCHED_RR) {
        param.sched_priority = sched_get_priority_max(SCHED_RR);
        if (sched_setscheduler(pid, SCHED_RR, &param) == -1) {
//      fprintf(stderr, "Can't set scheduler, harmless: %s\n", strerror(errno));
            root_notice = 1;
        }
    } else {
        if (sched_getparam(pid, &param) == -1 ||
            param.sched_priority != sched_get_priority_max(SCHED_RR)) {
            param.sched_priority = sched_get_priority_max(SCHED_RR);
            if (sched_setparam(pid, &param) == -1) {
//      fprintf(stderr, "Can't set scheduler parameters, harmless: %s\n",
//              strerror(errno));
                root_notice = 1;
            }
        }
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
//    fprintf(stderr, "Can't lock memory, harmless: %s\n",
//          strerror(errno));
        root_notice = 1;
    }
    if (root_notice) {
        fprintf(stderr,
            "Couldn't increase scheduler priority or lock memory, consider running as root.\n");
    }

    int test = -1;
    for (int i = 0; i < NU_TEST_NAMES; i++)
        if (strcasecmp(argv[argi], test_name[i]) == 0) {
            test = i;
            break;
        }
    if (test == -1) {
        printf("No valid test name specified.\n");
        return 1;
    }

    X_pid = find_pid("Xorg");
    if (X_pid == -1)
        fprintf(stderr, "Couldn't find pid of X server.\n");

    XGCValues values;
    /* Clear the screen if drawing on the root window. */
    if (!window_mode) {
        values.foreground = 0;
        XChangeGC(display, window_gc, GCForeground, &values);
        XFillRectangle(display, window, window_gc, 0, 0, screen_width, screen_height);
    }

    bool include_test[NU_TESTS];
    for (int i = 0; i < NU_TESTS; i++) {
        include_test[i] = false;
        if (test == TEST_ALL)
            include_test[i] = true;
        else if (test == TEST_CORE) {
            if (i < NU_CORE_TESTS)
                include_test[i] = true;
        } else if (i == test)
            include_test[i] = true;
        /* Draw dots to indicate which test is will be run. */
        if (include_test[i]) {
            /* Blue if it will be run. */
            if (bpp == 32)
                values.foreground = pixel_32bpp_rgba(0, 0, 0xFF, 0);
            else
                values.foreground = 0x0000001F;
        } else {
            /* Red if it will not be run. */
            if (bpp == 32)
                values.foreground = pixel_32bpp_rgba(0xFF, 0, 0, 0);
            else
                values.foreground = 0x0000F800;
        }
        XChangeGC(display, window_gc, GCForeground, &values);
        XFillRectangle(display, window,
            window_gc, i * 16 + 8, area_height + 8, 8, 8);
    }
    int nu_fonts;
    char **font_name;
    XFontStruct *font_info;
    if (include_test[TEST_TEXT8X13]) {
        font_name = XListFontsWithInfo(display,
            "-misc-fixed-*-*-*-*-13-*-*-*-c-*-iso8859-1",
            256, &nu_fonts, &font_info);
        int i;
        for (i = 0; i < nu_fonts; i++) {
            int w =
                font_info[i].max_bounds.rbearing -
                font_info[i].min_bounds.lbearing;
            int h = font_info[i].ascent + font_info[i].descent;
//            printf("%s, w = %d, h = %d\n", font_name[i], w, h);
            if (font_info[i].per_char == NULL && w == 8 && h == 13)
                break;
        }
        X_font_8x13 = NULL;
        if (i < nu_fonts)
            X_font_8x13 = XLoadQueryFont(display, font_name[i]);
        if (X_font_8x13 == NULL)
            fprintf(stderr, "Could not load 8x13 font.\n");
        XFreeFontInfo(font_name, font_info, nu_fonts);
    }
    if (include_test[TEST_TEXT10X20]) {
        font_name = XListFontsWithInfo(display,
            "-misc-fixed-*-*-*-*-20-*-*-*-c-*-iso8859-1",
            256, &nu_fonts, &font_info);
        int i;
        for (i = 0; i < nu_fonts; i++) {
            int w =
                font_info[i].max_bounds.rbearing -
                font_info[i].min_bounds.lbearing;
            int h = font_info[i].ascent + font_info[i].descent;
//            printf("%s, w = %d, h = %d\n", font_name[i], w, h);
            if (font_info[i].per_char == NULL && w == 10 && h == 20)
                break;
        }
        X_font_10x20 = NULL;
        if (i < nu_fonts)
            X_font_10x20 = XLoadQueryFont(display, font_name[i]);
        if (X_font_10x20 == NULL)
            fprintf(stderr, "Could not load 10x20 font.\n");
        XFreeFontInfo(font_name, font_info, nu_fonts);
    }

    if (test != TEST_ALL && test != TEST_CORE)
        if (!check_test_available(test))
            return 1;

    sleep(2);

    int max_size = area_width;
    if (max_size > area_height)
        max_size = area_height;

    char s[80];
    sprintf(s, "Screen size %d x %d, depth %d (%d bpp), area size %d x %d",
        screen_width, screen_height, depth, bpp, max_size, max_size);
    printf("%s\n", s);
    print_text_graphical(s);

    /*
     * Note: The destination coordinates on the root window vary from (0, 0) to (7, 7).
     */
    for (int i = 0; i < NU_TESTS; i++)
        if (include_test[i] && check_test_available(i)) {
            if (i == TEST_POINT)
                do_test(i, 0, 1, 1);
            else if (i == TEST_TEXT8X13 || i == TEST_TEXT10X20) {
                /* Size is length in characters. */
                int font_width, font_height;
                if (i == TEST_TEXT8X13) {
                    font_width = 8;
                    font_height = 13;
                } else {
                    font_width = 10;
                    font_height = 20;
                }
                int subtest = 0;
                for (int size = 8; size * font_width + 8 <= max_size;
                    size = size * 2) {
                    int lines = size * font_width / font_height;
                    do_test(i, subtest, size, lines);
                    subtest++;
                }
            } else {
                int subtest = 0;
                for (int size = 5; size + 8 <= max_size; size = size * 3 / 2) {
                    do_test(i, subtest, size, size);
                    subtest++;
                }
            }
            /* Draw a green dot to indicate the test is finished. */
            if (bpp == 32)
                values.foreground = pixel_32bpp_rgba(0, 0x7F, 0, 0);
            else
                values.foreground = 0x000003E0;
            XChangeGC(display, window_gc, GCForeground, &values);
            XFillRectangle(display, window,
                window_gc, i * 16 + 8, area_height + 8, 8, 8);
        }

    XCloseDisplay(display);

    return 0;
}
