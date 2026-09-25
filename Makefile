CC   = gcc

BINDIR = /usr/bin

TARGET_MACHINE := $(shell $(CC) -dumpmachine)

ifeq ($(findstring x86_64,$(TARGET_MACHINE)),x86_64)

ELF_FORMAT = elf64-x86-64
ELF_ARCH   = i386:x86-64

else ifneq ($(findstring i386,$(TARGET_MACHINE)),)
	
ELF_ARCH   = i386
STATIC = -static
CFLAGS += -fno-builtin-pow -fno-builtin-exp -DCLOCK_MONOTONIC=CLOCK_REALTIME

else ifneq ($(findstring i686,$(TARGET_MACHINE)),)

ELF_FORMAT = elf32-i386
ELF_ARCH   = i386

else

$(error Unsupported target: $(TARGET_MACHINE))

endif

SRCS = midiero.c audio/audio.c audio/pcm.c

OBJS= $(SRCS:.c=.o)

TARGET = midiero

CFLAGS += -Os -Wall -Wshadow -Wextra -Wno-deprecated-declarations \
			--std=gnu99 -ffunction-sections -fdata-sections \
			-I./audio -I./xfileselect -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA  -DTHREAD
			
LDFLAGS += $(STATIC) -Wl,--gc-sections,--sort-common,-s -lX11 -lm -lpthread

.PHONY: all clean install

all:  $(TARGET)

bank.h: Nokia_6230i_RM-72_.sf2
	xxd -i Nokia_6230i_RM-72_.sf2 bank.h

$(TARGET): bank.h $(OBJS) 
	$(CC) -o $@ $^ $(LDFLAGS) $(CFLAGS) 
	
install: $(TARGET2)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

clean:
	rm -f $(TARGET) $(OBJS) bank.h

