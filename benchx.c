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
 */
#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 600

/* The running time in seconds for each subtest. */
#define RUNNING_TIME_IN_SECONDS 5

#define _POSIX_SOURCE
#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE_EXTENDED
#define _ISOC99_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
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

static Display *display = NULL;
static Window root_window = None;
static Visual  *visual = NULL;
static unsigned int depth;
static unsigned int bpp;

static int feature_composite = False;
static int feature_render = False;
static int feature_shm_pixmap = False;
static int feature_shm = False;

static XGCValues gcvalues;
static GC gc = None;
static GC root_gc = None;

static XImage *ximage = NULL;
static XImage *shmximage_ximage = NULL;
static XImage *shmximage_pixmap = NULL;

static Pixmap pixmap1 = None;
static Pixmap pixmap2 = None;
static Pixmap shmpixmap = None;

static uint8_t *data = NULL;
static uint8_t *shmdata_ximage = NULL;
static uint8_t *shmdata_pixmap = NULL;

static Picture pixmap_pict;
static Picture shmpixmap_pict;
static Picture window_pict;

static int X_pid;

/*
 * Get pid of process by name.
 */

pid_t find_pid(const char *process_name)
{
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
            pid = (pid_t)atoi(pglob.gl_pathv[i] + strlen("/proc/"));
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
    uint64_t vsize; // virtual memory size in bytes
    uint64_t rss; //Resident  Set  Size in bytes

    uint64_t cpu_total_time;
};

/*
 * read /proc data into the passed struct pstat
 * returns 0 on success, -1 on error
*/
int get_usage(const pid_t pid, struct pstat* result) {
    //convert  pid to string
    char pid_s[20];
    snprintf(pid_s, sizeof(pid_s), "%d", pid);
    char stat_filepath[30] = "/proc/"; strncat(stat_filepath, pid_s,
            sizeof(stat_filepath) - strlen(stat_filepath) -1);
    strncat(stat_filepath, "/stat", sizeof(stat_filepath) -
            strlen(stat_filepath) -1);

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

    for(int i=0; i < 10;i++)
        result->cpu_total_time += cpu_time[i];

    return 0;
}

/*
* calculates the elapsed CPU usage between 2 measuring points. in percent
*/
void calc_cpu_usage_pct(const struct pstat* cur_usage,
                        const struct pstat* last_usage,
                        double* ucpu_usage, double* scpu_usage)
{
    const uint64_t total_time_diff = cur_usage->cpu_total_time -
                                              last_usage->cpu_total_time;

    *ucpu_usage = 100 * (((cur_usage->utime_ticks + cur_usage->cutime_ticks)
                    - (last_usage->utime_ticks + last_usage->cutime_ticks))
                    / (double) total_time_diff);

    *scpu_usage = 100 * ((((cur_usage->stime_ticks + cur_usage->cstime_ticks)
                    - (last_usage->stime_ticks + last_usage->cstime_ticks))) /
                    (double) total_time_diff);
}

static void
flush_xevent(Display *d)
{
  /* flush events */				
  for(;;) {
    XEvent e;
    
    /* check X event */
    if (!XPending(d)) {
      break;
    }
    
    XNextEvent(d, &e);
    printf("event %d\n", e.type);
  }
}

#define NU_TEST_TYPES 11
#define TEST_SCREENCOPY 0
#define TEST_ALIGNEDSCREENCOPY 1
#define TEST_FILLRECT 2
#define TEST_PUTIMAGE 3
#define TEST_SHMPUTIMAGE 4
#define TEST_ALIGNEDSHMPUTIMAGE 5
#define TEST_SHMPIXMAPTOSCREENCOPY 6
#define TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY 7
#define TEST_PIXMAPCOPY 8
#define TEST_PIXMAPFILLRECT 9
#define TEST_ALL 10

static const char *test_name[] = {
    "ScreenCopy", "AlignedScreenCopy", "FillRect", "PutImage", "ShmPutImage", "AlignedShmPutImage",
    "ShmPixmapToScreenCopy", "AlignedShmPixmapToScreenCopy", "PixmapCopy", "PixmapFillRect", "All"
};

static void test_iteration(int test, int i, int w, int h) {
            switch (test) {
            case TEST_SCREENCOPY :
                XCopyArea(display,
                    root_window, root_window,
                    root_gc,
                    i & 7, 1,
                    w, h,
                    (i / 8) & 7, 0);
                break;
            case TEST_ALIGNEDSCREENCOPY :
                XCopyArea(display,
                    root_window, root_window,
                    root_gc,
                    i & 7, 1,
                    w, h,
                    i & 7, 0);
                break;
            case TEST_FILLRECT :
                XFillRectangle(display, root_window,
                    root_gc, i & 7, ((i / 8) & 7), w, h);
                break;
            case TEST_PUTIMAGE :
                XPutImage(display,
                    root_window,
                    root_gc,
                    ximage,
                    0, 0,
                    (i & 7), ((i / 8) & 7),
                    w, h);
                break;
            case TEST_SHMPUTIMAGE :
                XShmPutImage(display,
                    root_window,
                    root_gc,
                    shmximage_ximage,
                    0, 0,
                    (i & 7), ((i / 8) & 7),
                    w, h, False);
                break;
            case TEST_ALIGNEDSHMPUTIMAGE :
                XShmPutImage(display,
                    root_window,
                    root_gc,
                    shmximage_ximage,
                    0, 0,
                    (i & 1) * 8, ((i / 8) & 7),
                    w, h, False);
                break;
            case TEST_SHMPIXMAPTOSCREENCOPY :
                XCopyArea(display,
                    shmpixmap, root_window,
                    root_gc,
                    0, 0,
                    w, h,
                    i & 7, ((i / 8) & 7));
                break;
            case TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY :
                XCopyArea(display,
                    shmpixmap, root_window,
                    root_gc,
                    0, 0,
                    w, h,
                    (i & 1) * 8, ((i / 8) & 7));
                break;
            case TEST_PIXMAPCOPY :
                XCopyArea(display,
                    pixmap1, pixmap2,
                    root_gc,
                    0, 0,
                    w, h,
                    i & 7, ((i / 8) & 7));
                break;
            case TEST_PIXMAPFILLRECT :
                XFillRectangle(display, pixmap1,
                    root_gc, i & 7, ((i / 8) & 7), w, h);
                break;
            }
}

void do_test(int test, int w, int h) {
    struct pstat usage_before, usage_after;
    /* Get CPU usage statistics for the X server */
    int no_usage;
    if (X_pid == - 1)
        no_usage = 1;
    else
        no_usage = get_usage(X_pid, &usage_before);

    int nu_iterations = 1000;
    int operation_count;
    int area = w * h;
    /*
     * Set the number iterations to queue before calling
     * XFlush and checking the duration.
     * This does not define the running time; RUNNING_TIME_IN_SECONDS
     * does.
     */
    switch (test) {
    case TEST_SCREENCOPY :
    case TEST_ALIGNEDSCREENCOPY :
        nu_iterations = 64;
        if (area < 10000)
            nu_iterations = 1024;
        if (area < 1000)
            nu_iterations = 8192;
        break;
    case TEST_FILLRECT :
    case TEST_PIXMAPFILLRECT :
        nu_iterations = 128;
        if (area < 100000)
            nu_iterations = 512;
        if (area < 10000)
            nu_iterations = 8192;
        if (area < 1000)
            nu_iterations = 32768;
        break;
    case TEST_PUTIMAGE :
        nu_iterations = 64;
        if (area < 10000)
            nu_iterations = 512;
        if (area < 1000)
            nu_iterations = 2048;
        break;
    case TEST_SHMPUTIMAGE :
    case TEST_ALIGNEDSHMPUTIMAGE :
    case TEST_SHMPIXMAPTOSCREENCOPY :
    case TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY :
        nu_iterations = 128;
        if (area < 100000)
            nu_iterations = 512;
        if (area < 10000)
            nu_iterations = 8192;
        if (area < 1000)
            nu_iterations = 32768;
        break;
    }

    unsigned int c = rand();
    for (int i = 0; i < WINDOW_WIDTH * WINDOW_HEIGHT * (bpp / 8); i++) {
            *((unsigned char *)data + i) = c & 0xFF;
            c += 0x7E7E7E7E;
            if ((i & 255) == 255)
                c = rand();
    }
    XPutImage(display,
                root_window, 
		root_gc,
		ximage, 
		0, 0, 
		0, 0, 
		WINDOW_WIDTH, WINDOW_HEIGHT);
    if (test == TEST_SHMPUTIMAGE || test == TEST_ALIGNEDSHMPUTIMAGE) {
        unsigned int c = rand();
        for (int i = 0; i < WINDOW_WIDTH * WINDOW_HEIGHT * (bpp / 8); i++) {
            *((unsigned char *)shmdata_ximage + i) = c & 0xFF;
            c += 0x7E7E7E7E;
            if ((i & 255) == 255)
                c = rand();
        }
    }
    if (test == TEST_SHMPIXMAPTOSCREENCOPY || test == TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY
    || test == TEST_PIXMAPCOPY) {
        unsigned int c = rand();
        for (int i = 0; i < WINDOW_WIDTH * WINDOW_HEIGHT * (bpp / 8); i++) {
            *((unsigned char *)shmdata_pixmap + i) = rand() & 0xFF;
            c += 0x7E7E7E7E;
            if ((i & 255) == 255)
                c = rand();
        }
    }
    XGCValues values;
    /* Draw a dot to indicate which test is being run. */
    values.foreground = 0xFFFFFFFF;
    XChangeGC(display, root_gc, GCForeground, &values);
    XFillRectangle(display, root_window,
                   root_gc, test * 16 + 8, WINDOW_HEIGHT + 8, 8, 8);
    /* For FillRect, set a random fill color. */
    if (test == TEST_FILLRECT || test == TEST_PIXMAPFILLRECT) {
        values.foreground = rand();
        XChangeGC(display, root_gc, GCForeground, &values);
    }

    /* Warm-up caches etc. */
    for (int i = 0; i < 8; i++)
        test_iteration(test, i, w, h);

    struct timespec begin;
    struct timespec end;
    struct timespec duration;
    /* be sure no pending request remaining */
    XSync(display, False);
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
        if (current.tv_sec >= begin.tv_sec + RUNNING_TIME_IN_SECONDS)
            break;
    }
    /* be sure no pending request remaining */
    XSync(display, False);
    clock_gettime(CLOCK_REALTIME, &end);
    double begin_t = (double)begin.tv_sec + (double)begin.tv_nsec / 1000000000.0;
    double end_t = (double)end.tv_sec + (double)end.tv_nsec / 1000000000.0;
    double dt = end_t - begin_t;
    if (!no_usage) {
        double ucpu_usage, scpu_usage;
        /* Get CPU usage statistics for the X server. */
        get_usage(X_pid, &usage_after);
        calc_cpu_usage_pct(&usage_after, &usage_before, &ucpu_usage, &scpu_usage);
        printf("%s (%d x %d): ops/sec: %.2f (%.2f MB/s), CPU %d%% + %d%% = %d%%\n",
            test_name[test], w, h, operation_count / dt,
            (operation_count / dt) * w * h * (bpp / 8) / (1024 * 1024),
            (int)ucpu_usage, (int)scpu_usage, (int)(ucpu_usage + scpu_usage));
    }
    else
        printf("%s (%d x %d): ops/sec: %.2f (%.2f MB/s)\n", test_name[test],
            w, h, operation_count / dt, (operation_count / dt) * w * h * (bpp / 8) / (1024 * 1024));
    fflush(stdout);
}

int check_test_available(int test) {
    if ((test == TEST_SHMPUTIMAGE || test == TEST_ALIGNEDSHMPUTIMAGE) && !feature_shm) {
        printf("Cannot run test %s because SHM is not supported.\n", test_name[test]);
        return 0;
    }
    if ((test == TEST_SHMPIXMAPTOSCREENCOPY || test == TEST_ALIGNEDSHMPIXMAPTOSCREENCOPY)
    && !feature_shm_pixmap) {
        printf("Cannot run test %s because SHM pixmap is not supported.\n", test_name[test]);
        return 0;
    }
    return 1;
}

int
main(int argc, char *argv[])
{
  int i;
  int screen;
  Colormap colormap;
  XSetWindowAttributes winattr;

  int count;
  XVisualInfo *xvinfos;
  XVisualInfo xvinfo_template;

  XRenderPictFormat pf;
  XRenderPictFormat *pict_format = NULL;
  XRenderPictFormat *pict_visualformat = NULL;
    
  XRenderPictureAttributes pict_attr;

  XShmSegmentInfo shminfo_ximage;
  XShmSegmentInfo shminfo_pixmap;
  
  int major;
  int minor;

  unsigned int width = WINDOW_WIDTH;
  unsigned int height = WINDOW_HEIGHT;

  int prio;
  int sched;
  struct sched_param param;
  pid_t pid;

    if (argc == 1) {
        printf("Usage: benchx <testname>\nTests:\n");
        for (int i = 0; i < NU_TEST_TYPES; i++)
            printf("    %s\n", test_name[i]);
        return 0;
    }

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

  if (!XCompositeQueryVersion(display,
			      &major, &minor)) {
    fprintf(stderr, "No Composite extension available\n");
  } else {
    fprintf(stderr, "Composite: %d.%d\n", major, minor);
    feature_composite = True;
  }

  if (!XRenderQueryVersion(display,
			   &major, &minor)) {
    fprintf(stderr, "No Render extension available\n");
  } else {
    fprintf(stderr, "Render: %d.%d\n", major, minor);
    feature_render = True;
    fprintf(stderr, "Render extension is disabled in benchx.\n");
    feature_render = False;
  }

  if (!XShmQueryVersion(display,
			&major, &minor,
			&feature_shm_pixmap)) {
    fprintf(stderr, "no MIT-SHM extension available\n");
  } else {
    fprintf(stderr, "MIT-SHM: %d.%d\n", major, minor);
    feature_shm = True;
    if (!feature_shm_pixmap) {
      fprintf(stderr, "No support for shared memory pixmap\n");
    } else {
      if (XShmPixmapFormat(display) != ZPixmap) {
	fprintf(stderr, "ShmPixmapFormat not ZPixmap\n"); /* should never happen */
      }
    }
  }

  screen = DefaultScreen(display);
  root_window = RootWindow(display, screen);
  depth = DefaultDepth(display, screen);
  fprintf(stderr, "Default depth = %d.\n", depth);
  if (depth != 32 && depth != 24 && depth != 16) {
      fprintf(stderr, "Unsupported depth. Depth must be 32, 24 or 16.\n");
      return 1;
  }
  bpp = depth;
  /* Hack, we assume the internal bits per pixel is 32 for depth 24. */
  if (depth == 24)
      bpp = 32;

  if (feature_render) {
    /* lookup a ARGB picture format */
    if (bpp == 16)
        pict_format = NULL;
    else
        pict_format = XRenderFindStandardFormat(display, PictStandardARGB32);
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
				       PictFormatBlueMask | PictFormatBlue),
				      &pf,
				      0);
      }
      else {
          fprintf(stderr, "Can't find standard format for ARGB32\n");
  
          /* lookup an other ARGB picture format */
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
      
          pict_format = XRenderFindFormat(display,
				      (PictFormatType |
				       PictFormatDepth |
				       PictFormatRedMask | PictFormatRed |
				       PictFormatGreenMask | PictFormatGreen |
				       PictFormatBlueMask | PictFormatBlue   |
				       PictFormatAlphaMask | PictFormatAlpha),
				      &pf,
				      0);
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
        xvinfo_template.depth = 32;
        xvinfo_template.bits_per_rgb = 8;
    
        xvinfos = XGetVisualInfo(display, 
			     (VisualScreenMask | 
			      VisualDepthMask | 
			      VisualBitsPerRGBMask),
			     &xvinfo_template,
			     &count);
    }
    else {
        visual = DefaultVisual(display, screen);
        xvinfos = malloc(sizeof(XVisualInfo) * 1);
        xvinfos[0].visual = visual;
        count = 1;
    }
    if (xvinfos == NULL) {
      fprintf(stderr, "No visual matching criteria\n");
    } else {
      for(i = 0; i < count; i++) {
	pict_visualformat = XRenderFindVisualFormat(display, xvinfos[i].visual);
	if (pict_visualformat != NULL &&
	    pict_visualformat->id == pict_format->id) {
	  
	  visual = xvinfos[i].visual;
	  break;
	}
      }
    }
  } else {
    /* No feature_render */
    if (bpp == 32) {
        /* try to lookup a RGB visual */
        count = 0;
        xvinfo_template.screen = screen;
        xvinfo_template.depth = 32;
        xvinfo_template.class = TrueColor;
        xvinfo_template.red_mask = 0xff0000;
        xvinfo_template.green_mask = 0x00ff00;
        xvinfo_template.blue_mask = 0x0000ff;
        xvinfo_template.bits_per_rgb = 8;
    
        xvinfos = XGetVisualInfo(display, 
			     (VisualScreenMask |
			      VisualDepthMask |
			      VisualClassMask |
			      VisualRedMaskMask |
			      VisualGreenMaskMask |
			      VisualBlueMaskMask |
			      VisualBitsPerRGBMask),
			     &xvinfo_template,
			     &count);
    }
    else {
        visual = DefaultVisual(display, screen);
        xvinfos = malloc(sizeof(XVisualInfo) * 1);
        xvinfos[0].visual = visual;
        xvinfos[0].depth = depth;
        count = 1;
    }
    if (xvinfos == NULL) {
      fprintf(stderr, "No visual matching criteria\n");
    } else {
      for(i = 0; i < count; i++) {      
	if (xvinfos[i].depth == bpp) {
	  visual = xvinfos[i].visual;
	  break;
	}
      }
    }
  }

  if (visual == NULL) {
    printf("No suitable visual, fatal, leaving ...\n");
    return 1;
  }

  /* create/get the colormap associated to the visual */
  colormap = XCreateColormap(display,
			     root_window,
			     visual,
			     AllocNone);

  XInstallColormap(display,
		   colormap);

  /* create basic XImage used for XPutImage() */
  data = (uint8_t *) malloc(width * height * (bpp / 8));
  if (data == NULL) {
    perror("malloc()");
    return 1;
  }

  ximage = XCreateImage(display, 
			visual,
			depth, 
			ZPixmap, /* format */
			0,
			(char *)data,
			width,
			height,
			32, /* bitmap pad: 32bits */
		        width * (bpp / 8));  /* bytes per line */
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
    shminfo_ximage.shmaddr = (char *) -1;
    shminfo_ximage.readOnly = False;
    
    shmximage_ximage = XShmCreateImage(display,
				       visual,
				       depth,
				       ZPixmap,
				       NULL,
				       &shminfo_ximage,
				       width,
				       height);
    
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
  
    if (shminfo_ximage.shmaddr == (void *) -1 ||
	shminfo_ximage.shmaddr == NULL) {
      perror("shmat()");
      return 1;
    }

    shmdata_ximage = shmximage_ximage->data;
    
    XShmAttach(display, &shminfo_ximage);
  }

  /*
   * create the shared memory segment for the shared pixmap
   * the XImage created is just for convienience
   */
  if (feature_shm_pixmap) {
    /* for the XShmPixmap */
    shminfo_pixmap.shmid = -1;
    shminfo_pixmap.shmaddr = (char *) -1;
    shminfo_pixmap.readOnly = False;
    
    shmximage_pixmap = XShmCreateImage(display,
				       visual,
				       depth,
				       ZPixmap,
				       NULL,
				       &shminfo_pixmap,
				       width,
				       height);

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
    
    if (shminfo_pixmap.shmaddr == (void *) -1 ||
	shminfo_pixmap.shmaddr == NULL) {
      perror("shmat()");
      return 1;
    }
    
    XShmAttach(display, &shminfo_pixmap);
    
    shmdata_pixmap = shmximage_pixmap->data;
  }

  /* create the graphic context */
  gcvalues.foreground = None;
  gcvalues.background = None;
  gcvalues.function = GXcopy;
  gcvalues.plane_mask = XAllPlanes();
  gcvalues.clip_mask = None;
  gcvalues.graphics_exposures = False; /* No NoExpose */
  root_gc = XCreateGC(display, root_window,
                 (GCBackground |
                  GCForeground |
                  GCFunction |
                  GCPlaneMask |
                  GCClipMask |
                  GCGraphicsExposures),
                 &gcvalues);

  /* create pixmaps */
  pixmap1 = XCreatePixmap(display,
			 root_window, 
			 width,
			 height,
			 depth);
  pixmap2 = XCreatePixmap(display,
			 root_window, 
			 width,
			 height,
			 depth);

  if (feature_shm_pixmap) {
    shmpixmap = XShmCreatePixmap(display,
				 root_window,
				 shminfo_pixmap.shmaddr,
				 &shminfo_pixmap,
				 width,
				 height,
				 depth);
  }

  if (feature_render) {
    /* create XRender Picture objects */
    pict_attr.component_alpha = False;
    
    pixmap_pict = XRenderCreatePicture(display, 
				       pixmap1,
				       pict_format,
				       CPComponentAlpha,
				       &pict_attr);
    
    window_pict = XRenderCreatePicture(display, 
				       root_window,
				       pict_format,
				       CPComponentAlpha,
				       &pict_attr);
  
    if (feature_shm_pixmap) {
      shmpixmap_pict = XRenderCreatePicture(display, 
					    shmpixmap,
					    pict_format,
					    CPComponentAlpha,
					    &pict_attr);
    }
  }

  /*
   * get high priority
   *
   */
  errno = 0;
  prio = getpriority(PRIO_PROCESS, pid);
  if (errno != 0) {
    fprintf(stderr, "Can't get priority, harmless: %s\n", strerror(errno));
    prio = 0;
  }
  
  prio -= 20;
 
  if (setpriority(PRIO_PROCESS, pid, prio) == -1) {
    fprintf(stderr, "Can't set priority, harmless: %s\n", strerror(errno));
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
    fprintf(stderr, "Can't get scheduler, harmless: %s\n", strerror(errno));
  }

  if (sched != SCHED_RR) {
    param.sched_priority = sched_get_priority_max(SCHED_RR);
    if (sched_setscheduler(pid, SCHED_RR, &param) == -1) {
      fprintf(stderr, "Can't set scheduler, harmless: %s\n", strerror(errno));
    }
  } else {
    if (sched_getparam(pid, &param) == -1 || 
	param.sched_priority != sched_get_priority_max(SCHED_RR)) {
      param.sched_priority = sched_get_priority_max(SCHED_RR);
      if (sched_setparam(pid, &param) == -1) {
	fprintf(stderr, "Can't set scheduler parameters, harmless: %s\n",
		strerror(errno));
      }
    }
  }

  if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
    fprintf(stderr, "Can't lock memory, harmless: %s\n",
	    strerror(errno));
  }

    int test = - 1;
    for (int i = 0; i < NU_TEST_TYPES; i++)
        if (strcasecmp(argv[1], test_name[i]) == 0) {
            test = i;
            break;
        }
    if (test == - 1) {
        printf("No valid test name specified.\n");
        return 1;
    }

    if (test != TEST_ALL)
        if (!check_test_available(test))
            return 1;

    X_pid = find_pid("Xorg");
    if (X_pid == - 1)
        fprintf(stderr, "Couldn't find pid of X server.\n");

    sleep(2);


    /* Draw dots to indicate which test is being run. */
    XGCValues values;
    if (bpp == 32)
        values.foreground = 0x000000FF;
    else
        values.foreground = 0x0000001F;
    XChangeGC(display, root_gc, GCForeground, &values);
    for (int i = 0; i < NU_TEST_TYPES - 1; i++) {
        XFillRectangle(display, root_window,
                       root_gc, i * 16 + 8, WINDOW_HEIGHT + 8, 8, 8);
    }

    int max_size = WINDOW_WIDTH;
    if (max_size > WINDOW_HEIGHT)
        max_size = WINDOW_HEIGHT;
    /*
     * Note: The destination coordinates on the root window vary from (0, 0) to (7, 7).
     */
    if (test == TEST_ALL) {
        for (int i = 0; i < NU_TEST_TYPES - 1; i++)
            if (check_test_available(i)) {
                for (int size = 5; size + 8 <= max_size; size = size * 3 / 2)
                    do_test(i, size, size);
                /* Draw a green dot to indicate the test is finished. */
                if (bpp == 32)
                    values.foreground = 0x00007F00;
                else
                    values.foreground = 0x000003E0;
                XChangeGC(display, root_gc, GCForeground, &values);
                XFillRectangle(display, root_window,
                   root_gc, i * 16 + 8, WINDOW_HEIGHT + 8, 8, 8);
            }
    }
    else
        for (int size = 5; size + 8 <= max_size; size = size * 3 / 2)
            do_test(test, size, size);

  XCloseDisplay(display);

  return 0;
}
