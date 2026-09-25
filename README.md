# midiero

A lightweight, interactive MIDI visualization and music learning tool for Linux. Built from scratch in pure C using **Xlib** for graphics and **TinyALSA** for real-time sound generation via an embedded SoundFont. 

Conceptually inspired by games like *Guitar Hero*, **midiero** focuses on helping users see, hear, and learn music through proper musical notation rather than abstract blocks.

With a fully static build utilizing `uclibc`, the entire application—including its internal SoundFont synthesizer—compiles into a standalone binary of **around 400 KB**, making it exceptionally resource-efficient and portable.

## Features

- **Live Musical Notation (Node Mode):** Renders notes onto an intelligent double-staff (Treble and Bass clefs) with dynamic support for accidentals (`#`) and ledger lines.
- **Classic Arcade View (Hero Mode):** Toggle instantly into a linear, geometric block-stream for quick visual decoding.
- **Dynamic Window Rescaling:** Seamlessly handles window resizing, maximizing, and full-screen modes, automatically adapting the staff scale and sidebar.
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
| `Q` / `Esc` | Quit the application |

## Architecture & Dependencies

midiero is crafted to be completely independent of bloated desktop frameworks (like GTK or Qt) and external server-side synths. 

- **Graphics & Input:** Pure X11 / Xlib
- **MIDI Parsing:** `tml.h` (Tiny MIDI Loader)
- **Sound Synthesis:** `tsf.h` (TinySoundFont)
- **Audio Output:** OSS and/or TinyALSA

## Installation & Compilation

Ensure you have the X11 development headers installed on your Linux system (e.g., `libx11-dev` on Debian/Ubuntu).

View the build options in Makefile. 
A threaded and a non threaded build is possible toggling flag -DTHREAD - for best graphics the threaded version is recommended.
OSS only, TinyALSA only or both is possible. If both are compiled in via -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA  - the OSS is tried first with TinyALSA as fallback.

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

