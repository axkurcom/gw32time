CC := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres
STRIP := i686-w64-mingw32-strip

TARGET := gw32time.exe
RES := src/gui/resources.o
SRC := \
	src/main.c \
	src/cli/cli.c \
	src/cli/format.c \
	src/gui/gui.c \
	src/core/config_file.c \
	src/core/diagnostics.c \
	src/core/domain.c \
	src/core/error.c \
	src/core/ntp_probe.c \
	src/core/preset.c \
	src/core/privilege.c \
	src/core/registry.c \
	src/core/service.c \
	src/core/time_set.c \
	src/core/winver.c \
	src/core/w32time.c \
	src/core/w32tm.c

CFLAGS := -std=c99 -Wall -Wextra -Werror -Os -march=i686 -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 \
	-ffunction-sections -fdata-sections -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 \
	-DNTDDI_VERSION=0x05010000 -DUNICODE -D_UNICODE
LDFLAGS := -municode -Wl,--gc-sections -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 \
	-ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -lnetapi32 -lshell32 -lws2_32

.PHONY: all clean verify

all: $(TARGET)

$(TARGET): $(SRC) $(RES)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(RES) $(LDFLAGS)
	$(STRIP) --strip-all $@

$(RES): src/gui/resources.rc src/gui/resource.h
	$(WINDRES) -O coff -o $@ src/gui/resources.rc

clean:
	rm -f $(TARGET) $(RES)

verify: $(TARGET)
	i686-w64-mingw32-objdump -x $(TARGET) | grep -E "MajorSubsystemVersion|MinorSubsystemVersion"
	i686-w64-mingw32-size $(TARGET)
	if i686-w64-mingw32-objdump -d $(TARGET) | grep -Ei "\\b(xmm|ymm|mmx|movaps|movups|movdqa|movdqu|addps|mulps|sqrtps)\\b"; then exit 1; fi
