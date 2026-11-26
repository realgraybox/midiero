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
//midiero.c
#include "mmidi.h"		// SoundFont 2 synthesizer and MIDI file parser (mmidi/)

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "audio.h"
#include "bank.h"

#ifdef THREAD
#include <pthread.h>
#endif

// Text: xbmfont (xbmfont/), bitmap fonts built into the program, so midiero
// looks the same on every X server. Its implementation is compiled here.
#define XBMFONT_IMPLEMENTATION
#include "xbmfont.h"
#include "fonts/xbf_fixed_6x13.h"	// the look of X's own "fixed" font
#ifndef MIDIERO_SMALL_FONTS
#include "fonts/xbf_fixed_10x20.h"	// sharper text on large screens (about 17 KB)
#endif

//xfileselect area
#define FC_DEFAULT_WIDTH        500
#define FC_DEFAULT_HEIGHT       300
#define FC_FONT                 xbf_fixed_6x13
#define FC_STATUS_PADDING_Y     4
#define FC_HEADER_PADDING_Y     4
#define FC_LINE_EXTRA           2
#define FC_FILE_TEXT_X          10
#define FC_INFO_TEXT_X          300

#include "xfileselect.c"
//end xfileselect area

#define SAMPLE_RATE		48000		//48000 more universal than 44100
#define MASTER_VOLUME	0.7f		//about the loudness tsf had at -3 dB; the limiter catches the peaks
#define BUFFER_SIZE		2048		//threaded build: frames rendered per audio_write (the thread blocks on it)
#define WINDOW_WIDTH	800
#define WINDOW_HEIGHT	600
#define TARGET_FPS		60			//smoothest when equal to the monitor refresh rate
#define FRAME_TIME_MS	(1000.0 / TARGET_FPS)

#ifndef THREAD
// Single-threaded build: audio is rendered in small chunks, and only when the device
// has room, so the loop never waits for the sound card. TARGET_QUEUE_MS is how much
// audio is kept queued = how long one frame may take before the sound runs dry.
#define CHUNK_FRAMES		256		//~5.3 ms at 48 kHz
#define TARGET_QUEUE_MS		70
#define TARGET_QUEUE_FRAMES	(SAMPLE_RATE * TARGET_QUEUE_MS / 1000)
// An OSS driver may report up to one fragment more than is really queued (see
// audio_fragment_frames), so that much is added. Normally a few ms; with drivers
// that pick huge fragments (seen: 512 ms) this keeps the sound card fed.
#define QUEUE_TARGET()		(TARGET_QUEUE_FRAMES + audio_fragment_frames())
#else
// Threaded build: the audio thread is paced by its blocking writes. A driver with
// a huge buffer (seen: 16 s) would let it run seconds ahead, so pause and speed
// changes would be heard late; it waits while more than this is queued.
#define THREAD_QUEUE_MAX()	(4 * BUFFER_SIZE + 2 * audio_fragment_frames())
#endif

#include <math.h>

void set_window_icon(Display *dpy, Window win) {
	unsigned long icon_data[] = {
		8, 8, // width, height
		0xFF000000, 0xFF000000, 0xFF000000, 0xFF8B0000, 0xFF000000, 0xFF0000FF, 0xFF0000FF, 0xFF000000,
		0xFF000000, 0xFF00FFFF, 0xFF00FFFF, 0xFF8B0000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
		0xFF000000, 0xFF000000, 0xFF000000, 0xFF8B0000, 0xFF8A2BE2, 0xFF8A2BE2, 0xFF000000, 0xFF000000,
		0xFF8A2BE2, 0xFF8A2BE2, 0xFF000000, 0xFF8B0000, 0xFF8A2BE2, 0xFF000000, 0xFF000000, 0xFF000000,
		0xFF000000, 0xFF00FFFF, 0xFF00FFFF, 0xFF8B0000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
		0xFF000000, 0xFF000000, 0xFF000000, 0xFF8B0000, 0xFF000000, 0xFF0000FF, 0xFF0000FF, 0xFF000000,
		0xFF000000, 0xFF8A2BE2, 0xFF000000, 0xFF8B0000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
		0xFF000000, 0xFF000000, 0xFF000000, 0xFF8B0000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000
	};
   
    int data_length = 2 + (8 * 8); // 2 (measure) + (width * height)

    Atom wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    Atom cardinal = XInternAtom(dpy, "CARDINAL", False);
    XChangeProperty(dpy, win, wm_icon, cardinal, 32, PropModeReplace, (unsigned char*)icon_data, data_length);
    XFlush(dpy); 
}

// Global variables for playback control
double g_time_ms = 0.0;
double g_speed_modifier = 1.0;
bool g_is_paused = false;
bool g_channels_enabled[16];


char g_nuvaerende_midi_navn[256] = "No MIDI loaded - Use key [L]";
char g_nuvaerende_sf2_navn[256]  = "Build in Nokia Bank (Default)";

bool g_node_mode = true; 	// True = Node mode, False = Hero mode
bool g_show_bars = false; 	// True = show duration bars, False = hide them

// The loaded song: its events in play order, copied out of mmidi once per song
// (as tml handed out its message list). The synth only ever gets live events.
typedef struct {
    mm_event *ev;
    int count;
} song_t;

static song_t g_song = {NULL, 0};	// count 0 = no song loaded
static int g_current_ev = 0;		// next event to send

#ifdef THREAD
static pthread_t audio_tid;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool running = true;
static volatile int audio_error = 0;
#endif

// A uniform color function, ensuring that pieces and nodes always match 100%.
unsigned long hent_kanal_farve(int kanal) {
    if (kanal == 9) return 0xAAAAAA; // Drums are always gray.
    // Precise formula that delivers clear, distinct colors.
    unsigned char r = (kanal * 37) % 200 + 55;
    unsigned char g = (kanal * 73) % 200 + 55;
    unsigned char b = (kanal * 111) % 150 + 105;
    return (r << 16) | (g << 8) | b;
}

// Intelligent conversion from MIDI pitch to precise vertical note position (white keys)
int hent_node_trin(int key) {
    // Table showing the number of white keys from a C within an octave (0=C, 1=D, 2=E, 3=F, 4=G, 5=A, 6=B)
    static const int oktav_trin[] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    
    int oktav = (key / 12) - 5; // Centered around octave 5 (middle C)
    int note_i_oktav = key % 12;
    
    // Returns the total number of linear steps up/down from Middle C (MIDI 60).
    return (oktav * 7) + oktav_trin[note_i_oktav];
}

// Checks if a note is a black key (must have a sharp/accidental)
bool er_sort_tangent(int key) {
    int note = key % 12;
    return (note == 1 || note == 3 || note == 6 || note == 8 || note == 10);
}

// Helper for precise wall-clock timing
double get_time_in_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

static double get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

// =========================================================================
// OUTPUT STAGE: synth (float) -> look-ahead peak limiter -> 16-bit samples
// =========================================================================
// A hard cut of every sample above full scale sounds loud and scratchy. Instead
// the mix is rendered as float (mm_render_float, before mmidi's own soft knee),
// with MASTER_VOLUME leaving headroom, and a limiter turns the gain down smoothly just before a peak would
// pass LIMITER_THRESHOLD, then lets it recover over LIMITER_RELEASE_S.
// The look-ahead delays the sound by LIMITER_LOOKAHEAD frames (1.3 ms).
// Only called from the audio side (audio thread / single-threaded loop).
#define LIMITER_LOOKAHEAD	64			//frames
#define LIMITER_THRESHOLD	0.891f		//-1 dBFS
#define LIMITER_RELEASE_S	0.15f
#define RENDER_BLOCK		256			//frames rendered per mm_render_float() call

static void render_audio(mm_synth* f, short* out, int frames) {
    static float block[RENDER_BLOCK * 2];
    static float delay_line[LIMITER_LOOKAHEAD * 2];
    static float needed[LIMITER_LOOKAHEAD];	//gain each delayed frame needs (ring)
    static int pos = 0;
    static float gain = 1.0f;
    static float window_min = 1.0f;		//smallest 'needed' in the ring ...
    static int window_min_age = 0;		//... and how many frames ago it entered
    static bool init = false;
    static float attack, release;

    if (!init) {
        for (int i = 0; i < LIMITER_LOOKAHEAD; i++) needed[i] = 1.0f;
        attack  = (float)(1.0 - exp(-4.0 / LIMITER_LOOKAHEAD));	//~98 % of the way within the look-ahead
        release = (float)(1.0 - exp(-1.0 / (LIMITER_RELEASE_S * SAMPLE_RATE)));
        init = true;
    }

    while (frames > 0) {
        int n = frames > RENDER_BLOCK ? RENDER_BLOCK : frames;
        mm_render_float(f, block, n);

        for (int i = 0; i < n; i++) {
            float l = block[2 * i], r = block[2 * i + 1];
            float al = l < 0.0f ? -l : l, ar = r < 0.0f ? -r : r; //no fabsf: missing in old uClibc
            float peak = al > ar ? al : ar;
            float need = peak > LIMITER_THRESHOLD ? LIMITER_THRESHOLD / peak : 1.0f;

            // Smallest gain needed by any frame within the look-ahead window
            needed[pos] = need;
            if (need <= window_min) {
                window_min = need;
                window_min_age = 0;
            } else if (++window_min_age >= LIMITER_LOOKAHEAD) {
                window_min = 1.0f; // the minimum left the window: rescan (rare)
                for (int k = 0; k < LIMITER_LOOKAHEAD; k++) {
                    if (needed[k] <= window_min) {
                        window_min = needed[k];
                        window_min_age = (pos - k + LIMITER_LOOKAHEAD) % LIMITER_LOOKAHEAD;
                    }
                }
            }
            gain += (window_min - gain) * (window_min < gain ? attack : release);

            // Output the frame that entered LIMITER_LOOKAHEAD frames ago
            float ol = delay_line[2 * pos] * gain, orr = delay_line[2 * pos + 1] * gain;
            delay_line[2 * pos] = l;
            delay_line[2 * pos + 1] = r;
            pos = (pos + 1) % LIMITER_LOOKAHEAD;

            if (ol > 1.0f) ol = 1.0f; else if (ol < -1.0f) ol = -1.0f; //safety only
            if (orr > 1.0f) orr = 1.0f; else if (orr < -1.0f) orr = -1.0f;
            out[2 * i] = (short)(ol * 32767.0f);
            out[2 * i + 1] = (short)(orr * 32767.0f);
        }
        out += 2 * n;
        frames -= n;
    }
}

// =========================================================================
// PLAYBACK CLOCK (shared by the threaded and the single-threaded build)
// =========================================================================
// The audio side records, for every chunk written to the device, which part of the
// song it contains (the "timeline"), and samples how much audio is still queued in
// the device. The graphics side runs a smooth clock counted in device frames that
// follows the real playback position, and looks up the song time of the frame being
// heard right now. This compensates the output delay of the sound card exactly, and
// handles pauses (silence chunks) and speed changes chunk by chunk.
//
// The device's "queued" report is often coarse (updated per fragment/period). Such a
// report can only claim MORE is queued than really is, so the clock follows the most
// advanced report seen (slowly forgetting it, to track clock drift), never the average.
// It glides towards that by at most +/-2 % speed, and only jumps when more than 100 ms
// off (start, underrun).
//
// Threaded build: call clock_audio_written(), clock_song_time() and
// clock_set_song_time() with audio_mutex held.

#define TIMELINE_SIZE		512					//chunks remembered (>= device buffer / chunk size)
#define CLOCK_SNAP_FRAMES	(SAMPLE_RATE / 10)	//more than 100 ms off: jump
#define CLOCK_MAX_ADJUST	0.02				//glide by at most +/-2 % speed ...
#define CLOCK_ADJUST_SPAN	(SAMPLE_RATE / 100)	//... reached at 10 ms error
#define CLOCK_FORGET		0.05				//frames/ms the best report is forgotten (~1 ms per s)

typedef struct {
    long long frame_start, frame_end;	//position in the stream of all frames ever written
    double song_start, song_end;		//song time (ms) at the start/end of the chunk
} tl_chunk;

static tl_chunk g_tl[TIMELINE_SIZE];
static int g_tl_head = 0;				//next slot to write
static int g_tl_count = 0;
static double g_tl_base = 0.0;			//song time before anything has been written
static long long g_frames_written = 0;

static bool g_meas_valid = false;
static double g_meas_offset = 0.0;		//playing frame minus wall time in frames (from the driver)
static double g_meas_wall = 0.0;

static bool g_clk_valid = false;
static bool g_clk_stalled = false;		//reached the end of the written audio (underrun)
static double g_clk_frame = 0.0;		//smoothed playing position, in frames
static double g_clk_wall = 0.0;

// Frames queued in the device. If the driver cannot tell, it is estimated by
// assuming the device plays SAMPLE_RATE frames per second.
// Only called from the audio side (audio thread / single-threaded loop).
static int playback_queued_frames(void) {
    static double est_played = 0.0, est_wall = 0.0;
    int q = audio_queued_frames();
    if (q >= 0) return q;
#ifdef DEBUG_TIMING
    static bool told = false;
    if (!told) {
        fprintf(stderr, "DEBUG_TIMING: the audio driver cannot report its queue; estimating it from the clock\n");
        told = true;
    }
#endif

    double now = get_monotonic_ms();
    if (est_wall != 0.0) est_played += (now - est_wall) * SAMPLE_RATE / 1000.0; //first call: playback starts now
    est_wall = now;
    if (est_played > (double)g_frames_written) est_played = (double)g_frames_written; //underrun
    return (int)(g_frames_written - (long long)est_played);
}

#ifndef THREAD
// =========================================================================
// X BACKLOG (single-threaded build): the main loop must never wait for the X server,
// or the sound card runs dry. Window moves make the server slow, and some window
// managers grab it for up to a second at the start of a drag. So instead of XSync()
// we look at how much we have sent that the X server has not read yet, and skip
// drawing while it is behind. Linux reports this for the socket (TIOCOUTQ); where it
// cannot, every frame is drawn and XSync() is used as before.
// =========================================================================
#include <sys/ioctl.h>
#define X_BACKLOG_LIMIT	(32 * 1024)		//bytes the X server may lag behind before frames are skipped

static int x_backlog(Display* d) {
    int pending = 0;
    if (ioctl(ConnectionNumber(d), TIOCOUTQ, &pending) < 0) return -1;
    return pending;
}
#endif

#if defined(DEBUG_TIMING) && !defined(THREAD)
// =========================================================================
// DEBUG_TIMING (single-threaded build): where does the main loop spend its time
// when the audio queue runs low? Build with: make DEBUG_TIMING=1
// Prints to stderr - redirect it to a file (2> timing.log) so a slow terminal
// does not cause stalls itself.
// =========================================================================
#define DBG_SLOW_MS		25.0	// report any loop phase that takes longer than this
#define DBG_START_S		3.0		// report every loop this long after playback starts

typedef struct {
    double events, audio, draw, xsync, sleep;	// ms spent in each phase
    int x_events, chunks;
    double render_max, write_max;				// slowest single chunk render / write (ms)
    double q_start, q_after;					// queue (ms) at loop start / after topping up
    bool paused;
    int x_backlog;								// bytes the X server had not read yet (-1 = unknown)
    bool skipped;								// frame not drawn because X was behind
} dbg_loop_t;

static dbg_loop_t g_dbg;			// loop being measured
static bool g_dbg_valid = false;
static double g_dbg_t0 = -1.0;		// program start (ms)
static double g_dbg_play = -1.0;	// when playback last started (ms), -1 = paused

static double dbg_frames_ms(int frames) { return frames * 1000.0 / SAMPLE_RATE; }

// Called at the top of every loop with the queue level now: reports the loop that just ended
static void dbg_loop_end(double now, int queued_now, bool paused_now) {
    if (g_dbg_t0 < 0.0) g_dbg_t0 = now;
    if (!paused_now && g_dbg_play < 0.0) {
        g_dbg_play = now;
        fprintf(stderr, "[%8.3f s] ---- playback started, queue %.1f ms (target %.1f ms = %d + fragment %.1f)\n",
                (now - g_dbg_t0) / 1000.0, dbg_frames_ms(queued_now), dbg_frames_ms(QUEUE_TARGET()),
                TARGET_QUEUE_MS, dbg_frames_ms(audio_fragment_frames()));
    } else if (paused_now) {
        g_dbg_play = -1.0;
    }

    if (g_dbg_valid) {
        const dbg_loop_t* d = &g_dbg;
        double q_now = dbg_frames_ms(queued_now);
        bool low = !d->paused && q_now < TARGET_QUEUE_MS / 4.0;
        bool slow = d->events > DBG_SLOW_MS || d->audio > DBG_SLOW_MS || d->draw > DBG_SLOW_MS ||
                    d->xsync > DBG_SLOW_MS || d->sleep > DBG_SLOW_MS;
        bool start = g_dbg_play >= 0.0 && now - g_dbg_play < DBG_START_S * 1000.0;
        if (low || slow || start || d->skipped) {
            fprintf(stderr, "[%8.3f s] %-5s queue %5.1f -> %5.1f -> now %5.1f ms | events %5.1f (%2d X) | "
                            "audio %5.1f (%2d chunks, render max %4.1f, write max %4.1f) | draw %5.1f | "
                            "x flush %5.1f | sleep %5.1f | X backlog %5.1f KB%s%s\n",
                    (now - g_dbg_t0) / 1000.0, low ? "LOW" : slow ? "SLOW" : d->skipped ? "SKIP" : "start",
                    d->q_start, d->q_after, q_now, d->events, d->x_events,
                    d->audio, d->chunks, d->render_max, d->write_max, d->draw,
                    d->xsync, d->sleep, d->x_backlog / 1024.0,
                    d->skipped ? " (frame skipped)" : "", d->paused ? " (paused)" : "");
        }
    }
    memset(&g_dbg, 0, sizeof g_dbg);
    g_dbg.q_start = dbg_frames_ms(queued_now);
    g_dbg.paused = paused_now;
    g_dbg_valid = true;
}
#endif

// Audio side: a chunk of 'frames' frames covering song time song_start..song_end
// (equal while paused) has just been written to the device.
static void clock_audio_written(int frames, double song_start, double song_end) {
    tl_chunk* c = &g_tl[g_tl_head];
    c->frame_start = g_frames_written;
    g_frames_written += frames;
    c->frame_end = g_frames_written;
    c->song_start = song_start;
    c->song_end = song_end;
    g_tl_head = (g_tl_head + 1) % TIMELINE_SIZE;
    if (g_tl_count < TIMELINE_SIZE) g_tl_count++;

    double now = get_monotonic_ms();
    double offset = (double)(g_frames_written - playback_queued_frames()) - now * SAMPLE_RATE / 1000.0;
    if (!g_meas_valid || offset < g_meas_offset - CLOCK_SNAP_FRAMES) {
        g_meas_offset = offset; // first report, or the device fell behind (underrun)
    } else {
        double forgotten = g_meas_offset - (now - g_meas_wall) * CLOCK_FORGET;
        g_meas_offset = offset > forgotten ? offset : forgotten; // most advanced report
    }
    g_meas_wall = now;
    g_meas_valid = true;
}

// New song loaded: the audio still queued (silence, we are paused) now belongs to it.
static void clock_set_song_time(double song_ms) {
    for (int i = 0; i < g_tl_count; i++) {
        g_tl[i].song_start = song_ms;
        g_tl[i].song_end = song_ms;
    }
    g_tl_base = song_ms;
}

// Song time of a stream position, interpolated within its chunk.
static double timeline_lookup(double frame) {
    const tl_chunk* oldest = NULL;
    for (int i = 0; i < g_tl_count; i++) {
        const tl_chunk* c = &g_tl[(g_tl_head - 1 - i + TIMELINE_SIZE) % TIMELINE_SIZE];
        if (frame >= (double)c->frame_start) {
            double len = (double)(c->frame_end - c->frame_start);
            double frac = len > 0.0 ? (frame - (double)c->frame_start) / len : 0.0;
            if (frac > 1.0) frac = 1.0;
            return c->song_start + (c->song_end - c->song_start) * frac;
        }
        oldest = c;
    }
    return oldest ? oldest->song_start : g_tl_base;
}

// Graphics side, once per frame: song time (ms) of the sound being heard right now.
static double clock_song_time(void) {
    if (!g_meas_valid) return g_tl_base;

    double now = get_monotonic_ms();
    double target = g_meas_offset + now * SAMPLE_RATE / 1000.0;
    if (target > (double)g_frames_written) target = (double)g_frames_written; //cannot play unwritten audio

    if (!g_clk_valid) {
        g_clk_frame = target;
        g_clk_valid = true;
    } else if (g_clk_stalled) {
        // Wait at the end of the audio until the device really plays again, then follow it
        if (target > g_clk_frame) {
            g_clk_frame = target;
            g_clk_stalled = false;
        }
    } else {
        double advance = (now - g_clk_wall) * SAMPLE_RATE / 1000.0;
        double err = target - (g_clk_frame + advance);
        if (err > CLOCK_SNAP_FRAMES || err < -CLOCK_SNAP_FRAMES) {
            g_clk_frame = target;
        } else {
            double adjust = err / CLOCK_ADJUST_SPAN * CLOCK_MAX_ADJUST;
            if (adjust > CLOCK_MAX_ADJUST) adjust = CLOCK_MAX_ADJUST;
            if (adjust < -CLOCK_MAX_ADJUST) adjust = -CLOCK_MAX_ADJUST;
            g_clk_frame += advance * (1.0 + adjust); //always forward
        }
    }
    if (g_clk_frame >= (double)g_frames_written) {
        g_clk_frame = (double)g_frames_written;
        g_clk_stalled = true;
    }
    g_clk_wall = now;

    return timeline_lookup(g_clk_frame);
}

// Send one song event to the synth (used by both the threaded and the single-threaded build).
// play_notes = false is used when replaying instrument/controller state onto a new SoundFont.
// Muting only suppresses note-ons: program, bank, controller and pitch-bend changes are
// always applied, so an unmuted channel has the right instrument and settings.
static void send_midi_event(mm_synth* f, const mm_event* e, bool play_notes) {
    int type = e->status & 0xF0, ch = e->status & 0x0F;
    if (e->status == 0xFF)
        return; // tempo: already applied to the event times
    if (e->status == 0xFE) {
        // Format 2: the next sequence starts. mmidi releases the sounding notes; the channels
        // keep their instrument and controllers.
        mm_midi(f, 0xFE, 0, 0);
        return;
    }

    switch (type) {
    case 0x90: // note on (a note-on with velocity 0 arrives as a note-off)
        if (!play_notes)
            break;
        if (g_channels_enabled[ch])
            mm_midi(f, e->status, e->d1, e->d2);
        break;
    case 0x80:
        if (play_notes)
            mm_midi(f, e->status, e->d1, e->d2);
        break;
    case 0xC0: // program change: channel 10 picks a drum kit, as in General MIDI
    case 0xB0: // volume, expression, pan, sustain, bank select, RPN pitch-bend range ...
        mm_midi(f, e->status, e->d1, e->d2);
        break;
    case 0xE0:
        mm_midi(f, e->status, e->d1, e->d2);
        break;
    }
}

// Put all 16 channels back to their power-on state (new song, rewind, new SoundFont):
// all voices and effects off, General MIDI defaults (volume 100, piano, channel 10 drums).
static void reset_channels(mm_synth* f) {
    mm_reset(f);
}

// =========================================================================
// NOTE TABLE (graphics only - built and used by the main thread)
// =========================================================================
// Every note's start and end is worked out once per song, so drawing never has to
// search the MIDI list for note-offs. The table is sorted by start time (the MIDI
// list is), so a frame can binary-search the first visible note and stop at the
// right edge of the window.
typedef struct {
    double start_ms;
    double end_ms;			// < 0: no note-off found (drawn as head only, like before)
    unsigned char channel, key;
} node_t;

static node_t* g_noder = NULL;
static int g_node_antal = 0;
static double g_max_node_ms = 0.0;	// longest note: how far back a visible bar can start

static void byg_node_tabel(const song_t* song) {
    static int aaben[16][128];	// newest open note per channel/key (-1 = none) ...
    int antal = 0;

    free(g_noder);
    g_noder = NULL;
    g_node_antal = 0;
    g_max_node_ms = 0.0;

    for (int i = 0; i < song->count; i++)
        if ((song->ev[i].status & 0xF0) == 0x90) antal++;
    if (antal == 0) return;

    g_noder = (node_t*)malloc((size_t)antal * sizeof(node_t));
    int* forrige = (int*)malloc((size_t)antal * sizeof(int));	// ... chained to the older open ones
    if (!g_noder || !forrige) {
        fprintf(stderr, "Out of memory for the note table\n");
        free(g_noder);
        free(forrige);
        g_noder = NULL;
        return;
    }
    for (int c = 0; c < 16; c++)
        for (int k = 0; k < 128; k++) aaben[c][k] = -1;

    for (int i = 0; i < song->count; i++) {
        const mm_event* m = &song->ev[i];
        int type = m->status & 0xF0;
        if (type != 0x90 && type != 0x80) continue;
        int ch = m->status & 0x0F, key = m->d1 & 0x7F;
        double time_ms = m->time * 1000.0;

        if (type == 0x90) {
            node_t* n = &g_noder[g_node_antal];
            n->start_ms = time_ms;
            n->end_ms = -1.0;
            n->channel = (unsigned char)ch;
            n->key = (unsigned char)key;
            forrige[g_node_antal] = aaben[ch][key];
            aaben[ch][key] = g_node_antal++;
        } else {
            // A note-off ends every open note of this key, exactly like the old
            // per-frame look-ahead (which took the first note-off after each note-on)
            for (int k = aaben[ch][key]; k >= 0; k = forrige[k]) {
                g_noder[k].end_ms = time_ms;
                double d = time_ms - g_noder[k].start_ms;
                if (d > g_max_node_ms) g_max_node_ms = d;
            }
            aaben[ch][key] = -1;
        }
    }
    free(forrige);
}

// =========================================================================
// INSTRUMENT STRETCHES (legend only - built and used by the main thread)
// =========================================================================
// A stretch is one instrument (bank + program) on one channel, from its first note
// to the end of its last note there. The legend shows a channel's instrument only
// while the song position being heard is inside such a stretch, and "Not used"
// before, between and after them. So instruments that are done, or have moved to
// another channel, no longer fill up the legend. Long rests inside a stretch keep
// the instrument shown. The table is sorted by first note (built in song order).
typedef struct {
    double first_ms, last_ms;	// start of the first note, end of the last note
    int bank, prog;				// as the channel selected them (CC 0 / program change)
    unsigned char channel;
} straekning_t;

static straekning_t* g_straek = NULL;
static int g_straek_antal = 0;

static void byg_straekninger(const song_t* song) {
    static int note_straek[16][128];	// stretch of the sounding note per channel/key (-1 = none)
    int bank[16], prog[16], cur[16];	// channel state while walking the song; cur = open stretch
    int max_antal = 0;

    free(g_straek);
    g_straek = NULL;
    g_straek_antal = 0;

    for (int i = 0; i < song->count; i++)
        if ((song->ev[i].status & 0xF0) == 0x90) max_antal++;	// at most one stretch per note
    if (max_antal == 0) return;

    g_straek = (straekning_t*)malloc((size_t)max_antal * sizeof(straekning_t));
    if (!g_straek) {
        fprintf(stderr, "Out of memory for the instrument stretches\n");
        return;
    }
    for (int c = 0; c < 16; c++) {
        bank[c] = 0; prog[c] = 0; cur[c] = -1;	// General MIDI power-on state, as reset_channels()
        for (int k = 0; k < 128; k++) note_straek[c][k] = -1;
    }

    for (int i = 0; i < song->count; i++) {
        const mm_event* m = &song->ev[i];
        if (m->status == 0xFE) {	// format 2: the next sequence starts. mmidi releases the
            double time_ms = m->time * 1000.0;	// sounding notes there; the channels keep their instrument
            for (int c = 0; c < 16; c++)
                for (int k = 0; k < 128; k++) {
                    int s = note_straek[c][k];
                    if (s >= 0 && time_ms > g_straek[s].last_ms) g_straek[s].last_ms = time_ms;
                    note_straek[c][k] = -1;
                }
            continue;
        }
        if (m->status >= 0xF0) continue;
        int type = m->status & 0xF0, ch = m->status & 0x0F;
        double time_ms = m->time * 1000.0;

        if (type == 0xC0) {
            prog[ch] = m->d1;
        } else if (type == 0xB0 && m->d1 == 0) {
            bank[ch] = m->d2;		// bank select, as mmidi uses it
        } else if (type == 0x90) {
            int key = m->d1 & 0x7F;
            straekning_t* st = cur[ch] >= 0 ? &g_straek[cur[ch]] : NULL;
            if (!st || st->bank != bank[ch] || st->prog != prog[ch]) {
                // First note with a new instrument on this channel: a new stretch begins
                cur[ch] = g_straek_antal++;
                st = &g_straek[cur[ch]];
                st->first_ms = st->last_ms = time_ms;
                st->bank = bank[ch];
                st->prog = prog[ch];
                st->channel = (unsigned char)ch;
            }
            note_straek[ch][key] = cur[ch];
        } else if (type == 0x80) {
            // The note ends the stretch it started in, even if the program changed meanwhile
            int k = note_straek[ch][m->d1 & 0x7F];
            if (k >= 0 && time_ms > g_straek[k].last_ms) g_straek[k].last_ms = time_ms;
            note_straek[ch][m->d1 & 0x7F] = -1;
        }
    }
}

// Preset of a stretch, chosen the way mmidi chooses it for a note: channel 10 plays
// drum kits (bank 128, falling back to kit 0), the others fall back to bank 0.
static int straek_preset(const mm_synth* syn, const straekning_t* st) {
    int p;
    if (st->channel == 9) {
        if ((p = mm_preset_find(syn, 128, st->prog)) < 0) p = mm_preset_find(syn, 128, 0);
    } else if ((p = mm_preset_find(syn, st->bank, st->prog)) < 0) {
        p = mm_preset_find(syn, 0, st->prog);
    }
    return p;
}

// Index of the first note starting at or after t
static int foerste_node_fra(double t) {
    int lo = 0, hi = g_node_antal;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_noder[mid].start_ms < t) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// =========================================================================
// BATCHED DRAWING: a colour change and a few X requests per channel, instead
// of several requests and a colour change per note. Drawn in layers: all
// duration bars, then all noteheads and sharps, then the ledger lines - so a
// bar never covers a notehead.
// =========================================================================
#define BATCH 256

typedef struct {
    XRectangle bars[BATCH];		// duration bars (node mode) / blocks (hero mode)
    XRectangle ends[BATCH];		// end marks of the duration bars
    XArc       heads[BATCH];	// noteheads
    XPoint     sharps[BATCH];	// position of each '#'
    int n_bars, n_ends, n_heads, n_sharps;
} kanal_batch_t;

static kanal_batch_t g_batch[16];
static xbf_ctx* g_txt = NULL;		// text font, uploaded once to the X server

// --- Size: on a large screen everything (text, panels, notes, lines) grows ---
// The layout is designed for 800x600 with a 13 px font; U(x) is a base-size
// measure at the current size, which is set by the text's line height.
typedef struct { const xbf_font* font; int scale; } ui_size_t;
static const ui_size_t g_sizes[] = {
#ifndef MIDIERO_SMALL_FONTS
    { &xbf_fixed_6x13, 1 }, { &xbf_fixed_10x20, 1 }, { &xbf_fixed_6x13, 2 },
    { &xbf_fixed_10x20, 2 }, { &xbf_fixed_10x20, 3 }, { &xbf_fixed_10x20, 4 },
#else
    { &xbf_fixed_6x13, 1 }, { &xbf_fixed_6x13, 2 }, { &xbf_fixed_6x13, 3 },
    { &xbf_fixed_6x13, 4 }, { &xbf_fixed_6x13, 6 },
#endif
};
#define N_SIZES ((int)(sizeof g_sizes / sizeof g_sizes[0]))
static int g_size = 0;				// index in g_sizes
static int g_size_manual = -1;		// chosen with +/-, or -1 = follow the window size
static int g_unit = 13;				// line height of the current size (13 = base)
#define U(x) (((x) * g_unit + 6) / 13)

// Line height of a size
static int size_line(int i) { return g_sizes[i].font->height * g_sizes[i].scale; }

// Largest size whose scaled 800x600 layout fits in the window
static int size_for_window(int w, int h) {
    int best = 0;
    for (int i = 1; i < N_SIZES; i++)
        if (size_line(i) * 800 <= w * 13 && size_line(i) * 600 <= h * 13) best = i;
    return best;
}

// Switch to size i: upload its font (only when it changes). False if X refused.
static bool set_size(Display* d, Drawable like, int i) {
    if (g_txt && i == g_size) return true;
    xbf_ctx* t = xbf_create(d, like, g_sizes[i].font, g_sizes[i].scale);
    if (!t) return g_txt != NULL;	// keep the current size
    xbf_free(g_txt);
    g_txt = t;
    g_size = i;
    g_unit = size_line(i);
    x11_filechooser_font(g_sizes[i].font, g_sizes[i].scale);	// dialogs match
    return true;
}
static XRectangle g_ledger_c[BATCH], g_ledger_ydre[BATCH];	// middle-C ledger / outer ledger lines
static int g_n_ledger_c = 0, g_n_ledger_ydre = 0;

// Send everything collected so far, layer by layer, and empty the batches
static void tegn_batches(Display* d, Drawable p, GC gc) {
    for (int ch = 0; ch < 16; ch++) {				// 1. bars and their end marks
        kanal_batch_t* b = &g_batch[ch];
        if (!b->n_bars && !b->n_ends) continue;
        XSetForeground(d, gc, hent_kanal_farve(ch));
        if (b->n_bars) XFillRectangles(d, p, gc, b->bars, b->n_bars);
        if (b->n_ends) XFillRectangles(d, p, gc, b->ends, b->n_ends);
        b->n_bars = b->n_ends = 0;
    }
    for (int ch = 0; ch < 16; ch++) {				// 2. noteheads and sharps
        kanal_batch_t* b = &g_batch[ch];
        if (!b->n_heads && !b->n_sharps) continue;
        XSetForeground(d, gc, hent_kanal_farve(ch));
        if (b->n_heads) XFillArcs(d, p, gc, b->heads, b->n_heads);
        for (int i = 0; i < b->n_sharps; i++)
            xbf_draw(g_txt, p, b->sharps[i].x, b->sharps[i].y, hent_kanal_farve(ch), "#");
        b->n_heads = b->n_sharps = 0;
    }
    if (g_n_ledger_c) {								// 3. ledger lines
        XSetForeground(d, gc, 0x666666);
        XFillRectangles(d, p, gc, g_ledger_c, g_n_ledger_c);
        g_n_ledger_c = 0;
    }
    if (g_n_ledger_ydre) {
        XSetForeground(d, gc, 0x444444);
        XFillRectangles(d, p, gc, g_ledger_ydre, g_n_ledger_ydre);
        g_n_ledger_ydre = 0;
    }
}

// Batch of a channel with room for one more note of each kind (sends all early if full)
static kanal_batch_t* batch_til(Display* d, Drawable p, GC gc, int ch) {
    kanal_batch_t* b = &g_batch[ch];
    if (b->n_bars >= BATCH || b->n_ends >= BATCH || b->n_heads >= BATCH || b->n_sharps >= BATCH)
        tegn_batches(d, p, gc);
    return b;
}

// Horizontal line from x1 to x2 (both included) at y, lw pixels thick
static void tilfoej_ledger(Display* d, Drawable p, GC gc, bool midter_c, int x1, int x2, int y, int lw) {
    if (g_n_ledger_c >= BATCH || g_n_ledger_ydre >= BATCH) tegn_batches(d, p, gc);
    XRectangle* r = midter_c ? &g_ledger_c[g_n_ledger_c++] : &g_ledger_ydre[g_n_ledger_ydre++];
    r->x = (short)x1; r->y = (short)(y - lw / 2);
    r->width = (unsigned short)(x2 - x1 + 1); r->height = (unsigned short)lw;
}

// Function to load a MIDI file (format 0, 1 or 2, also RIFF RMID) into *ny_song.
// Returns false on failure. The caller swaps it in and frees the old song, so a failed
// load keeps the current song. A short-lived mmidi synth parses the file; the events
// are copied out, so the song does not depend on the synth (nor on a SoundFont swap).
bool skift_midi_fil(const char* sti, song_t* ny_song) {
    char* rent_navn = strrchr(sti, '/');
    const char* navn = rent_navn ? rent_navn + 1 : sti;
    mm_synth* parser = mm_create(SAMPLE_RATE);
    int n = 0;

    ny_song->ev = NULL;
    ny_song->count = 0;
    if (parser && mm_load_file(parser, sti) == 0 && (n = mm_event_count(parser)) > 0 &&
        (ny_song->ev = (mm_event*)malloc((size_t)n * sizeof(mm_event)))) {
        for (int i = 0; i < n; i++) mm_get_event(parser, i, &ny_song->ev[i]);
        ny_song->count = n;
    }
    mm_free(parser);

    if (!ny_song->count) {
        snprintf(g_nuvaerende_midi_navn, sizeof(g_nuvaerende_midi_navn), "Error loading %s", navn);
        return false;
    }

    // Save the clean filename without the path to the header.
    snprintf(g_nuvaerende_midi_navn, sizeof(g_nuvaerende_midi_navn), "%s", navn);
    printf("Successfully loaded MIDI: %s\n", g_nuvaerende_midi_navn);
    return true;
}

static void song_free(song_t* song) {
    free(song->ev);
    song->ev = NULL;
    song->count = 0;
}

// A synth for a bank, at the output format: sti == NULL means the built-in bank
static mm_synth* lav_synth(const char* sti) {
    mm_synth* s = mm_create(SAMPLE_RATE);
    if (!s) return NULL;
    if ((sti ? mm_load_bank(s, sti) : mm_load_bank_mem(s, Nokia_6230i_RM_72__sf2, Nokia_6230i_RM_72__sf2_len)) < 0) {
        mm_free(s);
        return NULL;
    }
    mm_set_volume(s, MASTER_VOLUME);
    return s;
}

// Function to dynamically load/switch a SoundFont (.sf2). sti == NULL means the built-in bank.
// song/song_pos: the song and the next event to be played, used to replay the
// instrument and controller state onto the new bank so it continues seamlessly.
// The (slow) file loading happens outside the audio lock; only the swap is locked.
void skift_soundfont(const char* sti, mm_synth** nuvaerende_synth, const song_t* song, int song_pos) {
    mm_synth* ny_synth = NULL;

    if (sti == NULL) {
        ny_synth = lav_synth(NULL);
        snprintf(g_nuvaerende_sf2_navn, sizeof(g_nuvaerende_sf2_navn), "Built-in Nokia Bank (Default)");
    } else {
        ny_synth = lav_synth(sti);
        if (!ny_synth) {
            ny_synth = lav_synth(NULL);
            snprintf(g_nuvaerende_sf2_navn, sizeof(g_nuvaerende_sf2_navn), "Error! Switched to built-in fallback.");
        } else {
            char* rent_navn = strrchr(sti, '/');
            snprintf(g_nuvaerende_sf2_navn, sizeof(g_nuvaerende_sf2_navn), "%s", rent_navn ? rent_navn + 1 : sti);
        }
    }
    if (!ny_synth) {
        fprintf(stderr, "Could not load any SoundFont\n");
        return; // keep the current one (if any)
    }

#ifdef THREAD
    pthread_mutex_lock(&audio_mutex);
#endif
    // --- SYNCHRONIZATION OF THE NEW SOUNDFONT ---
    // Replay every program/bank/controller/pitch-bend message up to the current position.
    reset_channels(ny_synth);
    for (int i = 0; i < song_pos && i < song->count; i++)
        send_midi_event(ny_synth, &song->ev[i], false);

    mm_synth* gammel_synth = *nuvaerende_synth;
    *nuvaerende_synth = ny_synth;
#ifdef THREAD
    pthread_mutex_unlock(&audio_mutex);
#endif
    if (gammel_synth)
        mm_free(gammel_synth); // Close the old synth bank (the audio thread no longer uses it)

    printf("Active SoundFont reloaded and synchronized: %s\n", g_nuvaerende_sf2_navn);
}

#ifdef THREAD
// All shared state (g_time_ms, g_current_ev, g_is_paused, the playback clock) is only
// read and written while holding audio_mutex; only the blocking audio_write() is unlocked.
static void *audio_thread(void *arg) {
    mm_synth **synth_ptr = (mm_synth **)arg;

    short sample_buffer[BUFFER_SIZE * 2];

    while (running) {
        // Normally the blocking write paces this loop; see THREAD_QUEUE_MAX
        while (running && playback_queued_frames() > THREAD_QUEUE_MAX())
            usleep(2000);

        pthread_mutex_lock(&audio_mutex);

        double chunk_song_start = g_time_ms; //song time covered by this chunk (for the clock)
        double chunk_song_end = g_time_ms;

        if (!g_is_paused && g_song.count) {
            double chunk_time_ms = ((double)BUFFER_SIZE / SAMPLE_RATE) * 1000.0;
            double next_time_ms = g_time_ms + chunk_time_ms * g_speed_modifier;

            // Process and handle all MIDI events belonging to this time interval.
            while (g_current_ev < g_song.count && g_song.ev[g_current_ev].time * 1000.0 <= next_time_ms) {
                send_midi_event(*synth_ptr, &g_song.ev[g_current_ev], true);
                if (++g_current_ev == g_song.count)
                    mm_all_notes_off(*synth_ptr); // end of the song: notes without a note-off ring out
            }

            render_audio(*synth_ptr, sample_buffer, BUFFER_SIZE);
            g_time_ms = next_time_ms;
            chunk_song_end = next_time_ms;

            // Pause and go back to start
            if (g_current_ev >= g_song.count && mm_active_voices(*synth_ptr) == 0) {
                g_time_ms = 0.0;
                g_is_paused = true;
                g_current_ev = 0;
                reset_channels(*synth_ptr);

                printf("The song is over. Rewinds to the start and pauses.\n");
            }
        } else {
            // Keep the device fed with silence while paused (no underruns)
            memset(sample_buffer, 0, sizeof(sample_buffer));
        }

        pthread_mutex_unlock(&audio_mutex);

        // Blocks until the device has room: this paces the thread
        if (audio_write(sample_buffer, BUFFER_SIZE) < 0) {
            perror("audio_write");
            audio_error = 1;
            running = false;
            break;
        }

        pthread_mutex_lock(&audio_mutex);
        clock_audio_written(BUFFER_SIZE, chunk_song_start, chunk_song_end);
        pthread_mutex_unlock(&audio_mutex);
    }
    return NULL;
}

#endif

int main(int argc, char *argv[]) {

	char* soundfont_path = NULL;
    char* midi_path = NULL;

	// Initialize arrays
    for (int i = 0; i < 16; i++) g_channels_enabled[i] = true;

    // 1. Initialize the audio backend (OSS/TinyALSA) immediately
    if (audio_init(SAMPLE_RATE, 2) < 0) {
        fprintf(stderr, "Could not initialize audio\n");
        return 1;
    }

    // 2. Create an empty pointer for the synth
    mm_synth* g_synth = NULL;

    // 3. Check arguments at startup
    if (argc >= 2) {
        // If there is an argument, we always assume it is a MIDI file
        skift_midi_fil(argv[1], &g_song);
        byg_node_tabel(&g_song);
        byg_straekninger(&g_song);
        g_time_ms = 0.0;
        g_current_ev = 0;
    }
    
    // If an sf2 file is specified as argument 2, use it; otherwise, use the built-in one.
    if (argc >= 3) {
        skift_soundfont(argv[2], &g_synth, &g_song, 0);
    } else {
        skift_soundfont(NULL, &g_synth, &g_song, 0); // Uses the embedded Nokia bank automatically
    }
    if (!g_synth) {
        audio_close();
        song_free(&g_song);
        free(g_noder);
        free(g_straek);
        return 1;
    }

    // 5. Initialize X11 Window
    Display* display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Error: Could not open X display\n");
        audio_close();
        song_free(&g_song);
        free(g_noder);
        free(g_straek);
        mm_free(g_synth);
        return 1;
    }

    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 
                                        10, 10, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
                                        BlackPixel(display, screen), BlackPixel(display, screen));

    // StructureNotifyMask: the window size arrives as ConfigureNotify events, so we
    // never have to ask the X server for it (a full round trip) every frame.
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    int win_w = WINDOW_WIDTH, win_h = WINDOW_HEIGHT;
    XStoreName(display, window, "Midiero - The Interactive MIDI player");
    
    XMapWindow(display, window);
    set_window_icon(display, window);

    // --- NEW CODE: Tell X11 that we want to handle the close button (X) ourselves ---
	Atom wm_delete_window;
	// 1. Tell X11 that we want to access the "WM_DELETE_WINDOW" atom
	wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);

	// 2. Register this atom as a protocol on your window
	XSetWMProtocols(display, window, &wm_delete_window, 1);

    GC gc = XCreateGC(display, window, 0, NULL);
    Pixmap pixmap = XCreatePixmap(display, window, WINDOW_WIDTH, WINDOW_HEIGHT, DefaultDepth(display, screen));

    if (!set_size(display, window, 0)) {
        fprintf(stderr, "Could not upload the font to the X server\n");
        return 1;
    }
#ifdef THREAD
	bool audio_thread_started = (pthread_create(&audio_tid, NULL, audio_thread, &g_synth) == 0);
	if (!audio_thread_started) {
		fprintf(stderr, "Could not start the audio thread\n");
		running = false;
	}
#endif
    // 6. Main loop
#ifndef THREAD
	short sample_buffer[CHUNK_FRAMES * 2]; // Stereo buffer
	bool running = true;
#endif
	double next_frame_ms = get_monotonic_ms();
#if defined(DEBUG_TIMING) && !defined(THREAD)
	double dbg_mark = 0.0;
#endif
    while (running) {
#if defined(DEBUG_TIMING) && !defined(THREAD)
        dbg_mark = get_monotonic_ms();
        dbg_loop_end(dbg_mark, playback_queued_frames(), g_is_paused || !g_song.count);
#endif
        int target_channel = -1;
#ifdef THREAD
        while (XPending(display)) {
#else
        // Read what has arrived without flushing our output: that could block on a busy X server
        while (XEventsQueued(display, QueuedAfterReading)) {
#endif
            XEvent event;
            XNextEvent(display, &event);
#if defined(DEBUG_TIMING) && !defined(THREAD)
            g_dbg.x_events++;
#endif
            // Allows the game to be closed using the window's "X" button with the mouse.
			if (event.type == ClientMessage) {
				if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
					running = false;
				}
			}
			// The window was resized (or the window manager placed it at another size)
			if (event.type == ConfigureNotify && event.xconfigure.width > 0 && event.xconfigure.height > 0) {
				win_w = event.xconfigure.width;
				win_h = event.xconfigure.height;
			}
            if (event.type == KeyPress) {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                if (keysym == XK_Escape || keysym == XK_q || keysym == XK_Q) {
                    running = false;
                } else if (keysym == XK_space) {
#ifdef THREAD					
					pthread_mutex_lock(&audio_mutex);
					
                    g_is_paused = !g_is_paused;
                    pthread_mutex_unlock(&audio_mutex);
#else
					g_is_paused = !g_is_paused;
#endif                 
                } else if (keysym == XK_Up) {
#ifdef THREAD
					pthread_mutex_lock(&audio_mutex);
					g_speed_modifier += 0.1;
					if (g_speed_modifier > 2.5)
						g_speed_modifier = 2.5;
					pthread_mutex_unlock(&audio_mutex);
#else					
                    g_speed_modifier += 0.1;
                    if (g_speed_modifier > 2.5) g_speed_modifier = 2.5;
#endif
                } else if (keysym == XK_Down) {
#ifdef THREAD
					pthread_mutex_lock(&audio_mutex);
					g_speed_modifier -= 0.1;
					if (g_speed_modifier < 0.1)
						g_speed_modifier = 0.1;
					pthread_mutex_unlock(&audio_mutex);
#else
                    g_speed_modifier -= 0.1;
                    if (g_speed_modifier < 0.1) g_speed_modifier = 0.1;
#endif
				}
				else if (keysym == XK_plus || keysym == XK_equal || keysym == XK_KP_Add) {
					g_size_manual = g_size < N_SIZES - 1 ? g_size + 1 : g_size;	// bigger
				}
				else if (keysym == XK_minus || keysym == XK_KP_Subtract) {
					g_size_manual = g_size > 0 ? g_size - 1 : 0;				// smaller
				}
				else if (keysym == XK_a || keysym == XK_A) {
					g_size_manual = -1;											// follow the window again
				}
				else if (keysym == XK_t || keysym == XK_T) {
					g_node_mode = !g_node_mode; // Switch between Hero and Node view!
				}
				else if (keysym == XK_l || keysym == XK_L) {
#ifdef THREAD
					pthread_mutex_lock(&audio_mutex);
					g_is_paused = true; // Pause while selecting a file
					mm_all_notes_off(g_synth);
					pthread_mutex_unlock(&audio_mutex);
#else
					g_is_paused = true; // Pause while selecting a file
                    mm_all_notes_off(g_synth);
#endif
                     midi_path = x11_filechooser(NULL,
						"Midiero - The Interactive MIDI player",	//title
						"Select MIDI file for Midiero",	// header_text
						".mid, .midi",	//filter extension
						FC_MODE_FILE,	//select mode
						"[q/Esc]: Cancel [Enter]: Open [Click]: Select");			//help text / footer area
                    if (midi_path) {
                        // Load outside the lock; on failure the current song is kept
                        song_t ny_song;
                        bool ok = skift_midi_fil(midi_path, &ny_song);
                        free(midi_path);

                        if (ok) {
                            song_t gammel_song = g_song;
#ifdef THREAD
							pthread_mutex_lock(&audio_mutex);
#endif
							g_is_paused = true;
							reset_channels(g_synth);
							g_song = ny_song;
							g_current_ev = 0;
							g_time_ms = 0.0; // Rewind to the beginning
							clock_set_song_time(0.0); // audio still queued belongs to the new song
#ifdef THREAD
							pthread_mutex_unlock(&audio_mutex);
#endif
							song_free(&gammel_song); // the audio thread no longer references it
							byg_node_tabel(&g_song); // graphics only: no lock needed
							byg_straekninger(&g_song);
                        }
                    }
				}
				else if (keysym == XK_f || keysym == XK_F) {
#ifdef THREAD
					pthread_mutex_lock(&audio_mutex);
					g_is_paused = true;
					mm_all_notes_off(g_synth);
					pthread_mutex_unlock(&audio_mutex);
#else
                    g_is_paused = true;
                    mm_all_notes_off(g_synth);
#endif                   
                    soundfont_path = x11_filechooser(NULL, 
						"Midiero - The Interactive MIDI player",
						"Select a SoundFont .sf2",
						".sf2",
						FC_MODE_FILE,
						"[q/Esc]: Cancel [Enter]: Open [Click]: Select");
                    
                    // If the user selects a file, it is loaded. If cancelled (NULL),
                    // we can either keep the current one or force the built-in one.
                    if (soundfont_path) {
                        // skift_soundfont() takes the audio lock itself, only for the swap.
                        // Safe to read g_current_ev here: the song is paused.
                        skift_soundfont(soundfont_path, &g_synth, &g_song, g_current_ev);
                        free(soundfont_path);
                    }
                }
                else if (keysym == XK_v || keysym == XK_V) {
                    g_show_bars = !g_show_bars; // Toggle duration bars
                }
				
                // CHANNEL CONTROL (1-16)              
				// 1. Check Shift combinations FIRST (otherwise they will be caught by the if-statement below)
				if (event.xkey.state & ShiftMask) {
					if (keysym >= XK_1 && keysym <= XK_6) {
						target_channel = 10 + (keysym - XK_1); //Subtract XK_1 to get 0–5, plus 10 = tracks 11–16.
					}
				}
				// 2. Standard presses of keys 1–9 (without Shift)
				else if (keysym >= XK_1 && keysym <= XK_9) {
					target_channel = keysym - XK_1; // Giver 0-8 (spor 1-9)
				}
				// 3. Normal press of 0 (Remember the two equals signs: ==)
				else if (keysym == XK_0) {
					target_channel = 9; // Yields 9 (track 10)
				}
                // 4. If a valid channel was pressed, toggle its status.
#ifdef THREAD                
                if (target_channel >= 0 && target_channel < 16) {
					pthread_mutex_lock(&audio_mutex);
					
                    g_channels_enabled[target_channel] = !g_channels_enabled[target_channel];
                    if (!g_channels_enabled[target_channel]) {
						mm_midi(g_synth, 0xB0 | target_channel, 123, 0); // all notes off on this channel
					}
					pthread_mutex_unlock(&audio_mutex);
				}
#else
				if (target_channel >= 0 && target_channel < 16) {
                    g_channels_enabled[target_channel] = !g_channels_enabled[target_channel];
                    if (!g_channels_enabled[target_channel]) {
						mm_midi(g_synth, 0xB0 | target_channel, 123, 0); // all notes off on this channel
                    }
                }
#endif                
            }
        }
#if defined(DEBUG_TIMING) && !defined(THREAD)
        { double t = get_monotonic_ms(); g_dbg.events = t - dbg_mark; dbg_mark = t; }
#endif
#ifndef THREAD
        // --- AUDIO: top the device up to TARGET_QUEUE_MS in small chunks ---
        // Only writes while there is room, so the loop does not wait for the sound card
        // and can draw at TARGET_FPS.
        double chunk_time_ms = ((double)CHUNK_FRAMES / SAMPLE_RATE) * 1000.0;
        int queue_target = QUEUE_TARGET();
        for (int n = 0; running && n < queue_target / CHUNK_FRAMES + 4; n++) {
            if (playback_queued_frames() >= queue_target) break;

            // Pause and go back to start
            if (g_song.count && g_current_ev >= g_song.count && mm_active_voices(g_synth) == 0) {
                g_time_ms = 0.0;               // Reset the time
                g_is_paused = true;            // Pause
                g_current_ev = 0;              // Rewind MIDI to the start
                reset_channels(g_synth);         // Instruments/controllers back to the song's start state

                printf("The song is over. Rewinds to the start and pauses.\n");
            }

            double chunk_song_start = g_time_ms;
            if (!g_is_paused && g_song.count) {
                // Advance the time based on the fixed audio chunk and the speed.
                double next_time_ms = g_time_ms + (chunk_time_ms * g_speed_modifier);

                // Process and handle all MIDI events belonging to this time interval.
                while (g_current_ev < g_song.count && g_song.ev[g_current_ev].time * 1000.0 <= next_time_ms) {
                    send_midi_event(g_synth, &g_song.ev[g_current_ev], true);
                    if (++g_current_ev == g_song.count)
                        mm_all_notes_off(g_synth); // end of the song: notes without a note-off ring out
                }

#if defined(DEBUG_TIMING)
                double dbg_r = get_monotonic_ms();
#endif
                render_audio(g_synth, sample_buffer, CHUNK_FRAMES);
#if defined(DEBUG_TIMING)
                dbg_r = get_monotonic_ms() - dbg_r;
                if (dbg_r > g_dbg.render_max) g_dbg.render_max = dbg_r;
#endif
                g_time_ms = next_time_ms;
            } else {
                // Paused: keep the device fed with silence (no underruns)
                memset(sample_buffer, 0, sizeof(sample_buffer));
            }

            double write_start_ms = get_monotonic_ms();
            if (audio_write(sample_buffer, CHUNK_FRAMES) < 0) {
                perror("audio_write");
                running = false;
                break;
            }
            clock_audio_written(CHUNK_FRAMES, chunk_song_start, g_time_ms);
#if defined(DEBUG_TIMING)
            {
                double w = get_monotonic_ms() - write_start_ms;
                if (w > g_dbg.write_max) g_dbg.write_max = w;
                g_dbg.chunks++;
            }
#endif

            // The write had to wait: the device buffer is smaller than TARGET_QUEUE_MS
            // (or the driver under-reports its delay). Stop here so drawing is not held up.
            if (get_monotonic_ms() - write_start_ms > chunk_time_ms * 0.5) break;
        }
#endif

#if defined(DEBUG_TIMING) && !defined(THREAD)
        { double t = get_monotonic_ms(); g_dbg.audio = t - dbg_mark; dbg_mark = t;
          g_dbg.q_after = dbg_frames_ms(playback_queued_frames()); }
#endif
        // --- Song time of the sound being heard right now (both builds) ---
#ifdef THREAD
        pthread_mutex_lock(&audio_mutex);
#endif
        double graphic_time_ms = clock_song_time();
#ifdef THREAD
        pthread_mutex_unlock(&audio_mutex);
#endif
#ifndef THREAD
        // X is behind (window being moved, server grabbed ...): keep the audio going, draw later
        int backlog = x_backlog(display);
#if defined(DEBUG_TIMING)
        g_dbg.x_backlog = backlog;
#endif
        if (backlog > X_BACKLOG_LIMIT) {
#if defined(DEBUG_TIMING)
            g_dbg.skipped = true;
#endif
            goto frame_done;
        }
#endif
		// =========================================================================
        // 7. FULLY DYNAMIC & SCALABLE GRAPHICS (Xlib resizing supported)
        // =========================================================================
        
        // Current window dimensions (kept up to date by ConfigureNotify events)
        int W = win_w;
        int H = win_h;

		// If the window size has changed, free the old pixmap and create a new one
        // (This ensures that double-buffering works in full-screen mode)
        static int last_w = 0, last_h = 0;
        if (W != last_w || H != last_h) {
            XFreePixmap(display, pixmap);
            pixmap = XCreatePixmap(display, window, W, H, DefaultDepth(display, screen));
            last_w = W; last_h = H;
        }

        // Size: from the window, unless chosen with +/-
        set_size(display, window, g_size_manual >= 0 ? g_size_manual : size_for_window(W, H));

        // Clear the screen (Pixmap)
        XSetForeground(display, gc, BlackPixel(display, screen));
        XFillRectangle(display, pixmap, gc, 0, 0, W, H);
        

        // Define the dynamic layout zones based on the current screen size.
        // U(x): a measure of the 800x600 design at the current size.
        int lw = U(1);          // line width
        int base_h = H * 13 / g_unit;	// window height in base-size pixels
        double px_ms = g_unit / 130.0;	// time axis: 0.1 px per ms at base size
        int top_h = U(40);      // Height of top beam
        int bot_h = U(60);      // Height of bottom beam
        int side_w = U(240);    // Side panel width (Legend)
        int play_x_start = side_w; // The playing area starts where the side panel ends.
        int play_w = W - side_w;   // The rest of the screen is used for sheet music.
        
        // The red "NOW" line is now moved close to the left side of the playing area,
        // giving you the best possible view into the future (the right side)!
        int dynamic_center_x = play_x_start + (int)(play_w * 0.15); 
        int play_bottom_y = H - bot_h;

        // --- ZONE 1: ALWAYS VISIBLE HEADER (Top) ---
        XSetForeground(display, gc, 0x222222); 
        XFillRectangle(display, pixmap, gc, 0, 0, W, top_h);
        char header_str[600]; // room for both 256-char names

		snprintf(header_str, sizeof(header_str), "  Midiero :: File: %s :: Bank: %s :: Resolution: %dx%d :: Size: %d%s", g_nuvaerende_midi_navn, g_nuvaerende_sf2_navn, W, H, g_size + 1, g_size_manual >= 0 ? "" : " (auto)");
        
        xbf_draw_fit(g_txt, pixmap, U(10), (top_h - xbf_line_height(g_txt)) / 2 + xbf_ascent(g_txt),
                     WhitePixel(display, screen), header_str, W - U(20), XBF_CUT_END);
        XSetForeground(display, gc, 0x444444);
        XFillRectangle(display, pixmap, gc, 0, top_h, W, lw);

		// --- ZONE 2: INTELLIGENT PLAYING SURFACE (Now vertically scalable) ---
        int center_y = top_h + (play_bottom_y - top_h) / 2; // The vertical center is adjusted automatically.
        int linje_afstand = U(base_h > 700 ? 14 : 10);      // Increase the line spacing if the screen is large!

        if (g_node_mode) {
            // Draw the staff lines (G and F clefs).
            XSetForeground(display, gc, 0x333333);
            int g_trin[] = {2, 4, 6, 8, 10};
            for (int l = 0; l < 5; l++) {
                int y_linje = center_y - (g_trin[l] * linje_afstand / 2);
                if (y_linje > top_h && y_linje < play_bottom_y) {
                    XFillRectangle(display, pixmap, gc, play_x_start, y_linje - lw / 2, W - play_x_start, lw);
                }
            }
            int f_trin[] = {-12, -10, -8, -6, -4};
            for (int l = 0; l < 5; l++) {
                int y_linje = center_y - (f_trin[l] * linje_afstand / 2);
                if (y_linje > top_h && y_linje < play_bottom_y) {
                    XFillRectangle(display, pixmap, gc, play_x_start, y_linje - lw / 2, W - play_x_start, lw);
                }
            }

            // Red "NOW" playhead
            XSetForeground(display, gc, 0xFF0000); 
            XFillRectangle(display, pixmap, gc, dynamic_center_x - lw / 2, top_h, lw, play_bottom_y - top_h + 1);

            // Define the dimensions of the node head first (so they are known for the calculation).
            int node_w = U(base_h > 700 ? 18 : 14);
            int node_h = U(base_h > 700 ? 10 : 8);

			// Only notes that can be on screen: the first candidate is found by binary
			// search (notes are sorted by start time); the loop stops at the right edge.
			// With bars on, a note that started up to g_max_node_ms earlier can still show.
			double venstre_ms = (dynamic_center_x - play_x_start + U(40)) / px_ms;
			double t_min = graphic_time_ms - venstre_ms - (g_show_bars ? g_max_node_ms : 0.0);

			for (int i = foerste_node_fra(t_min); i < g_node_antal; i++) {
				const node_t* n = &g_noder[i];
				int x_pos = dynamic_center_x + (int)((n->start_ms - graphic_time_ms) * px_ms);
				if (x_pos >= W) break; // all later notes start even further right
				if (!g_channels_enabled[n->channel]) continue;

				int trin = hent_node_trin(n->key);
				int y_pos = center_y - (trin * linje_afstand / 2);
				if (y_pos <= top_h || y_pos >= play_bottom_y) continue;

				// --- DURATION OF THE NOTE (LENGTH OF THE BEAM) ---
				int node_laengde_pixels = U(16); // Standard minimum width (head only)
				if (g_show_bars && n->end_ms >= 0.0) {
					node_laengde_pixels = (int)((n->end_ms - n->start_ms) * px_ms); // 1ms = 0.1px at base size
					if (node_laengde_pixels < node_w) node_laengde_pixels = node_w;
				}

				// Calculate where the bar ends (if g_show_bars is false, x_end is simply equal to the end of the notehead).
				int x_ende = x_pos + node_laengde_pixels;

				// Draw only if the note or its beam is visible on the playing area.
				if (x_ende <= play_x_start) continue;

				kanal_batch_t* b = batch_til(display, pixmap, gc, n->channel);

				// 1. The duration bar itself (ONLY if the toggle is ON and it is actually longer than the head).
				//    Clipped to the window so the coordinates stay within X's 16-bit range.
				if (g_show_bars && node_laengde_pixels > node_w) {
					int bx1 = x_pos + (node_w / 2);
					int bx2 = x_ende;
					if (bx1 < play_x_start - U(10)) bx1 = play_x_start - U(10);
					if (bx2 > W + U(10)) bx2 = W + U(10);
					if (bx2 > bx1) {
						XRectangle* r = &b->bars[b->n_bars++];
						r->x = (short)bx1; r->y = (short)(y_pos - U(2));
						r->width = (unsigned short)(bx2 - bx1); r->height = (unsigned short)U(4);
					}
					// A small vertical end line at the end of the beam.
					if (x_ende < W + U(10)) {
						XRectangle* e = &b->ends[b->n_ends++];
						e->x = (short)(x_ende - lw / 2); e->width = (unsigned short)lw;
						e->y = (short)(y_pos - U(4)); e->height = (unsigned short)(2 * U(4) + 1);
					}
				}

				// Head, sharp and ledger lines: skip if hidden behind the legend panel
				if (x_pos + node_w + U(10) < play_x_start) continue;

				// 2. The round notehead itself (drawn after the bars of its channel)
				XArc* a = &b->heads[b->n_heads++];
				a->x = (short)x_pos; a->y = (short)(y_pos - (node_h / 2));
				a->width = (unsigned short)node_w; a->height = (unsigned short)node_h;
				a->angle1 = 0; a->angle2 = 360 * 64;

				// 3. Sharps (#) and ledger lines
				if (er_sort_tangent(n->key)) {
					XPoint* sp = &b->sharps[b->n_sharps++];
					sp->x = (short)(x_pos - U(8)); sp->y = (short)(y_pos + U(4));
				}
				if (n->key == 60 || n->key == 61)
					tilfoej_ledger(display, pixmap, gc, true, x_pos - U(4), x_pos + node_w + U(4), y_pos, lw);
				if ((trin >= 12 || trin <= -14) && trin % 2 == 0)
					tilfoej_ledger(display, pixmap, gc, false, x_pos - U(3), x_pos + node_w + U(3), y_pos, lw);
			}

			// Send the batches: bars, then heads and sharps, then ledger lines
			tegn_batches(display, pixmap, gc);
		} else {
            // Hero mode
            XSetForeground(display, gc, 0x1A1A1A);
            int grid_step = (play_bottom_y - top_h) / 15;
            if (grid_step < 1) grid_step = 1; // window very small: avoid an endless loop
            for (int i = top_h + grid_step; i < play_bottom_y; i += grid_step) {
                XFillRectangle(display, pixmap, gc, play_x_start, i - lw / 2, W - play_x_start, lw);
            }

            XSetForeground(display, gc, 0xFF0000); 
            XFillRectangle(display, pixmap, gc, dynamic_center_x - lw / 2, top_h, lw, play_bottom_y - top_h + 1);

            double venstre_ms = (dynamic_center_x - play_x_start + U(40)) / px_ms;
            for (int i = foerste_node_fra(graphic_time_ms - venstre_ms); i < g_node_antal; i++) {
                const node_t* n = &g_noder[i];
                int x_pos = dynamic_center_x + (int)((n->start_ms - graphic_time_ms) * px_ms);
                if (x_pos >= W) break; // all later notes start even further right
                if (x_pos <= play_x_start || !g_channels_enabled[n->channel]) continue;

                // Spreads the tones across the entire available height.
                float pct = (float)(n->key - 30) / 60.0f; // assume a range of 60 notes
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;
                int y_pos = play_bottom_y - (int)(pct * (play_bottom_y - top_h - U(20))) - U(10);
                if (y_pos <= top_h || y_pos >= play_bottom_y) continue;

                kanal_batch_t* b = batch_til(display, pixmap, gc, n->channel);
                XRectangle* r = &b->bars[b->n_bars++];
                r->x = (short)x_pos; r->y = (short)(y_pos - U(6));
                r->width = (unsigned short)U(16); r->height = (unsigned short)U(12);
            }
            tegn_batches(display, pixmap, gc);
        }

        // --- ZONE 3: ADLIB INSTRUMENT LEGEND ---
        XSetForeground(display, gc, 0x151515); 
        XFillRectangle(display, pixmap, gc, 0, top_h, side_w, play_bottom_y - top_h);
        XSetForeground(display, gc, 0x333333);
        XFillRectangle(display, pixmap, gc, side_w, top_h, lw, play_bottom_y - top_h + 1); // Vertical dividing line

        xbf_draw(g_txt, pixmap, U(15), top_h + U(20), 0x00FF00, "INSTRUMENT LEGEND:");

        // Dynamic spacing in the list based on screen height, so it doesn't get cramped in full-screen mode.
        int y_spacing = (H - U(100)) / 17;
        if (y_spacing > U(30)) y_spacing = U(30); // Maximum distance for the sake of appearance
        int y_offset = top_h + U(45);

        // The instrument stretch each channel is in at the song position being heard
        // (the latest started one if two overlap); NULL = the channel is not used now
        const straekning_t* aktiv[16] = {NULL};
        for (int i = 0; i < g_straek_antal && g_straek[i].first_ms <= graphic_time_ms; i++)
            if (graphic_time_ms <= g_straek[i].last_ms) aktiv[g_straek[i].channel] = &g_straek[i];

        for (int i = 0; i < 16; i++) {
            char legend_item[128];
            char key_hint[16];
            if (i < 9) snprintf(key_hint, sizeof(key_hint), "%d", i + 1);
            else if (i == 9) snprintf(key_hint, sizeof(key_hint), "0");
            else snprintf(key_hint, sizeof(key_hint), "S+%d", i - 9);

            // Name of the preset this channel's current instrument plays (bank-aware)
            char preset_name[21] = "";
            if (aktiv[i] && mm_preset_info(g_synth, straek_preset(g_synth, aktiv[i]), NULL, NULL, preset_name) < 0)
                snprintf(preset_name, sizeof preset_name, "%s", (i == 9) ? "Standard Drum Kit" : "Acoustic Grand Piano");

            if (!g_channels_enabled[i]) {
                // Muted: stays visible so the mute can be seen and undone, in use or not
                XSetForeground(display, gc, 0x442222);
                XFillRectangle(display, pixmap, gc, U(15), y_offset - U(8), U(10), U(8));
                if (aktiv[i])
                    snprintf(legend_item, sizeof(legend_item), "[%s] Ch%02d: [MUTED] %.10s", key_hint, i + 1, preset_name);
                else
                    snprintf(legend_item, sizeof(legend_item), "[%s] Ch%02d: [MUTED] Not used", key_hint, i + 1);
                xbf_draw(g_txt, pixmap, U(35), y_offset, aktiv[i] ? 0x777777 : 0x444444, legend_item);
            }
            else if (!aktiv[i]) {
                XSetForeground(display, gc, 0x333333);
                XFillRectangle(display, pixmap, gc, U(15), y_offset - U(8), U(10), U(8));
                snprintf(legend_item, sizeof(legend_item), "[%s] Ch%02d: Not used", key_hint, i + 1);
                xbf_draw(g_txt, pixmap, U(35), y_offset, 0x444444, legend_item);
            }
            else {
                XSetForeground(display, gc, hent_kanal_farve(i));
                XFillRectangle(display, pixmap, gc, U(15), y_offset - U(8), U(10), U(8));
                snprintf(legend_item, sizeof(legend_item), "[%s] Ch%02d: %.16s", key_hint, i + 1, preset_name);
                xbf_draw(g_txt, pixmap, U(35), y_offset, WhitePixel(display, screen), legend_item);
            }
            y_offset += y_spacing;
        }

                // --- ZONE 4: FOOTER (Slim bottom bar at the bottom of the screen) ---
                XSetForeground(display, gc, 0x111111);
                XFillRectangle(display, pixmap, gc, 0, play_bottom_y, W, bot_h);
                XSetForeground(display, gc, 0x444444);
                XFillRectangle(display, pixmap, gc, 0, play_bottom_y, W, lw);
                char help_str_fmt[128];
				snprintf(help_str_fmt, sizeof(help_str_fmt), "[Space]:Pause [L]:Load MIDI [F]:Load SF2 [T]:Toggle View [V]:Bars [Arrows Up/Down]:Speed (%.1fx) [+/-]:Size [A]:Auto [Q]:Quit", g_speed_modifier);
              
                xbf_draw_fit(g_txt, pixmap, U(20), H - U(40), 0xBBBBBB, help_str_fmt, W - U(40), XBF_CUT_END);
                char status_str[128];

				if (!g_song.count) {
					snprintf(status_str, sizeof(status_str), "STATUS: READY  |  Press [L] to load a MIDI file to start!");
				} else {
					snprintf(status_str, sizeof(status_str), "STATUS: %s  |  PLAYBACK TIME: %.2f s", g_is_paused ? "PAUSE  " : "PLAYING", graphic_time_ms / 1000.0);
				}
                xbf_draw_fit(g_txt, pixmap, U(20), H - U(20), 0x00FF00, status_str, W - U(40), XBF_CUT_END);
				// Copy the entire dynamic frame to the window				
				XCopyArea(display, pixmap, window, gc, 0, 0, W, H, 0, 0);
#if defined(DEBUG_TIMING) && !defined(THREAD)
				{ double t = get_monotonic_ms(); g_dbg.draw = t - dbg_mark; dbg_mark = t; }
#endif
#ifdef THREAD
				// Wait until the X server has executed this frame, so frames can never
				// pile up in its queue (and lag behind the sound) on a slow display.
				XSync(display, False);
#else
				// Send the frame without waiting for it: the backlog check above keeps frames
				// from piling up. Only if the backlog cannot be measured, wait as before.
				if (backlog < 0) XSync(display, False);
				else XFlush(display);
#if defined(DEBUG_TIMING)
				{ double t = get_monotonic_ms(); g_dbg.xsync = t - dbg_mark; dbg_mark = t; }
#endif
frame_done:
#endif

				// Sleep until the next frame is due (absolute deadline, so drawing time
				// does not lower the frame rate). If far behind (e.g. after a file
				// dialog) start counting again from now instead of racing to catch up.
				next_frame_ms += FRAME_TIME_MS;
				double now_ms = get_monotonic_ms();
				if (next_frame_ms < now_ms - FRAME_TIME_MS) next_frame_ms = now_ms;
				if (next_frame_ms > now_ms) usleep((unsigned int)((next_frame_ms - now_ms) * 1000.0));
#if defined(DEBUG_TIMING) && !defined(THREAD)
				g_dbg.sleep = get_monotonic_ms() - dbg_mark;
#endif
    }

    // 8. Clean up
#ifdef THREAD
    running = false;
	if (audio_thread_started) pthread_join(audio_tid, NULL);
	if (audio_error) fprintf(stderr, "Audio output stopped because of an error.\n");
#endif

    xbf_free(g_txt);
    XFreePixmap(display, pixmap);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    
    audio_close();
    song_free(&g_song);
    free(g_noder);
    free(g_straek);
    mm_free(g_synth);
    return 0;
}
