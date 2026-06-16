# Makefile for CMOC + DECB disk image build

CC      = cmoc
DECB    = decb

TARGET  = fmfm
DSK     = fmfm.dsk

SRCS    = fmfm.c x-dskcon-standalone.c fm-dskcon-standalone.c
OBJS    = $(SRCS:.c=.o)
PSRC    = $(SRCS:.c=.s)
LSTS    = $(SRCS:.c=.lst)
MAPS    = $(SRCS:.c=.map)
LINK    = $(SRCS:.c=.link)

all: $(DSK)

fmfm.o: fm-dskcon-standalone.h x-dskcon-standalone.h
x-dskcon-standalone.o: x-dskcon-standalone.h
fm-dskcon-standalone.o: fm-dskcon-standalone.h

# Add CMOC options here if needed
CFLAGS  = -i

# Build executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $<

# Build disk image and copy executable into it
$(DSK): $(TARGET)
	$(DECB) dskini $(DSK)
	$(DECB) copy -2b $(TARGET) $(DSK),FMFM.BIN

clean:
	rm -f $(OBJS) $(TARGET) $(DSK) $(PSRC) $(LSTS) $(MAPS) $(LINK)

.PHONY: all clean
