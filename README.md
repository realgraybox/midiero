

https://github.com/user-attachments/assets/91d5e461-0ca1-4e39-be96-81390f5f4d5e

# midiero

A lightweight, interactive MIDI visualization and music learning tool for Linux. Built from scratch in pure C using **Xlib** for graphics and **TinyALSA** for real-time sound generation via an embedded SoundFont. 

Conceptually inspired by games like *Guitar Hero*, **midiero** focuses on helping users see, hear, and learn music through proper musical notation rather than abstract blocks.

With a fully static build utilizing `uclibc`, the entire application—including its internal SoundFont synthesizer—compiles into a standalone binary of **around 348 KB** (single-threaded; the threaded build with DEBUG_TIMING is about 405 KB), making it exceptionally resource-efficient and portable.

Not related to SpyderTL/MidiHero, a Windows practice utility for isolating instrument tracks — midiero is a Linux-native, dependency-free MIDI visualizer with live notation.

## Features

- **Live Musical Notation (Node Mode):** Renders notes onto an intelligent double-staff (Treble and Bass clefs) with dynamic support for accidentals (`#`) and ledger lines.
- **Classic Arcade View (Hero Mode):** Toggle instantly into a linear, geometric block-stream for quick visual decoding.
- **Dynamic Window Rescaling:** Seamlessly handles window resizing, maximizing, and full-screen modes. On a large screen everything grows together - text, sidebar, staff, notes and lines - so a 4K monitor shows the same layout as an 800x600 window, only bigger. The size follows the window automatically, or is chosen with `+` / `-`.
- **Per-Channel Track Control:** Mute or solo any of the 16 MIDI channels in real-time to isolate specific instruments (e.g., piano melodies or basslines).
- **Dynamic Instrument Legend:** A built-in sidebar displays active SoundFont/AdLib preset names for each track in real-time, matching the color codes of the cascading notes.
- **Embedded SoundBank & Hot-Swapping:** Operates out-of-the-box with a built-in lightweight patch bank (Nokia fallback), while supporting on-the-fly loading of external `.mid` and `.sf2` files via a native Xlib file chooser.
- **Playback Controls:** Pause, play, and adjust playback speeds in real-time without altering the audio pitch.

## Controls

| Key | Action |
| :--- | :--- |
| `Space` | Play / Pause playback |
| `T` | Toggle between **Node Mode** and **Hero Mode** |
| `V` | Toggle note duration bars (visual tails) On / Off |
| `Arrow Up` / `Down` | Increase / Decrease playback speed (0.1x increments) |
| `1` - `9` | Toggle MIDI Channels 1 – 9 On / Off |
| `0` | Toggle MIDI Channel 10 (Drums) On / Off |
| `Shift` + `1` - `6` | Toggle MIDI Channels 11 – 16 On / Off |
| `L` | Open Xlib File Chooser to load a new **MIDI file** |
| `F` | Open Xlib File Chooser to hot-swap the **SoundFont (.sf2)** |
| `+` / `-` | Bigger / smaller text and notes |
| `A` | Size follows the window again (default) |
| `Q` / `Esc` | Quit the application |

## Architecture & Dependencies

midiero is crafted to be completely independent of bloated desktop frameworks (like GTK or Qt) and external server-side synths. 

- **Graphics & Input:** Pure X11 / Xlib
- **Text:** [xbmfont](https://github.com/realgraybox/xbmfont) (bundled in `xbmfont/`, zlib licensed; the fonts are the public-domain X11 misc-fixed fonts): the font is built into the program, so midiero looks the same on every X server, and song and file names in UTF-8 or old 8-bit encodings show right.
- **MIDI Parsing and Sound Synthesis:** [mmidi](https://github.com/realgraybox/mmidi) (its `v2/` engine, bundled in `mmidi/`, zlib licensed): Standard MIDI files of format 0, 1 and 2 (format 2 sequences back to back) and RIFF RMID, a SoundFont 2 synthesizer with the bank's own per-instrument reverb and chorus, built `.sf2`-only (`-DMM_NO_MODELS`). midiero reads the song's events from mmidi once and runs its own sequencer, so pause, speed and the note display work as before.
- **Audio Output:** OSS and/or TinyALSA

## Installation & Compilation

Ensure you have the X11 development headers installed on your Linux system (e.g., `libx11-dev` on Debian/Ubuntu).

View the build options in Makefile. 
A threaded and a non threaded build is possible toggling flag -DTHREAD. Both show the notes in sync with what is heard (the sound card's output delay is compensated). The non threaded build keeps `TARGET_QUEUE_MS` (default 70 ms) of audio queued; raise it on very slow machines. Set `TARGET_FPS` to the monitor refresh rate for the smoothest scrolling.
OSS only, TinyALSA only or both is possible. If both are compiled in via -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA  - the OSS is tried first with TinyALSA as fallback.
`make SMALL_FONTS=1` leaves out the 10x20 font (about 17 KB): large screens then get the 6x13 font doubled, tripled ..., and a full HD window stays at the base size unless `+` is pressed.
`MASTER_VOLUME` in `midiero.c` sets the synth's level (default 0.7); a look-ahead limiter keeps peaks below -1 dBFS.
Options for the bundled mmidi (`-DMM_NO_REVERB`, `-DMM_NO_CHORUS`, `-DMAXVOICE=n`) go in `CFLAGS` in the environment, e.g. `CFLAGS="-DMM_NO_CHORUS" make`; see `mmidi/README.md`. i386 builds with SSE2 maths (`CFLAGS="-msse2 -mfpmath=sse"`) get the stack alignment they need from the Makefile.

## Usage

Simply launch the executable. If no arguments are provided, midiero boots instantly using its internal SoundFont and prompts you to select a song:

```bash
./midiero
```

Alternatively, pass files directly as command-line arguments:
```bash
./midiero song.mid custom_bank.sf2
```

## License

View license in top of source files.

