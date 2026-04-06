CC = cc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Isrc
# Linux needs _DEFAULT_SOURCE for clock_gettime + MAP_ANONYMOUS
UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
  CFLAGS += -D_DEFAULT_SOURCE -Wno-stringop-truncation -Wno-format-truncation -Wno-stringop-overflow
endif
LDFLAGS =

# Monocypher compiled separately (no -pedantic, it uses extensions)
MONOCYPHER_FLAGS = -Wall -O2 -std=c11 -Isrc
ifeq ($(UNAME),Linux)
  MONOCYPHER_FLAGS += -D_DEFAULT_SOURCE
endif

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
      src/verify/preprocess.c \
      src/verify/numeric.c \
      src/verify/llm_decompose.c \
      src/ontology/bridge.c \
      src/ontology/self.c \
      src/ontology/datalog.c \
      src/ontology/frames.c \
      src/ontology/inference.c \
      src/compiler/lexer.c \
      src/compiler/compiler.c \
      src/compiler/exec.c \
      src/vm/semantic.c \
      src/compiler/terraform.c \
      src/terraform/terraform.c \
      src/coordinate/bridge.c

OBJ = $(SRC:.c=.o) src/vm/monocypher.o
BIN = tardygrada

.PHONY: all clean run size bench

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "Built: $@ ($$(wc -c < $@ | tr -d ' ') bytes)"

src/vm/monocypher.o: src/vm/monocypher.c
	$(CC) $(MONOCYPHER_FLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN)

size: $(BIN)
	@echo "Binary size:"
	@ls -la $(BIN)
	@echo "\nSection sizes:"
	@size $(BIN)

bench: $(BIN)
	$(CC) $(CFLAGS) $(MONOCYPHER_FLAGS) $(LDFLAGS) -o bench tests/benchmark.c \
		src/vm/memory.c src/vm/context.c src/vm/vm.c src/vm/crypto.c \
		src/vm/monocypher.c src/vm/message.c src/vm/constitution.c \
		src/vm/heal.c src/vm/persist.c src/vm/semantic.c \
		src/verify/pipeline.c src/verify/decompose.c \
		src/mcp/json.c src/ontology/bridge.c
	./bench
	rm -f bench

clean:
	rm -f $(OBJ) $(BIN) src/vm/monocypher.o
