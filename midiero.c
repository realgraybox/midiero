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
#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TML_IMPLEMENTATION
#include "tml.h"

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

//xfileselect area
#define FC_DEFAULT_WIDTH        500
#define FC_DEFAULT_HEIGHT       300
#define FC_FONT_NAME            "fixed"
#define FC_STATUS_PADDING_Y     4
#define FC_HEADER_PADDING_Y     4
#define FC_LINE_EXTRA           2
#define FC_FILE_TEXT_X          10
#define FC_INFO_TEXT_X          300

#include "xfileselect.c"
//end xfileselect area

#define SAMPLE_RATE		48000		//48000 more universal than 44100
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
#endif

#if defined(__UCLIBC__)
#include <math.h>
float expf (float x) { return (float) exp( (double)x ); }
float powf (float x, float y) {	return (float) pow( (double)x, (double)y ); }
#endif

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


int g_channel_preset_index[16] = {0}; // Preset the synth really uses per channel (bank-aware, for the legend)
bool g_channel_has_played[16] = {false}; // Checking whether the track actually contains music.
char g_nuvaerende_midi_navn[256] = "No MIDI loaded - Use key [L]";
char g_nuvaerende_sf2_navn[256]  = "Build in Nokia Bank (Default)";

bool g_node_mode = true; 	// True = Node mode, False = Hero mode
bool g_show_bars = false; 	// True = show duration bars, False = hide them

#ifdef THREAD
static tml_message *g_current_msg = NULL;
static pthread_t audio_tid;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool running = true;
static volatile int audio_error = 0;
tml_message *g_midi = NULL;
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

    double now = get_monotonic_ms();
    if (est_wall != 0.0) est_played += (now - est_wall) * SAMPLE_RATE / 1000.0; //first call: playback starts now
    est_wall = now;
    if (est_played > (double)g_frames_written) est_played = (double)g_frames_written; //underrun
    return (int)(g_frames_written - (long long)est_played);
}

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

// Send one MIDI message to the synth (used by both the threaded and the single-threaded build).
// play_notes = false is used when replaying instrument/controller state onto a new SoundFont.
// Muting only suppresses note-ons: program, bank, controller and pitch-bend changes are
// always applied, so an unmuted channel has the right instrument and settings.
static void send_midi_event(tsf* f, const tml_message* msg, bool play_notes) {
    switch (msg->type) {
    case TML_NOTE_ON:
    case TML_NOTE_OFF:
    case TML_PROGRAM_CHANGE:
    case TML_PITCH_BEND:
    case TML_CONTROL_CHANGE:
        break;
    default:
        return; // meta/sysex messages: their channel field is not initialized by tml
    }
    if (msg->channel >= 16)
        return;

    switch (msg->type) {
    case TML_NOTE_ON:
        if (!play_notes)
            break;
        if (msg->velocity > 0)
            g_channel_has_played[msg->channel] = true; // The track is now in use!
        if (g_channels_enabled[msg->channel])
            tsf_channel_note_on(f, msg->channel, msg->key, msg->velocity / 127.0f);
        break;
    case TML_NOTE_OFF:
        if (play_notes)
            tsf_channel_note_off(f, msg->channel, msg->key);
        break;
    case TML_PROGRAM_CHANGE:
        tsf_channel_set_presetnumber(f, msg->channel, msg->program, (msg->channel == 9));
        g_channel_preset_index[msg->channel] = tsf_channel_get_preset_index(f, msg->channel);
        break;
    case TML_PITCH_BEND:
        tsf_channel_set_pitchwheel(f, msg->channel, msg->pitch_bend);
        break;
    case TML_CONTROL_CHANGE:
        // Volume, expression, pan, sustain, bank select (MSB/LSB), RPN pitch-bend range ...
        tsf_channel_midi_control(f, msg->channel, msg->control, msg->control_value);
        break;
    }
}

// Put all 16 channels back to their power-on state (new song, rewind, new SoundFont).
static void reset_channels(tsf* f) {
    tsf_reset(f); // stop all voices
    for (int i = 0; i < 16; i++) {
        tsf_channel_midi_control(f, i, 121, 0); // Reset All Controllers (volume, pan, bank, pitch range ...)
        tsf_channel_set_sustain(f, i, 0);
        tsf_channel_set_pitchwheel(f, i, 8192);
        tsf_channel_set_presetnumber(f, i, 0, (i == 9));
        g_channel_preset_index[i] = tsf_channel_get_preset_index(f, i);
    }
}

// Function to load a MIDI file. Returns the new song, or NULL on failure.
// The caller swaps it in and frees the old song, so a failed load keeps the current song.
tml_message* skift_midi_fil(const char* sti) {
    tml_message* ny_midi = tml_load_filename(sti);
    char* rent_navn = strrchr(sti, '/');
    const char* navn = rent_navn ? rent_navn + 1 : sti;

    if (!ny_midi) {
        snprintf(g_nuvaerende_midi_navn, sizeof(g_nuvaerende_midi_navn), "Error loading %s", navn);
        return NULL;
    }

    // Save the clean filename without the path to the header.
    snprintf(g_nuvaerende_midi_navn, sizeof(g_nuvaerende_midi_navn), "%s", navn);
    printf("Successfully loaded MIDI: %s\n", g_nuvaerende_midi_navn);
    return ny_midi;
}

// Function to dynamically load/switch a SoundFont (.sf2). sti == NULL means the built-in bank.
// song_start/song_pos: the song and the next message to be played, used to replay the
// instrument and controller state onto the new bank so it continues seamlessly.
// The (slow) file loading happens outside the audio lock; only the swap is locked.
void skift_soundfont(const char* sti, tsf** nuvaerende_tsf, tml_message* song_start, tml_message* song_pos) {
    tsf* ny_tsf = NULL;

    if (sti == NULL) {
        ny_tsf = tsf_load_memory(Nokia_6230i_RM_72__sf2, Nokia_6230i_RM_72__sf2_len);
        snprintf(g_nuvaerende_sf2_navn, sizeof(g_nuvaerende_sf2_navn), "Built-in Nokia Bank (Default)");
    } else {
        ny_tsf = tsf_load_filename(sti);
        if (!ny_tsf) {
            ny_tsf = tsf_load_memory(Nokia_6230i_RM_72__sf2, Nokia_6230i_RM_72__sf2_len);
            snprintf(g_nuvaerende_sf2_navn, sizeof(g_nuvaerende_sf2_navn), "Error! Switched to built-in fallback.");
        } else {
            char* rent_navn = strrchr(sti, '/');
            snprintf(g_nuvaerende_sf2_navn, sizeof(g_nuvaerende_sf2_navn), "%s", rent_navn ? rent_navn + 1 : sti);
        }
    }
    if (!ny_tsf) {
        fprintf(stderr, "Could not load any SoundFont\n");
        return; // keep the current one (if any)
    }

    // Configure the new output format
    tsf_set_output(ny_tsf, TSF_STEREO_INTERLEAVED, SAMPLE_RATE, 0.0f);

#ifdef THREAD
    pthread_mutex_lock(&audio_mutex);
#endif
    // --- SYNCHRONIZATION OF THE NEW SOUNDFONT ---
    // Replay every program/bank/controller/pitch-bend message up to the current position.
    reset_channels(ny_tsf);
    for (tml_message* m = song_start; m && m != song_pos; m = m->next)
        send_midi_event(ny_tsf, m, false);

    tsf* gammel_tsf = *nuvaerende_tsf;
    *nuvaerende_tsf = ny_tsf;
#ifdef THREAD
    pthread_mutex_unlock(&audio_mutex);
#endif
    if (gammel_tsf)
        tsf_close(gammel_tsf); // Close the old synth bank (the audio thread no longer uses it)

    printf("Active SoundFont reloaded and synchronized: %s\n", g_nuvaerende_sf2_navn);
}

#ifdef THREAD
// All shared state (g_time_ms, g_current_msg, g_is_paused, the playback clock) is only
// read and written while holding audio_mutex; only the blocking audio_write() is unlocked.
static void *audio_thread(void *arg) {
    tsf **tsf_ptr = (tsf **)arg;

    short sample_buffer[BUFFER_SIZE * 2];

    while (running) {
        pthread_mutex_lock(&audio_mutex);

        double chunk_song_start = g_time_ms; //song time covered by this chunk (for the clock)
        double chunk_song_end = g_time_ms;

        if (!g_is_paused && g_midi) {
            double chunk_time_ms = ((double)BUFFER_SIZE / SAMPLE_RATE) * 1000.0;
            double next_time_ms = g_time_ms + chunk_time_ms * g_speed_modifier;

            // Process and handle all MIDI events belonging to this time interval.
            while (g_current_msg && g_current_msg->time <= next_time_ms) {
                send_midi_event(*tsf_ptr, g_current_msg, true);
                g_current_msg = g_current_msg->next;
            }

            tsf_render_short(*tsf_ptr, sample_buffer, BUFFER_SIZE, 0);
            g_time_ms = next_time_ms;
            chunk_song_end = next_time_ms;

            // Pause and go back to start
            if (g_current_msg == NULL && tsf_active_voice_count(*tsf_ptr) == 0) {
                g_time_ms = 0.0;
                g_is_paused = true;
                g_current_msg = g_midi;
                reset_channels(*tsf_ptr);

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

    // 2. Create empty pointers for music data
    tsf* g_tsf = NULL;

#ifndef THREAD
	tml_message* g_midi = NULL;
    tml_message* current_msg = NULL;
#endif
    // 3. Check arguments at startup
    if (argc >= 2) {
        // If there is an argument, we always assume it is a MIDI file
        g_midi = skift_midi_fil(argv[1]);
        g_time_ms = 0.0;

#ifdef THREAD
		g_current_msg = g_midi;
#else
        current_msg = g_midi;
#endif
    }
    
    // If an sf2 file is specified as argument 2, use it; otherwise, use the built-in one.
    if (argc >= 3) {
        skift_soundfont(argv[2], &g_tsf, g_midi, g_midi);
    } else {
        skift_soundfont(NULL, &g_tsf, g_midi, g_midi); // Uses the embedded Nokia bank automatically
    }
    if (!g_tsf) {
        audio_close();
        tml_free(g_midi);
        return 1;
    }

    // 5. Initialize X11 Window
    Display* display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Error: Could not open X display\n");
        audio_close();
        tml_free(g_midi);
        tsf_close(g_tsf);
        return 1;
    }

    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 
                                        10, 10, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
                                        BlackPixel(display, screen), BlackPixel(display, screen));

    XSelectInput(display, window, ExposureMask | KeyPressMask);
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

    XFontStruct* font = XLoadQueryFont(display, "fixed");
    if(font) XSetFont(display, gc, font->fid);
#ifdef THREAD
	bool audio_thread_started = (pthread_create(&audio_tid, NULL, audio_thread, &g_tsf) == 0);
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
    while (running) {
        int target_channel = -1;
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            // Allows the game to be closed using the window's "X" button with the mouse.
			if (event.type == ClientMessage) {
				if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
					running = false;
				}
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
				else if (keysym == XK_t || keysym == XK_T) {
					g_node_mode = !g_node_mode; // Switch between Hero and Node view!
				}
				else if (keysym == XK_l || keysym == XK_L) {
#ifdef THREAD
					pthread_mutex_lock(&audio_mutex);
					g_is_paused = true; // Pause while selecting a file
					tsf_note_off_all(g_tsf);
					pthread_mutex_unlock(&audio_mutex);
#else
					g_is_paused = true; // Pause while selecting a file
                    tsf_note_off_all(g_tsf);
#endif
                     midi_path = x11_filechooser(NULL,
						"Midiero - The Interactive MIDI player",	//title
						"Select MIDI file for Midiero",	// header_text
						".mid, .midi",	//filter extension
						FC_MODE_FILE,	//select mode
						"[q/Esc]: Cancel [Enter]: Open [Click]: Select");			//help text / footer area
                    if (midi_path) {
                        // Load outside the lock; on failure the current song is kept
                        tml_message* ny_midi = skift_midi_fil(midi_path);
                        free(midi_path);

                        if (ny_midi) {
                            tml_message* gammel_midi = g_midi;
#ifdef THREAD
							pthread_mutex_lock(&audio_mutex);
#endif
							g_is_paused = true;
							reset_channels(g_tsf);
							for (int i = 0; i < 16; i++) g_channel_has_played[i] = false; // Reset track history
							g_midi = ny_midi;
#ifdef THREAD
							g_current_msg = g_midi;
#else
							current_msg = g_midi;
#endif
							g_time_ms = 0.0; // Rewind to the beginning
							clock_set_song_time(0.0); // audio still queued belongs to the new song
#ifdef THREAD
							pthread_mutex_unlock(&audio_mutex);
#endif
							tml_free(gammel_midi); // the audio thread no longer references it
                        }
                    }
				}
				else if (keysym == XK_f || keysym == XK_F) {
#ifdef THREAD
					pthread_mutex_lock(&audio_mutex);
					g_is_paused = true;
					tsf_note_off_all(g_tsf);
					pthread_mutex_unlock(&audio_mutex);
#else
                    g_is_paused = true;
                    tsf_note_off_all(g_tsf);
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
                        // Safe to read g_current_msg here: the song is paused.
#ifdef THREAD
                        skift_soundfont(soundfont_path, &g_tsf, g_midi, g_current_msg);
#else
                        skift_soundfont(soundfont_path, &g_tsf, g_midi, current_msg);
#endif
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
						tsf_channel_note_off_all(g_tsf, target_channel);
					}
					pthread_mutex_unlock(&audio_mutex);
				}
#else
				if (target_channel >= 0 && target_channel < 16) {
                    g_channels_enabled[target_channel] = !g_channels_enabled[target_channel];
                    if (!g_channels_enabled[target_channel]) {
						tsf_channel_note_off_all(g_tsf, target_channel);
                    }
                }
#endif                
            }
        }
#ifndef THREAD
        // --- AUDIO: top the device up to TARGET_QUEUE_MS in small chunks ---
        // Only writes while there is room, so the loop does not wait for the sound card
        // and can draw at TARGET_FPS.
        double chunk_time_ms = ((double)CHUNK_FRAMES / SAMPLE_RATE) * 1000.0;
        for (int n = 0; running && n < TARGET_QUEUE_FRAMES / CHUNK_FRAMES + 4; n++) {
            if (playback_queued_frames() >= TARGET_QUEUE_FRAMES) break;

            // Pause and go back to start
            if (g_midi != NULL && current_msg == NULL && tsf_active_voice_count(g_tsf) == 0) {
                g_time_ms = 0.0;               // Reset the time
                g_is_paused = true;            // Pause
                current_msg = g_midi;          // Rewind MIDI to the start
                reset_channels(g_tsf);         // Instruments/controllers back to the song's start state

                printf("The song is over. Rewinds to the start and pauses.\n");
            }

            double chunk_song_start = g_time_ms;
            if (!g_is_paused && g_midi) {
                // Advance the time based on the fixed audio chunk and the speed.
                double next_time_ms = g_time_ms + (chunk_time_ms * g_speed_modifier);

                // Process and handle all MIDI events belonging to this time interval.
                while (current_msg && current_msg->time <= next_time_ms) {
                    send_midi_event(g_tsf, current_msg, true);
                    current_msg = current_msg->next;
                }

                tsf_render_short(g_tsf, sample_buffer, CHUNK_FRAMES, 0);
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

            // The write had to wait: the device buffer is smaller than TARGET_QUEUE_MS
            // (or the driver under-reports its delay). Stop here so drawing is not held up.
            if (get_monotonic_ms() - write_start_ms > chunk_time_ms * 0.5) break;
        }
#endif

        // --- Song time of the sound being heard right now (both builds) ---
#ifdef THREAD
        pthread_mutex_lock(&audio_mutex);
#endif
        double graphic_time_ms = clock_song_time();
#ifdef THREAD
        pthread_mutex_unlock(&audio_mutex);
#endif
		// =========================================================================
        // 7. FULLY DYNAMIC & SCALABLE GRAPHICS (Xlib resizing supported)
        // =========================================================================
        
        // Get the current window dimensions in real time (if the user has resized the window)
        XWindowAttributes g_attrs;
        XGetWindowAttributes(display, window, &g_attrs);
        int W = g_attrs.width;
        int H = g_attrs.height;

		// If the window size has changed, free the old pixmap and create a new one
        // (This ensures that double-buffering works in full-screen mode)
        static int last_w = 0, last_h = 0;
        if (W != last_w || H != last_h) {
            XFreePixmap(display, pixmap);
            pixmap = XCreatePixmap(display, window, W, H, DefaultDepth(display, screen));
            last_w = W; last_h = H;
        }

        // Clear the screen (Pixmap)
        XSetForeground(display, gc, BlackPixel(display, screen));
        XFillRectangle(display, pixmap, gc, 0, 0, W, H);
        

        // Define the dynamic layout zones based on the current screen size.
        int top_h = 40;       // Height of top beam
        int bot_h = 60;       // Height of bottom beam
        int side_w = 240;     // Side panel width (Legend)
        int play_x_start = side_w; // The playing area starts where the side panel ends.
        int play_w = W - side_w;   // The rest of the screen is used for sheet music.
        
        // The red "NOW" line is now moved close to the left side of the playing area,
        // giving you the best possible view into the future (the right side)!
        int dynamic_center_x = play_x_start + (int)(play_w * 0.15); 
        int play_bottom_y = H - bot_h;

        // --- ZONE 1: ALWAYS VISIBLE HEADER (Top) ---
        XSetForeground(display, gc, 0x222222); 
        XFillRectangle(display, pixmap, gc, 0, 0, W, top_h);
        XSetForeground(display, gc, WhitePixel(display, screen));
        char header_str[600]; // room for both 256-char names

		snprintf(header_str, sizeof(header_str), "  Midiero :: File: %s :: Bank: %s :: Resolution: %dx%d", g_nuvaerende_midi_navn, g_nuvaerende_sf2_navn, W, H);
        
        XDrawString(display, pixmap, gc, 10, 24, header_str, strlen(header_str));
        XSetForeground(display, gc, 0x444444);
        XDrawLine(display, pixmap, gc, 0, top_h, W, top_h);

		// --- ZONE 2: INTELLIGENT PLAYING SURFACE (Now vertically scalable) ---
        int center_y = top_h + (play_bottom_y - top_h) / 2; // The vertical center is adjusted automatically.
        int linje_afstand = (H > 700) ? 14 : 10;            // Increase the line spacing if the screen is large!

        if (g_node_mode) {
            // Draw the staff lines (G and F clefs).
            XSetForeground(display, gc, 0x333333);
            int g_trin[] = {2, 4, 6, 8, 10};
            for (int l = 0; l < 5; l++) {
                int y_linje = center_y - (g_trin[l] * linje_afstand / 2);
                if (y_linje > top_h && y_linje < play_bottom_y) {
                    XDrawLine(display, pixmap, gc, play_x_start, y_linje, W, y_linje);
                }
            }
            int f_trin[] = {-12, -10, -8, -6, -4};
            for (int l = 0; l < 5; l++) {
                int y_linje = center_y - (f_trin[l] * linje_afstand / 2);
                if (y_linje > top_h && y_linje < play_bottom_y) {
                    XDrawLine(display, pixmap, gc, play_x_start, y_linje, W, y_linje);
                }
            }

            // Red "NOW" playhead
            XSetForeground(display, gc, 0xFF0000); 
            XDrawLine(display, pixmap, gc, dynamic_center_x, top_h, dynamic_center_x, play_bottom_y);

            // Define the dimensions of the node head first (so they are known for the calculation).
            int node_w = (H > 700) ? 18 : 14;
            int node_h = (H > 700) ? 10 : 8;

			// --- CALCULATE NODE DURATION (RUN ONLY IF BEAMS ARE ENABLED) ---
            int node_laengde_pixels = node_w; // Standard minimum width (head only)
            
			tml_message* scan = g_midi;
            while (scan) {
                if (scan->type == TML_NOTE_ON && scan->velocity > 0 && g_channels_enabled[scan->channel]) {		
                    int x_pos = dynamic_center_x + (int)((scan->time - graphic_time_ms) * 0.1);
                    int trin = hent_node_trin(scan->key);
                    int y_pos = center_y - (trin * linje_afstand / 2);

                    // --- CALCULATE THE DURATION OF THE NOTE (LENGTH OF THE BEAM) ---
                    node_laengde_pixels = 16; // Standard minimum width (head only)
				
					tml_message* lookahead = scan->next;
						
					if (g_show_bars) {
						lookahead = scan->next;
						while (lookahead) {
							// Stop the scanning if we find the switch-off message for the exact same channel and tone.
							if ((lookahead->type == TML_NOTE_OFF || (lookahead->type == TML_NOTE_ON && lookahead->velocity == 0))
										&& lookahead->channel == scan->channel 
										&& lookahead->key == scan->key) {
										
								double varighed_ms = lookahead->time - scan->time;
								node_laengde_pixels = (int)(varighed_ms * 0.1); // 1ms = 0.2px
								if (node_laengde_pixels < node_w) node_laengde_pixels = node_w;
									break;
								}
								lookahead = lookahead->next;
						}
					}

					// Calculate where the bar ends (if g_show_bars is false, x_end is simply equal to the end of the notehead).
					int x_ende = x_pos + node_laengde_pixels;

					// Draw only if the note or its beam is visible on the playing area.
					
					if (x_ende > play_x_start && x_pos < W && y_pos > top_h && y_pos < play_bottom_y) {
								unsigned long kanal_farve = hent_kanal_farve(scan->channel);
								XSetForeground(display, gc, kanal_farve);

						// 1. Draw the duration bar itself (ONLY if the toggle sequence is ON and it is actually longer than the head)
						if (g_show_bars && node_laengde_pixels > node_w) {
							// We draw a nice horizontal beam that starts at the notehead and extends to the right.
							XFillRectangle(display, pixmap, gc, x_pos + (node_w / 2), y_pos - 2, node_laengde_pixels - (node_w / 2), 4);
									
							// Draw a small vertical end line at the end of the beam.
							XDrawLine(display, pixmap, gc, x_ende, y_pos - 4, x_ende, y_pos + 4);
						}

						// 2. Draw the round notehead itself on top (Always do this so it stands out sharply in the foreground!)
						XFillArc(display, pixmap, gc, x_pos, y_pos - (node_h/2), node_w, node_h, 0, 360 * 64);
								
						// 3. Ledger lines and sharps (#)
						if (er_sort_tangent(scan->key)) {
							XDrawString(display, pixmap, gc, x_pos - 8, y_pos + 4, "#", 1);
						}
						if (scan->key == 60 || scan->key == 61) {
							XSetForeground(display, gc, 0x666666);
							XDrawLine(display, pixmap, gc, x_pos - 4, y_pos, x_pos + node_w + 4, y_pos);
						}
						if ((trin >= 12 || trin <= -14) && trin % 2 == 0) {
							XSetForeground(display, gc, 0x444444);
							XDrawLine(display, pixmap, gc, x_pos - 3, y_pos, x_pos + node_w + 3, y_pos);
						}
					}
					
				}
				scan = scan->next;
			}
		} else {
            // Hero mode
            XSetForeground(display, gc, 0x1A1A1A);
            int grid_step = (play_bottom_y - top_h) / 15;
            if (grid_step < 1) grid_step = 1; // window very small: avoid an endless loop
            for (int i = top_h + grid_step; i < play_bottom_y; i += grid_step) {
                XDrawLine(display, pixmap, gc, play_x_start, i, W, i);
            }

            XSetForeground(display, gc, 0xFF0000); 
            XDrawLine(display, pixmap, gc, dynamic_center_x, top_h, dynamic_center_x, play_bottom_y);

            tml_message* scan = g_midi;
            while (scan) {
                if (scan->type == TML_NOTE_ON && scan->velocity > 0 && g_channels_enabled[scan->channel]) {
                    int x_pos = dynamic_center_x + (int)((scan->time - graphic_time_ms) * 0.1);
                    
                    // Spreads the tones across the entire available height.
                    float pct = (float)(scan->key - 30) / 60.0f; // assume a range of 60 notes
                    if (pct < 0.0f) pct = 0.0f;
                    if (pct > 1.0f) pct = 1.0f;
                    int y_pos = play_bottom_y - (int)(pct * (play_bottom_y - top_h - 20)) - 10;

                    if (x_pos > play_x_start && x_pos < W && y_pos > top_h && y_pos < play_bottom_y) {
                        XSetForeground(display, gc, hent_kanal_farve(scan->channel));
                        XFillRectangle(display, pixmap, gc, x_pos, y_pos - 6, 16, 12);
                    }
                }
                scan = scan->next;
            }
        }

        // --- ZONE 3: ADLIB INSTRUMENT LEGEND ---
        XSetForeground(display, gc, 0x151515); 
        XFillRectangle(display, pixmap, gc, 0, top_h, side_w, play_bottom_y - top_h);
        XSetForeground(display, gc, 0x333333);
        XDrawLine(display, pixmap, gc, side_w, top_h, side_w, play_bottom_y); // Vertical dividing line

        XSetForeground(display, gc, 0x00FF00);
        XDrawString(display, pixmap, gc, 15, 60, "INSTRUMENT LEGEND:", 18);

        // Dynamic spacing in the list based on screen height, so it doesn't get cramped in full-screen mode.
        int y_spacing = (H - 100) / 17;
        if (y_spacing > 30) y_spacing = 30; // Maximum distance for the sake of appearance
        int y_offset = 85;

        for (int i = 0; i < 16; i++) {
            char legend_item[128];
            char key_hint[16];
            if (i < 9) snprintf(key_hint, sizeof(key_hint), "%d", i + 1);
            else if (i == 9) snprintf(key_hint, sizeof(key_hint), "0");
            else snprintf(key_hint, sizeof(key_hint), "S+%d", i - 9);

            if (!g_channel_has_played[i]) {
                XSetForeground(display, gc, 0x333333); 
                XFillRectangle(display, pixmap, gc, 15, y_offset - 8, 10, 8);
                XSetForeground(display, gc, 0x444444); 
                snprintf(legend_item, sizeof(legend_item), "[%s] Ch%02d: Not used", key_hint, i + 1);
                XDrawString(display, pixmap, gc, 35, y_offset, legend_item, strlen(legend_item));
            } 
            else if (!g_channels_enabled[i]) {
                XSetForeground(display, gc, 0x442222); 
                XFillRectangle(display, pixmap, gc, 15, y_offset - 8, 10, 8);
                // Name of the preset the synth really uses on this channel (bank-aware)
                const char* preset_name = tsf_get_presetname(g_tsf, g_channel_preset_index[i]);
                if (!preset_name) preset_name = (i == 9) ? "Standard Drum Kit" : "Acoustic Grand Piano";
                XSetForeground(display, gc, 0x777777); 
                snprintf(legend_item, sizeof(legend_item), "[%s] Ch%02d: [MUTED] %.10s", key_hint, i + 1, preset_name);
                XDrawString(display, pixmap, gc, 35, y_offset, legend_item, strlen(legend_item));
            }
            else {
                XSetForeground(display, gc, hent_kanal_farve(i));
                XFillRectangle(display, pixmap, gc, 15, y_offset - 8, 10, 8);
                
                // Name of the preset the synth really uses on this channel (bank-aware)
                const char* preset_name = tsf_get_presetname(g_tsf, g_channel_preset_index[i]);
                if (!preset_name) preset_name = (i == 9) ? "Standard Drum Kit" : "Acoustic Grand Piano";
                
                XSetForeground(display, gc, WhitePixel(display, screen)); 
                snprintf(legend_item, sizeof(legend_item), "[%s] Ch%02d: %.16s", key_hint, i + 1, preset_name);
                XDrawString(display, pixmap, gc, 35, y_offset, legend_item, strlen(legend_item));
			}
                y_offset += y_spacing;
		}	

                // --- ZONE 4: FOOTER (Slim bottom bar at the bottom of the screen) ---
                XSetForeground(display, gc, 0x111111);
                XFillRectangle(display, pixmap, gc, 0, play_bottom_y, W, bot_h);
                XSetForeground(display, gc, 0x444444);
                XDrawLine(display, pixmap, gc, 0, play_bottom_y, W, play_bottom_y);
                XSetForeground(display, gc, 0xBBBBBB);
                char help_str_fmt[128];
				snprintf(help_str_fmt, sizeof(help_str_fmt), "[Space]:Pause [L]:Load MIDI [F]:Load SF2 [T]:Toggle View [V]:Bars [Arrows Up/Down]:Speed (%.1fx) [Q]:Quit", g_speed_modifier);
              
                XDrawString(display, pixmap, gc, 20, H - 40, help_str_fmt, strlen(help_str_fmt));
                XSetForeground(display, gc, 0x00FF00);char status_str[128];

				if (!g_midi) {
					snprintf(status_str, sizeof(status_str), "STATUS: READY  |  Press [L] to load a MIDI file to start!");
				} else {
					snprintf(status_str, sizeof(status_str), "STATUS: %s  |  PLAYBACK TIME: %.2f s", g_is_paused ? "PAUSE  " : "PLAYING", graphic_time_ms / 1000.0);
				}
                XDrawString(display, pixmap, gc, 20, H - 20, status_str, strlen(status_str));
				// Copy the entire dynamic frame to the window				
				XCopyArea(display, pixmap, window, gc, 0, 0, W, H, 0, 0);
				XFlush(display);

				// Sleep until the next frame is due (absolute deadline, so drawing time
				// does not lower the frame rate). If far behind (e.g. after a file
				// dialog) start counting again from now instead of racing to catch up.
				next_frame_ms += FRAME_TIME_MS;
				double now_ms = get_monotonic_ms();
				if (next_frame_ms < now_ms - FRAME_TIME_MS) next_frame_ms = now_ms;
				if (next_frame_ms > now_ms) usleep((useconds_t)((next_frame_ms - now_ms) * 1000.0));
    }

    // 8. Clean up
#ifdef THREAD
    running = false;
	if (audio_thread_started) pthread_join(audio_tid, NULL);
	if (audio_error) fprintf(stderr, "Audio output stopped because of an error.\n");
#endif

    if (font) XFreeFont(display, font);
    XFreePixmap(display, pixmap);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    
    audio_close();
    tml_free(g_midi);
    tsf_close(g_tsf);
    return 0;
}
