CC = cc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Isrc
LDFLAGS =

SRC = src/main.c \
      src/vm/memory.c \
      src/vm/context.c \
      src/vm/vm.c \
      src/vm/crypto.c \
      src/vm/message.c \
      src/vm/constitution.c \
      src/vm/heal.c \
      src/mcp/json.c \
      src/mcp/server.c \
      src/verify/pipeline.c \
      src/verify/decompose.c \
      src/ontology/bridge.c \
      src/compiler/lexer.c \
      src/compiler/compiler.c \
      src/compiler/exec.c \
      src/vm/semantic.c

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

clean:
	rm -f $(OBJ) $(BIN)
