CC   = gcc

BINDIR = /usr/bin

TARGET_MACHINE := $(shell $(CC) -dumpmachine)

# i386 with SSE2 maths (CFLAGS="-msse2 -mfpmath=sse" in the environment) needs
# a 16-byte aligned stack: -mstackrealign on gcc 4.2 and later, else every
# frame kept 16-byte aligned (gcc 3.4 of uClibc toolchains). No cost on x87.
I386_CFLAGS = $(shell $(CC) -mstackrealign -E -x c /dev/null >/dev/null 2>&1 && echo -mstackrealign || echo -mpreferred-stack-boundary=4)

ifeq ($(findstring x86_64,$(TARGET_MACHINE)),x86_64)

ELF_FORMAT = elf64-x86-64
ELF_ARCH   = i386:x86-64

else ifneq ($(findstring i386,$(TARGET_MACHINE)),)
	
ELF_ARCH   = i386
STATIC = -static
CFLAGS += -DCLOCK_MONOTONIC=CLOCK_REALTIME
ARCH_CFLAGS = $(I386_CFLAGS)

else ifneq ($(findstring i686,$(TARGET_MACHINE)),)

ELF_FORMAT = elf32-i386
ELF_ARCH   = i386
ARCH_CFLAGS = $(I386_CFLAGS)

else

$(error Unsupported target: $(TARGET_MACHINE))

endif

SRCS = midiero.c mmidi/mmidi.c mmidi/mmmodel.c audio/audio.c audio/pcm.c

OBJS= $(SRCS:.c=.o)

TARGET = midiero

CFLAGS += -Os -Wall -Wshadow -Wextra -Wno-deprecated-declarations \
			--std=gnu99 -ffunction-sections -fdata-sections \
			-I./audio -I./xfileselect -I./mmidi -I./xbmfont -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA \
			-DMM_NO_MODELS -DMM_NO_OSS
# mmidi (mmidi/): .sf2 banks only, midiero has its own audio output. More
# options (-DMM_NO_REVERB, -DMM_NO_CHORUS, -DMAXVOICE=n): see mmidi/README.md.
			
# make DEBUG_TIMING=1 : the single-threaded build prints main-loop/audio timing to stderr
ifeq ($(DEBUG_TIMING),1)
CFLAGS += -DDEBUG_TIMING
endif

LDFLAGS += $(STATIC) -Wl,--gc-sections,--sort-common,-s -lX11 -lm

# make SMALL_FONTS=1 : only the 6x13 font (scaled on large screens), about 17 KB smaller
ifeq ($(SMALL_FONTS),1)
CFLAGS += -DMIDIERO_SMALL_FONTS
endif

THREAD ?= 0
ifeq ($(THREAD),1)
CFLAGS  += -DTHREAD
LDFLAGS += -lpthread
endif

# last of all: once CFLAGS is overridden, plain "CFLAGS +=" lines below it are ignored
override CFLAGS += $(ARCH_CFLAGS)

.PHONY: all clean install

all:  $(TARGET)

bank.h: Nokia_6230i_RM-72_.sf2
	xxd -i Nokia_6230i_RM-72_.sf2 bank.h

# midiero.c includes the generated bank.h (also correct with make -j)
midiero.o: bank.h

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(CFLAGS)
	
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

clean:
	rm -f $(TARGET) $(OBJS) bank.h

