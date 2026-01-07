# Chrome Developer Launcher - MinGW Cross-Compilation Makefile
# Compile on Linux for Windows

CC = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
CFLAGS = -Wall -O2 -mwindows -DUNICODE -D_UNICODE
LDFLAGS = -liphlpapi -lole32 -lshell32 -lwininet -ladvapi32 -lcomdlg32 -lws2_32 -lgdi32

RELEASE_DIR = release
TARGET = $(RELEASE_DIR)/ChromeDevLauncher.exe
SRC = ChromeDevLauncher.c
RC = ChromeDevLauncher.rc
RES_OBJ = ChromeDevLauncher_res.o

.PHONY: all clean

all: $(TARGET)

$(RELEASE_DIR):
	mkdir -p $(RELEASE_DIR)

# Compile resource file
$(RES_OBJ): $(RC) ChromeDevLauncher.ico
	$(WINDRES) $< -o $@

# Link final executable
$(TARGET): $(SRC) $(RES_OBJ) | $(RELEASE_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

clean:
	rm -rf $(RELEASE_DIR) *.o
