# Makefile for CMOC + DECB disk image build

CC      = cmoc
DECB    = decb

TARGET  = fmfm
SECTOR_TARGET = sectwrite
DSK     = fmfm.dsk

SRCS    = fmfm.c sectwrite.c x-dskcon-standalone.c fm-dskcon-standalone.c
OBJS    = $(SRCS:.c=.o)
DRIVER_OBJS = x-dskcon-standalone.o fm-dskcon-standalone.o
PSRC    = $(SRCS:.c=.s)
LSTS    = $(SRCS:.c=.lst)
MAPS    = $(SRCS:.c=.map)
LINK    = $(SRCS:.c=.link)

all: $(DSK)

fmfm.o sectwrite.o: fm-dskcon-standalone.h x-dskcon-standalone.h
x-dskcon-standalone.o: x-dskcon-standalone.h
fm-dskcon-standalone.o: fm-dskcon-standalone.h

# Add CMOC options here if needed
CFLAGS  = -i

# Build executable
$(TARGET): fmfm.o $(DRIVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(SECTOR_TARGET): sectwrite.o $(DRIVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $<

# Build disk image and copy executable into it
$(DSK): $(TARGET) $(SECTOR_TARGET)
	$(DECB) dskini $(DSK)
	$(DECB) copy -2b $(TARGET) $(DSK),FMFM.BIN
	$(DECB) copy -2b $(SECTOR_TARGET) $(DSK),SECTWR.BIN

clean:
	rm -f $(OBJS) $(TARGET) $(DSK) $(PSRC) $(LSTS) $(MAPS) $(LINK) $(SECTOR_TARGET)

.PHONY: all clean
