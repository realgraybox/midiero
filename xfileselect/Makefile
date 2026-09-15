CC   = gcc
BINDIR = /usr/bin

TARGET_MACHINE := $(shell $(CC) -dumpmachine)

ifeq ($(findstring x86_64,$(TARGET_MACHINE)),x86_64)
    ELF_FORMAT = elf64-x86-64
    ELF_ARCH   = i386:x86-64
else ifneq ($(findstring i386,$(TARGET_MACHINE)),)
    ELF_ARCH   = i386
    STATIC = -static
    CFLAGS += -fno-builtin-pow -fno-builtin-exp
else ifneq ($(findstring i686,$(TARGET_MACHINE)),)
    ELF_FORMAT = elf32-i386
    ELF_ARCH   = i386
else
    $(error Unsupported target: $(TARGET_MACHINE))
endif

SRCS  = xfileselect.c
SRCS1 = example_main.c

OBJS  = $(SRCS:.c=.o)
OBJS1 = $(SRCS1:.c=.o)

TARGET  = xfileselect
TARGET1 = example_main

CFLAGS += -Os -Wall -Wshadow -Wextra -Wno-deprecated-declarations -Wformat=2 \
          --std=gnu99 -ffunction-sections -fdata-sections
          

LDFLAGS += $(STATIC) -Wl,--gc-sections,--sort-common,-s -lX11 -lm

.PHONY: all clean install

all: $(TARGET) $(TARGET1)


$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(TARGET1): $(OBJS1)
	$(CC) -o $@ $^ $(LDFLAGS)

xfileselect.o: xfileselect.c
	$(CC) $(CFLAGS) -DSTANDALONE -c $< -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

clean:
	rm -f $(TARGET) $(OBJS) $(TARGET1) $(OBJS1)
