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

# Add CMOC options here if needed
CFLAGS  = -i

all: $(DSK)

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
