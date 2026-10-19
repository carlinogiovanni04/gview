CC ?= clang
CFLAGS = -std=c11 -g -Wall -Wextra -Werror
INCLUDES = -Iinclude -I../gcui/include
SDL_CFLAGS ?= -I../_libraries_/include
SDL_LIBS ?= -lSDL3 -lSDL3_ttf

OUT = bin/gview
TEST_OUT = bin/gview_test
SRC = src/main.c

all: $(OUT)

$(OUT): $(SRC) include/gview.h ../gcui/include/gcui.h
	mkdir -p bin/
	$(CC) $(CFLAGS) $(INCLUDES) $(SDL_CFLAGS) $(SRC) -o $(OUT) $(SDL_LIBS)

$(TEST_OUT): tests/test_parse.c include/gview.h
	mkdir -p bin/
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_parse.c -o $(TEST_OUT)

test: $(TEST_OUT)
	./$(TEST_OUT)

summary: $(OUT)
	./$(OUT) --summary tests/fixtures/sample_trace.json

clean:
	rm -rf bin/
