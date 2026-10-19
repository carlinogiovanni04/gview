CC ?= clang
CFLAGS = -std=c11 -Wno-format-truncation -g -Wall -Wextra -Werror
INCLUDES = -Iinclude -I../gcui/include
SDL_CFLAGS ?= -I../_libraries_/include
SDL_LIBS ?= -lSDL3 -lSDL3_ttf

OUT = bin/gview
SRC = src/main.c

all: $(OUT)

$(OUT): $(SRC) include/gview.h ../gcui/include/gcui.h
	mkdir -p bin/
	$(CC) $(CFLAGS) $(INCLUDES) $(SDL_CFLAGS) $(SRC) -o $(OUT) $(SDL_LIBS)

clean:
	rm -rf bin/
