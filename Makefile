# unemployed.com – static server + page
#
#   make              build build/server
#   make test         server tests (C server, curl)
#   make test-page    page tests in a browser (Node + Playwright) against build/server
#   make sanitize     server tests on an ASan/UBSan build
#   make check        all of the above
#   make run          serve public/ on http://localhost:8080
#   make STATIC=1     fully static binary (used by the Dockerfile)

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Werror
HARDEN   = -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDHARDEN = -pie -Wl,-z,relro,-z,now
ifeq ($(STATIC),1)
LDHARDEN = -static-pie -Wl,-z,relro,-z,now
endif

BIN  = build/server
ASAN = build/server-asan
SRC  = src/server.c

.PHONY: all test test-server test-page sanitize check run clean

all: $(BIN)

$(BIN): $(SRC) | build
	$(CC) $(WARN) $(CFLAGS) $(HARDEN) -o $@ $(SRC) $(LDHARDEN) $(LDFLAGS)

$(ASAN): $(SRC) | build
	$(CC) $(WARN) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
		-fno-sanitize-recover=all -o $@ $(SRC)

build:
	mkdir -p build

test: test-server

test-server: $(BIN)
	bash tests/server_test.sh $(BIN)

test-page: $(BIN)
	node tests/page.test.js $(BIN)

sanitize: $(ASAN)
	bash tests/server_test.sh $(ASAN)

check: test-server sanitize test-page

run: $(BIN)
	PORT=8080 ROOT=public ./$(BIN)

clean:
	rm -rf build
