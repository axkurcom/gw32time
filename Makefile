CC := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres

TARGET := gw32time.exe
SRC := \
	src/main.c \
	src/cli/cli.c \
	src/core/error.c \
	src/core/privilege.c \
	src/core/registry.c \
	src/core/w32time.c \
	src/core/w32tm.c \
	src/core/service.c

CFLAGS := -std=c99 -Wall -Wextra -Werror -Os -DUNICODE -D_UNICODE
LDFLAGS := -municode -ladvapi32

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
