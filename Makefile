CC := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres

TARGET := gw32time.exe
RES := src/gui/resources.o
SRC := \
	src/main.c \
	src/cli/cli.c \
	src/gui/gui.c \
	src/core/config_file.c \
	src/core/diagnostics.c \
	src/core/error.c \
	src/core/ntp_probe.c \
	src/core/privilege.c \
	src/core/registry.c \
	src/core/service.c \
	src/core/winver.c \
	src/core/w32time.c \
	src/core/w32tm.c

CFLAGS := -std=c99 -Wall -Wextra -Werror -Os -DUNICODE -D_UNICODE
LDFLAGS := -municode -ladvapi32 -lcomdlg32 -lws2_32

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(RES)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(RES) $(LDFLAGS)

$(RES): src/gui/resources.rc src/gui/resource.h
	$(WINDRES) -O coff -o $@ src/gui/resources.rc

clean:
	rm -f $(TARGET) $(RES)
