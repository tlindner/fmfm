# Makefile for CMOC + DECB disk image build

CC      = cmoc
DECB    = decb

TARGET  = fmfm
DSK     = fmfm.dsk

SRCS    = fmfm.c x-dskcon-standalone.c fm-dskcon-standalone.c
OBJS    = $(SRCS:.c=.o)

# Add CMOC options here if needed
CFLAGS  =

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
	rm -f $(OBJS) $(TARGET) $(DSK)

.PHONY: all clean
