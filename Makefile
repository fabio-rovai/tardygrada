CC = cc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Isrc
# Linux needs _DEFAULT_SOURCE for clock_gettime + MAP_ANONYMOUS
UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
  CFLAGS += -D_DEFAULT_SOURCE -Wno-stringop-truncation -Wno-format-truncation
endif
LDFLAGS =

SRC = src/main.c \
      src/vm/memory.c \
      src/vm/context.c \
      src/vm/vm.c \
      src/vm/crypto.c \
      src/vm/message.c \
      src/vm/constitution.c \
      src/vm/heal.c \
      src/vm/persist.c \
      src/mcp/json.c \
      src/mcp/server.c \
      src/verify/pipeline.c \
      src/verify/decompose.c \
      src/ontology/bridge.c \
      src/compiler/lexer.c \
      src/compiler/compiler.c \
      src/compiler/exec.c \
      src/vm/semantic.c \
      src/compiler/terraform.c

OBJ = $(SRC:.c=.o)
BIN = tardygrada

.PHONY: all clean run size

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "Built: $@ ($$(wc -c < $@ | tr -d ' ') bytes)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN)

size: $(BIN)
	@echo "Binary size:"
	@ls -la $(BIN)
	@echo "\nSection sizes:"
	@size $(BIN)

bench: tardygrada
	$(CC) $(CFLAGS) $(LDFLAGS) -o bench tests/benchmark.c \
		src/vm/memory.c src/vm/context.c src/vm/vm.c src/vm/crypto.c \
		src/vm/message.c src/vm/constitution.c src/vm/heal.c src/vm/persist.c \
		src/vm/semantic.c src/verify/pipeline.c src/verify/decompose.c \
		src/mcp/json.c src/ontology/bridge.c
	./bench
	rm -f bench

clean:
	rm -f $(OBJ) $(BIN)
