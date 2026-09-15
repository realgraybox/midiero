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
// xfileselect.h
#ifndef XFILESELECT_H
#define XFILESELECT_H

/*
 * xfileselect – tiny X11 file/directory chooser
 *
 * Usage:
 *   In exactly one .c file of your project, before including this header,
 *   optionally override visual defaults:
 *
 *     #define FC_DEFAULT_WIDTH   400
 *     #define FC_DEFAULT_HEIGHT  500
 *     #define FC_FONT_NAME       "fixed"
 *
 *     // Colors must be in X11 "#RRGGBB" format (e.g. "#222222").
 *     #define FC_HEADER_BG_COLOR      "#222222"
 *     #define FC_HEADER_FG_COLOR      "#444444"
 *     #define FC_LIST_BG_COLOR        "#222222"
 *     #define FC_LIST_FG_COLOR        "#444444"
 *     #define FC_LIST_SEL_BG_COLOR    "#444444"
 *     #define FC_LIST_SEL_FG_COLOR    "#ffffff"
 *     #define FC_STATUS_BG_COLOR      "#222222"
 *     #define FC_STATUS_FG_COLOR      "#444444"
 *     #define FC_SEPARATOR_COLOR      "#444444"
 *
 *   Then include the implementation once:
 *
 *     #include "filemanager_xlib.c"
 *
 *   Other files should just #include "filemanager_xlib.h".
 *
 * Colors:
 *   All FC_*_COLOR defines are parsed with XParseColor/XAllocColor and
 *   must be in X11 string format: "#RRGGBB" (e.g. "#222222").
 *   Using "0xRRGGBB" or other formats is not supported.
 */

typedef enum {
    FC_MODE_FILE,
    FC_MODE_DIR
} FCMode;

/*
 * Open a small X11 file/directory chooser dialog.
 *
 * start_path   : Initial directory (NULL => current working directory).
 * title        : Window title for WM (NULL => "Filechooser").
 * header_text  : Text shown in header above file list (NULL => no header).
 * filter_ext   : Comma-separated extension list, e.g. ".mid,.midi,.cmf".
 *                NULL or "" => no filtering (all files shown).
 * mode         : FC_MODE_FILE to select a file, FC_MODE_DIR to select a directory.
 * help_text    : Status-line help text (NULL => built-in default).
 *
 * Returns:
 *   malloc'd string with the selected path on success,
 *   NULL if canceled, error, or no selection.
 *
 * Caller must free() the returned string.
 */
char *x11_filechooser(const char *start_path,
                      const char *title,
                      const char *header_text,
                      const char *filter_ext,
                      FCMode mode,
                      const char *help_text);


#endif // XFILESELECT_H
