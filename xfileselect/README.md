# xfileselect

A tiny, single-file X11 file/directory chooser for C programs.

- Standalone or embeddable via `#include "xfileselect.c"`
- File or directory selection mode
- Comma-separated extension filters (e.g. `.mid,.midi,.cmf`)
- Configurable appearance via `#define`s (sizes, font, colors, header/footer text)
- Minimal dependencies: X11 only

## Example usage

```c
// example_main.c

// Optional: override visual defaults before including the implementation
#define FC_DEFAULT_WIDTH   400
#define FC_DEFAULT_HEIGHT  500
#define FC_FONT_NAME       "fixed"

// Include the implementation (it pulls in xfileselect.h itself)
#include "xfileselect.c"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char *path = x11_filechooser(
        "/home/user",                 // start_path (NULL => current dir)
        "Open File",                  // window title
        "Select a MIDI file",         // header text (shown above file list)
        ".mid,.midi,.cmf",            // extension filter (NULL or "" => all)
        FC_MODE_FILE,                 // FC_MODE_FILE or FC_MODE_DIR
        "[q/Esc]: Cancel [Enter]: Open"  // status/help line
    );

    if (path) {
        printf("Chosen: %s\n", path);
        free(path);
    } else {
        printf("No file chosen.\n");
    }

    return 0;
}
```

Build:

```bash
gcc example_main.c -o example_main -lX11
```

## Features

- **Modes**
  - `FC_MODE_FILE`: navigate directories, select a file.
  - `FC_MODE_DIR`: navigate directories, select a directory.
- **Filtering**
  - Comma-separated extensions, e.g. `".mid,.midi,.cmf"`.
  - Directories are always shown.
- **Visual customization**
  - Define before `#include "xfileselect.c"`:
    - `FC_DEFAULT_WIDTH`, `FC_DEFAULT_HEIGHT`
    - `FC_FONT_NAME`
    - Color strings: `FC_HEADER_BG_COLOR`, `FC_LIST_BG_COLOR`, etc. (hex like `"0x222222"` or `"#222222"`)
    - Text: `FC_DEFAULT_HELP_TEXT`
  - Header text and window title are set per call.

## Files

- `xfileselect.h` – public API (`FCMode`, `x11_filechooser()`).
- `xfileselect.c` – implementation (include once per project).
- `example_main.c` – minimal example program.

## Building your own program

Either:

- Include the implementation directly in one `.c` file:

  ```c
  #define FC_DEFAULT_WIDTH 400
  #include "xfileselect.c"
  ```

- Or compile separately:

  ```bash
  gcc -c xfileselect.c -o xfileselect.o
  gcc -o myapp myapp.c xfileselect.o -lX11
  ```

  In this case, put any `#define` visual overrides in a config header included by `xfileselect.c`.

## License

See the license text at the top of `xfileselect.c` and `xfileselect.h`.

