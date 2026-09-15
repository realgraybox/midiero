/*
 * Copyright (C) 2026 M. Glargaard, aka graybox
 *
 * This software is provided "as-is", without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would
 *    be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not
 *    be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 */
// xfileselect.c
#include "xfileselect.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char *fc_strdup(const char *s)  {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);

    if (d)
        memcpy(d, s, n);

    return d;
}

// ----------------------------
// Visual / behavior configuration
// Override these in your main program before including this file.
// ----------------------------

#ifndef FC_DEFAULT_WIDTH
#define FC_DEFAULT_WIDTH        500
#endif

#ifndef FC_DEFAULT_HEIGHT
#define FC_DEFAULT_HEIGHT       300
#endif

#ifndef FC_FONT_NAME
#define FC_FONT_NAME            "fixed"
#endif

// Colors (as hex strings for parsing)
#ifndef FC_HEADER_BG_COLOR
#define FC_HEADER_BG_COLOR      "#222222"
#endif

#ifndef FC_HEADER_FG_COLOR
#define FC_HEADER_FG_COLOR      "#ffffff"
#endif

#ifndef FC_LIST_BG_COLOR
#define FC_LIST_BG_COLOR        "#222222"
#endif

#ifndef FC_LIST_FG_COLOR
#define FC_LIST_FG_COLOR        "#666666"
#endif

#ifndef FC_LIST_SEL_BG_COLOR
#define FC_LIST_SEL_BG_COLOR    "#666666"
#endif

#ifndef FC_LIST_SEL_FG_COLOR
#define FC_LIST_SEL_FG_COLOR    "#ffffff"
#endif

#ifndef FC_STATUS_BG_COLOR
#define FC_STATUS_BG_COLOR      "#222222"
#endif

#ifndef FC_STATUS_FG_COLOR
#define FC_STATUS_FG_COLOR      "#ffffff"
#endif

#ifndef FC_SEPARATOR_COLOR
#define FC_SEPARATOR_COLOR      "#666666"
#endif

#ifndef FC_HEADER_PADDING_Y
#define FC_HEADER_PADDING_Y     10
#endif

#ifndef FC_STATUS_PADDING_Y
#define FC_STATUS_PADDING_Y     10
#endif

#ifndef FC_LINE_EXTRA
#define FC_LINE_EXTRA           2	//not used
#endif

#ifndef FC_FILE_TEXT_X
#define FC_FILE_TEXT_X          10	//distance to left side of text in filearea
#endif

#ifndef FC_INFO_TEXT_X
#define FC_INFO_TEXT_X          300	//distance to left side of file info in filearea
#endif

#ifndef FC_DEFAULT_HELP_TEXT
#define FC_DEFAULT_HELP_TEXT \
    "[q/Esc]: Cancel [Arrows]: Scroll [Enter]: Select [Click]: Move/Select"
#endif

#define MAX_FILES 1024

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    XFontStruct *font;

    int width;
    int height;
    int line_height;

    char cwd[4096];
    char *files[MAX_FILES];
    int file_count;
    int selected;
    int scroll_offset;
    int visible_lines;

    // colors
    unsigned long header_bg;
    unsigned long header_fg;
    unsigned long list_bg;
    unsigned long list_fg;
    unsigned long list_sel_bg;
    unsigned long list_sel_fg;
    unsigned long status_bg;
    unsigned long status_fg;

    // configuration
    char title[256];
    char header_text[256];
    char filter_list[256];      // e.g. ".mid,.midi,.cmf" or "" for none
    FCMode mode;
    char help_text[256];

    // result
    char result_path[4096];
    int done;
    int canceled;
} FCContext;

static unsigned long fc_alloc_color(Display *dpy, int screen,
                                    const char *spec,
                                    unsigned long fallback);
static int compare_files(const void *a, const void *b);
static int matches_filter(FCContext *fc, const char *name, struct stat *st);
static int fc_init(FCContext *fc, const char *start_path,
                   const char *title, const char *header_text,
                   const char *filter_ext, FCMode mode, const char *help_text);
static void fc_load_files(FCContext *fc, const char *path);
static void fc_draw(FCContext *fc);
static void fc_handle_key(FCContext *fc, XKeyEvent *ev);
static void fc_handle_click(FCContext *fc, XButtonEvent *ev);
static void fc_run(FCContext *fc);
static void fc_cleanup(FCContext *fc);
static void fc_set_window_icon(Display *dpy, Window win);



static unsigned long fc_alloc_color(Display *dpy, int screen,
                                    const char *spec,
                                    unsigned long fallback)
{
    XColor color;
    Colormap cmap = DefaultColormap(dpy, screen);

    if (XParseColor(dpy, cmap, spec, &color) &&
        XAllocColor(dpy, cmap, &color)) {
        return color.pixel;
    }

    fprintf(stderr, "xfileselect: cannot allocate color %s\n", spec);
    return fallback;
}

static int compare_files(const void *a, const void *b) {
    const char *fa = *(const char **)a;
    const char *fb = *(const char **)b;
    return strcoll(fa, fb);
}

static void fc_set_window_icon(Display *dpy, Window win) {
	unsigned long icon_data[] = {
    8, 8, // width, height
		0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
		0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000,
		0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000,
		0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000,
		0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000,
		0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000,
		0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000
	};
   
    int data_length = 2 + (8 * 8); // 2 (measure) + (width * height)

    Atom wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    Atom cardinal = XInternAtom(dpy, "CARDINAL", False);
    XChangeProperty(dpy, win, wm_icon, cardinal, 32, PropModeReplace, (unsigned char*)icon_data, data_length);
    XFlush(dpy); 
}

static int matches_filter(FCContext *fc, const char *name, struct stat *st) {
    if (fc->filter_list[0] == '\0') return 1;
    if (S_ISDIR(st->st_mode)) return 1;

    const char *ext = strrchr(name, '.');
    if (!ext) return 0;

    char buf[256];
    strncpy(buf, fc->filter_list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok == '\0') {
            tok = strtok_r(NULL, ",", &saveptr);
            continue;
        }
        if (strcasecmp(ext, tok) == 0) return 1;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}

static int fc_init(FCContext *fc, const char *start_path,
            const char *title, const char *header_text,
            const char *filter_ext, FCMode mode, const char *help_text)
{
    memset(fc, 0, sizeof(*fc));

    fc->dpy = XOpenDisplay(NULL);
    if (!fc->dpy) return -1;

    int screen = DefaultScreen(fc->dpy);

    fc->width  = FC_DEFAULT_WIDTH;
    fc->height = FC_DEFAULT_HEIGHT;

    fc->win = XCreateSimpleWindow(fc->dpy,
                                  RootWindow(fc->dpy, screen),
                                  100, 100,
                                  fc->width, fc->height,
                                  1,
                                  BlackPixel(fc->dpy, screen),
                                  WhitePixel(fc->dpy, screen));

    if (title) {
        strncpy(fc->title, title, sizeof(fc->title) - 1);
    } else {
        strncpy(fc->title, "Filechooser", sizeof(fc->title) - 1);
    }

    XStoreName(fc->dpy, fc->win, fc->title);
    XSetIconName(fc->dpy, fc->win, fc->title);

    XSelectInput(fc->dpy, fc->win,
                 ExposureMask | KeyPressMask |
                 ButtonPressMask | StructureNotifyMask);
    
    fc_set_window_icon(fc->dpy, fc->win);

    fc->gc = XCreateGC(fc->dpy, fc->win, 0, NULL);

	fc->header_bg     = fc_alloc_color(fc->dpy, screen,
                             FC_HEADER_BG_COLOR,
                             WhitePixel(fc->dpy, screen));
	
	fc->header_fg     = fc_alloc_color(fc->dpy, screen,
                             FC_HEADER_FG_COLOR,
                             BlackPixel(fc->dpy, screen));
	
	fc->list_bg     = fc_alloc_color(fc->dpy, screen,
                             FC_LIST_BG_COLOR,
                             WhitePixel(fc->dpy, screen));
	
	fc->list_fg     = fc_alloc_color(fc->dpy, screen,
                             FC_LIST_FG_COLOR,
                             BlackPixel(fc->dpy, screen));

	fc->list_sel_bg     = fc_alloc_color(fc->dpy, screen,
                             FC_LIST_SEL_BG_COLOR,
                             BlackPixel(fc->dpy, screen));
	
	fc->list_sel_fg     = fc_alloc_color(fc->dpy, screen,
                             FC_LIST_SEL_FG_COLOR,
                             BlackPixel(fc->dpy, screen));
	
	fc->status_bg     = fc_alloc_color(fc->dpy, screen,
                             FC_STATUS_BG_COLOR,
                             BlackPixel(fc->dpy, screen));
	

	fc->status_fg     = fc_alloc_color(fc->dpy, screen,
                             FC_STATUS_FG_COLOR,
                             BlackPixel(fc->dpy, screen));
                             
    XSetWindowBackground(fc->dpy, fc->win, fc->list_bg);
                             
    XMapWindow(fc->dpy, fc->win);

    fc->font = XLoadQueryFont(fc->dpy, FC_FONT_NAME);
    if (fc->font) {
        XSetFont(fc->dpy, fc->gc, fc->font->fid);
        fc->line_height = fc->font->ascent + fc->font->descent + 2;
    } else {
        fc->line_height = 14;
    }

    if (filter_ext) {
        strncpy(fc->filter_list, filter_ext, sizeof(fc->filter_list) - 1);
        fc->filter_list[sizeof(fc->filter_list) - 1] = '\0';
    } else {
        fc->filter_list[0] = '\0';
    }

    fc->mode = mode;

    if (header_text) {
        strncpy(fc->header_text, header_text, sizeof(fc->header_text) - 1);
        fc->header_text[sizeof(fc->header_text) - 1] = '\0';
    } else {
        fc->header_text[0] = '\0';
    }

    if (help_text) {
        strncpy(fc->help_text, help_text, sizeof(fc->help_text) - 1);
        fc->help_text[sizeof(fc->help_text) - 1] = '\0';
    } else {
        strncpy(fc->help_text, FC_DEFAULT_HELP_TEXT, sizeof(fc->help_text) - 1);
        fc->help_text[sizeof(fc->help_text) - 1] = '\0';
    }

    if (start_path) {
        if (chdir(start_path) != 0) {
            start_path = ".";
        }
    } else {
        start_path = ".";
    }

    if (!getcwd(fc->cwd, sizeof(fc->cwd))) {
        XCloseDisplay(fc->dpy);
        return -1;
    }

    fc_load_files(fc, fc->cwd);
    fc->done = 0;
    fc->canceled = 0;
    fc->result_path[0] = '\0';

    return 0;
}

static void fc_load_files(FCContext *fc, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    for (int i = 0; i < fc->file_count; i++) free(fc->files[i]);
    fc->file_count = 0;

    fc->files[fc->file_count++] = fc_strdup("..");

    struct dirent *entry;
    while ((entry = readdir(dir)) && fc->file_count < MAX_FILES) {
        if (strcmp(entry->d_name, ".") == 0) continue;
        if (strcmp(entry->d_name, "..") == 0) continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full, &st) == 0) {
            if (!matches_filter(fc, entry->d_name, &st)) {
                continue;
            }
        }

        fc->files[fc->file_count++] = fc_strdup(entry->d_name);
    }

    closedir(dir);
    qsort(fc->files, fc->file_count, sizeof(char *), compare_files);

    for (int i = 0; i < fc->file_count; i++) {
        if (strcmp(fc->files[i], "..") == 0) {
            char *tmp = fc->files[i];
            for (int j = i; j > 0; j--) {
                fc->files[j] = fc->files[j - 1];
            }
            fc->files[0] = tmp;
            break;
        }
    }

    fc->selected = 0;
    fc->scroll_offset = 0;
}

static void fc_draw(FCContext *fc) {
    XClearWindow(fc->dpy, fc->win);

    int line_height = fc->line_height;
    int header_height = (fc->header_text[0] != '\0')
                      ? (line_height + FC_HEADER_PADDING_Y)
                      : 0;
    int status_line_height = line_height + FC_STATUS_PADDING_Y;

    int usable_height = fc->height - status_line_height - header_height;

    fc->visible_lines = usable_height / line_height;

    if (fc->file_count <= 0) {
        fc->scroll_offset = 0;
        fc->selected = 0;
    } else {
        if (fc->scroll_offset > fc->file_count - fc->visible_lines)
            fc->scroll_offset = fc->file_count - fc->visible_lines;
        if (fc->scroll_offset < 0)
            fc->scroll_offset = 0;
        if (fc->selected >= fc->file_count)
            fc->selected = fc->file_count - 1;
    }
    
    int screen = DefaultScreen(fc->dpy);
                                          
    unsigned long sep_color = fc_alloc_color(fc->dpy, screen ,
                             FC_SEPARATOR_COLOR,
                             WhitePixel(fc->dpy, screen));                                       

    // Draw header (if any)
    if (header_height > 0) {
        XSetForeground(fc->dpy, fc->gc, fc->header_bg);
        XFillRectangle(fc->dpy, fc->win, fc->gc,
                       0, 0, fc->width, header_height);
        XSetForeground(fc->dpy, fc->gc, fc->header_fg);
        XDrawString(fc->dpy, fc->win, fc->gc,
                    10, line_height + 2,
                    fc->header_text, strlen(fc->header_text));

        // Separator between header and file area
        XSetForeground(fc->dpy, fc->gc, sep_color);
        XFillRectangle(fc->dpy, fc->win, fc->gc,
                       0, header_height - 1, fc->width, 1);           
    }

    int list_y_start = header_height;

    // Draw file list
    for (int i = 0; i < fc->visible_lines; i++) {
        int file_index = i + fc->scroll_offset;
        if (file_index >= fc->file_count) break;

        int y = list_y_start + (i + 1) * line_height;

        if (file_index == fc->selected) {
            XSetForeground(fc->dpy, fc->gc, fc->list_sel_bg);
            XFillRectangle(fc->dpy, fc->win, fc->gc,
                           0, y - line_height + 2, fc->width, line_height);
            XSetForeground(fc->dpy, fc->gc, fc->list_sel_fg);
        } else {
            XSetForeground(fc->dpy, fc->gc, fc->list_bg);
            /*XFillRectangle(fc->dpy, fc->win, fc->gc,
                   0, y - line_height + 2, fc->width, line_height);*/
            XSetForeground(fc->dpy, fc->gc, fc->list_fg);
        }

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", fc->cwd, fc->files[file_index]);

        struct stat st;
        char info[64] = "";
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                snprintf(info, sizeof(info), "<DIR>");
            } else {
                snprintf(info, sizeof(info), "%ld bytes", (long)st.st_size);
            }
        } else {
            snprintf(info, sizeof(info), "?");
        }

        XDrawString(fc->dpy, fc->win, fc->gc,
                    FC_FILE_TEXT_X, y,
                    fc->files[file_index],
                    strlen(fc->files[file_index]));
        XDrawString(fc->dpy, fc->win, fc->gc,
                    FC_INFO_TEXT_X, y,
                    info, strlen(info));
    }
    
    // Separator between file area and status
    int status_y = fc->height - status_line_height;
    XSetForeground(fc->dpy, fc->gc, sep_color);
    XFillRectangle(fc->dpy, fc->win, fc->gc,
                   0, status_y - 1, fc->width, 1);

    // Draw status line
    XSetForeground(fc->dpy, fc->gc, fc->status_bg);
    XFillRectangle(fc->dpy, fc->win, fc->gc,
                   0, status_y, fc->width, status_line_height);
    XSetForeground(fc->dpy, fc->gc, fc->status_fg);

    XDrawString(fc->dpy, fc->win, fc->gc,
                10, fc->height - 6,
                fc->help_text, strlen(fc->help_text));
}

static void fc_handle_key(FCContext *fc, XKeyEvent *ev) {
    KeySym key = XLookupKeysym(ev, 0);

    if (key == XK_q || key == XK_Escape) {
        fc->done = 1;
        fc->canceled = 1;
        return;
    }

    if (key == XK_Up) {
        if (fc->selected > 0) {
            fc->selected--;
            if (fc->selected < fc->scroll_offset) fc->scroll_offset--;
        }
    } else if (key == XK_Down) {
        if (fc->selected < fc->file_count - 1) {
            fc->selected++;
            if (fc->selected >= fc->scroll_offset + fc->visible_lines)
                fc->scroll_offset++;
        }
    } else if (key == XK_Return) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", fc->cwd, fc->files[fc->selected]);

        struct stat st;
        int is_dir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));

        if (fc->mode == FC_MODE_DIR) {
            if (is_dir) {
                if (strcmp(fc->files[fc->selected], "..") == 0) {
                    if (chdir(path) == 0) {
                        if (getcwd(fc->cwd, sizeof(fc->cwd))) {
                            fc_load_files(fc, fc->cwd);
                            fc->selected = 0;
                            fc->scroll_offset = 0;
                        }
                    }
                } else {
                    strncpy(fc->result_path, path, sizeof(fc->result_path) - 1);
                    fc->done = 1;
                    fc->canceled = 0;
                }
            }
        } else {
            if (is_dir) {
                if (chdir(path) == 0) {
                    if (getcwd(fc->cwd, sizeof(fc->cwd))) {
                        fc_load_files(fc, fc->cwd);
                        fc->selected = 0;
                        fc->scroll_offset = 0;
                    }
                }
            } else {
                strncpy(fc->result_path, path, sizeof(fc->result_path) - 1);
                fc->done = 1;
                fc->canceled = 0;
            }
        }
    }
}

static void fc_handle_click(FCContext *fc, XButtonEvent *ev) {
    int line_height = fc->line_height;
    int header_height = (fc->header_text[0] != '\0')
                      ? (line_height + FC_HEADER_PADDING_Y)
                      : 0;
    int status_line_height = line_height + FC_STATUS_PADDING_Y;

    int list_y_start = header_height;
    int usable_height = fc->height - status_line_height - header_height;
    int rows = usable_height / line_height;

    int y_rel = ev->y - list_y_start;
    if (y_rel < 0) return; // clicked in header

    int idx = y_rel / line_height;
    if (idx >= rows) return; // clicked in status

    int file_idx = idx + fc->scroll_offset;
    if (file_idx < 0 || file_idx >= fc->file_count) return;

    fc->selected = file_idx;

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", fc->cwd, fc->files[fc->selected]);

    struct stat st;
    int is_dir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    if (fc->mode == FC_MODE_DIR) {
        if (is_dir) {
            if (strcmp(fc->files[fc->selected], "..") == 0) {
                if (chdir(path) == 0) {
                    if (getcwd(fc->cwd, sizeof(fc->cwd))) {
                        fc_load_files(fc, fc->cwd);
                        fc->scroll_offset = 0;
                        fc->selected = 0;
                    }
                }
            } else {
                strncpy(fc->result_path, path, sizeof(fc->result_path) - 1);
                fc->done = 1;
                fc->canceled = 0;
            }
        }
    } else {
        if (is_dir) {
            if (chdir(path) == 0) {
                if (getcwd(fc->cwd, sizeof(fc->cwd))) {
                    fc_load_files(fc, fc->cwd);
                    fc->scroll_offset = 0;
                    fc->selected = 0;
                }
            }
        } else {
            strncpy(fc->result_path, path, sizeof(fc->result_path) - 1);
            fc->done = 1;
            fc->canceled = 0;
        }
    }
}

static void fc_run(FCContext *fc) {
    fc_draw(fc);

    XEvent ev;
    while (!fc->done) {
        XNextEvent(fc->dpy, &ev);

        if (ev.type == Expose) {
            fc_draw(fc);
        } else if (ev.type == KeyPress) {
            fc_handle_key(fc, &ev.xkey);
            fc_draw(fc);
        } else if (ev.type == ButtonPress) {
            if (ev.xbutton.button == Button1) {
                fc_handle_click(fc, &ev.xbutton);
                fc_draw(fc);
            } else if (ev.xbutton.button == Button4) {
                if (fc->scroll_offset > 0) fc->scroll_offset--;
                fc_draw(fc);
            } else if (ev.xbutton.button == Button5) {
                int line_height = fc->line_height;
                int header_height = (fc->header_text[0] != '\0')
                                  ? (line_height + FC_HEADER_PADDING_Y)
                                  : 0;
                int status_line_height = line_height + FC_STATUS_PADDING_Y;
                int usable_height = fc->height - status_line_height - header_height;
                int rows = usable_height / line_height;

                if (fc->scroll_offset < fc->file_count - rows)
                    fc->scroll_offset++;
                fc_draw(fc);
            }
        } else if (ev.type == ConfigureNotify) {
            fc->width  = ev.xconfigure.width;
            fc->height = ev.xconfigure.height;
            fc_draw(fc);

            int line_height = fc->line_height;
            int header_height = (fc->header_text[0] != '\0')
                              ? (line_height + FC_HEADER_PADDING_Y)
                              : 0;
            int status_line_height = line_height + FC_STATUS_PADDING_Y;
            int usable_height = fc->height - status_line_height - header_height;
            int rows = usable_height / line_height;

            if (fc->scroll_offset > fc->file_count - rows)
                fc->scroll_offset = fc->file_count - rows;
            if (fc->scroll_offset < 0)
                fc->scroll_offset = 0;
        }
    }
}

static void fc_cleanup(FCContext *fc) {
    for (int i = 0; i < fc->file_count; i++) {
        free(fc->files[i]);
    }
    fc->file_count = 0;

    if (fc->font) XFreeFont(fc->dpy, fc->font);
    XFreeGC(fc->dpy, fc->gc);
    XDestroyWindow(fc->dpy, fc->win);
    XCloseDisplay(fc->dpy);
}

// Public API
char *x11_filechooser(const char *start_path,
                      const char *title,
                      const char *header_text,
                      const char *filter_ext,
                      FCMode mode,
                      const char *help_text)
{
    FCContext fc;
    if (fc_init(&fc, start_path, title, header_text,
                filter_ext, mode, help_text) != 0) {
        return NULL;
    }

    fc_run(&fc);

    char *result = NULL;
    if (!fc.canceled && fc.result_path[0] != '\0') {
        result = fc_strdup(fc.result_path);
    }

    fc_cleanup(&fc);
    return result;
}

#ifdef STANDALONE
int main(void) {
    char *path = x11_filechooser(
        NULL,
        "Filemanager",
        "Select a file",      // header_text
        NULL,
        FC_MODE_FILE,
        NULL
    );
    if (path) {
        printf("Selected file: %s\n", path);
        free(path);
    } else {
        printf("No file selected.\n");
    }
    return 0;
}
#endif
