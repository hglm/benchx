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

static Pixmap pixmap = None;
static Pixmap shmpixmap = None;

static uint8_t *data = NULL;
static uint8_t *shmdata_ximage = NULL;
static uint8_t *shmdata_pixmap = NULL;

static Picture pixmap_pict;
static Picture shmpixmap_pict;
static Picture window_pict;

/* test id */
static int test = 0;

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

#define NU_TEST_TYPES 9
#define TEST_COPYAREA 0
#define TEST_ALIGNEDCOPYAREA 1
#define TEST_FILLRECT 2
#define TEST_PUTIMAGE 3
#define TEST_SHMPUTIMAGE 4
#define TEST_ALIGNEDSHMPUTIMAGE 5
#define TEST_SHMPIXMAPCOPY 6
#define TEST_ALIGNEDSHMPIXMAPCOPY 7
#define TEST_ALL 8

static const char *test_name[] = {
    "CopyArea", "AlignedCopyArea", "FillRect", "PutImage", "ShmPutImage", "AlignedShmPutImage",
    "ShmPixmapCopy", "AlignedShmPixmapCopy", "All"
};

void do_test(int test, int w, int h) {
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
    case TEST_PUTIMAGE :
        nu_iterations = 10;
        if (area < 10000)
            nu_iterations = 50;
        if (area < 1000)
            nu_iterations = 200;
        break;
    case TEST_SHMPUTIMAGE :
    case TEST_ALIGNEDSHMPUTIMAGE :
    case TEST_SHMPIXMAPCOPY :
    case TEST_ALIGNEDSHMPIXMAPCOPY :
        nu_iterations = 10;
        if (area < 10000)
            nu_iterations = 200;
        if (area < 1000)
            nu_iterations = 1000;
        break;
    case TEST_COPYAREA :
    case TEST_ALIGNEDCOPYAREA :
        nu_iterations = 10;
        if (area < 10000)
            nu_iterations = 200;
        if (area < 1000)
            nu_iterations = 1000;
        break;
    case TEST_FILLRECT :
        nu_iterations = 100;
        if (area < 10000)
            nu_iterations = 500;
        if (area < 1000)
            nu_iterations = 2000;
    }

    /* Set the window/gc to draw to. */
    Window dest_window = root_window;
    GC dest_gc = root_gc;
    int y_offset = 0;

    for (int i = 0; i < WINDOW_WIDTH * WINDOW_HEIGHT * (bpp / 8); i++)
            *((unsigned char *)data + i) = rand() & 0xFF;
    XPutImage(display,
                dest_window, 
		dest_gc,
		ximage, 
		0, 0, 
		0, y_offset, 
		WINDOW_WIDTH, WINDOW_HEIGHT);
    if (test == TEST_SHMPUTIMAGE || test == TEST_ALIGNEDSHMPUTIMAGE)
        for (int i = 0; i < WINDOW_WIDTH * WINDOW_HEIGHT * (bpp / 8); i++)
            *((unsigned char *)shmdata_ximage + i) = rand() & 0xFF;
    if (test == TEST_SHMPIXMAPCOPY || test == TEST_ALIGNEDSHMPIXMAPCOPY)
        for (int i = 0; i < WINDOW_WIDTH * WINDOW_HEIGHT * (bpp / 8); i++)
            *((unsigned char *)shmdata_pixmap + i) = rand() & 0xFF;
    XGCValues values;
    /* Draw a dot to indicate which test is being run. */
    values.foreground = 0xFFFFFFFF;
    XChangeGC(display, dest_gc, GCForeground, &values);
    XFillRectangle(display, dest_window,
                   dest_gc, test * 16 + 8, WINDOW_HEIGHT + 8 + y_offset, 8, 8);
    /* For FillRect, set a random fill color. */
    if (test == TEST_FILLRECT) {
        values.foreground = rand();
        XChangeGC(display, dest_gc, GCForeground, &values);
    }

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
            switch (test) {
            case TEST_COPYAREA :
	        XCopyArea(display, 
	            dest_window, dest_window, 
		    dest_gc, 
		    i & 7, 1 + y_offset,
		    w, h,
		    (i / 8) & 7, y_offset);
                break;
            case TEST_ALIGNEDCOPYAREA :
                XCopyArea(display,
                    dest_window, dest_window,
                    dest_gc,
                    i & 7, 1 + y_offset,
                    w, h,
                    i & 7, y_offset);
                break;
            case TEST_FILLRECT :
                XFillRectangle(display, dest_window,
                    dest_gc, i & 7, ((i / 8) & 7) + y_offset, w, h);
                break;
            case TEST_PUTIMAGE :
                XPutImage(display,
		    dest_window, 
		    dest_gc,
		    ximage, 
		    0, 0,
		    (i & 7), ((i / 8) & 7) + y_offset, 
		    w, h);
                break;
            case TEST_SHMPUTIMAGE :
                XShmPutImage(display,
		    dest_window, 
		    dest_gc,
		    shmximage_ximage,
		    0, 0, 
		    (i & 7), ((i / 8) & 7) + y_offset, 
		    w, h, False);
                break;
            case TEST_ALIGNEDSHMPUTIMAGE :
                XShmPutImage(display,
                    dest_window,
                    dest_gc,
                    shmximage_ximage,
                    0, 0,
                    (i & 1) * 8, ((i / 8) & 7) + y_offset,
                    w, h, False);
                break;
            case TEST_SHMPIXMAPCOPY :
                XCopyArea(display,
                    shmpixmap, dest_window,
                    dest_gc,
                    0, 0,
                    w, h,
                    i & 7, ((i / 8) & 7) + y_offset);
                break;
            case TEST_ALIGNEDSHMPIXMAPCOPY :
                XCopyArea(display,
                    shmpixmap, dest_window,
                    dest_gc,
                    0, 0,
                    w, h,
                    (i & 1) * 8, ((i / 8) & 7) + y_offset);
                break;
            }
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
    printf("%s (%d x %d): operations per second: %.2f (%.2f MB/s)\n", test_name[test],
        w, h, operation_count / dt, (operation_count / dt) * w * h * (bpp / 8) / (1024 * 1024));
    /* Draw a green dot to indicate the test is finished. */
    if (bpp == 32)
        values.foreground = 0x0000FF00;
    else
        values.foreground = 0x000007E0;
    XChangeGC(display, dest_gc, GCForeground, &values);
    XFillRectangle(display, dest_window,
                   dest_gc, test * 16 + 8, WINDOW_HEIGHT + 8 + y_offset, 8, 8);

}

int check_test_available(int test) {
    if ((test == TEST_SHMPUTIMAGE || test == TEST_ALIGNEDSHMPUTIMAGE) && !feature_shm) {
        printf("Cannot run test %s because SHM is not supported.\n", test_name[test]);
        return 0;
    }
    if ((test == TEST_SHMPIXMAPCOPY || test == TEST_ALIGNEDSHMPIXMAPCOPY) && !feature_shm_pixmap) {
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
        printf("    All\n");
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
  pixmap = XCreatePixmap(display,
			 root_window, 
			 width,
			 height,
			 bpp);

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
				       pixmap,
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
            if (check_test_available(i))
                for (int size = 5; size + 8 <= max_size; size = size * 3 / 2)
                    do_test(i, size, size);
    }
    else
        for (int size = 5; size + 8 <= max_size; size = size * 3 / 2)
            do_test(test, size, size);

  XCloseDisplay(display);

  return 0;
}
