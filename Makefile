CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
TARGET  = raster-tspl
SRC     = src/raster-tspl.c

# Detect CUPS library flags via cups-config if available
CUPS_CFLAGS  := $(shell cups-config --cflags  2>/dev/null || echo "")
CUPS_LDFLAGS := $(shell cups-config --libs    2>/dev/null || echo "-lcups")

# Older CUPS (< 1.7) keep image routines in a separate -lcupsimage
# Newer CUPS bundles them into -lcups; try linking without it first.
LDFLAGS = $(CUPS_LDFLAGS)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(CUPS_CFLAGS) -o $@ $< $(LDFLAGS) || \
	$(CC) $(CFLAGS) $(CUPS_CFLAGS) -o $@ $< $(LDFLAGS) -lcupsimage

clean:
	rm -f $(TARGET)
