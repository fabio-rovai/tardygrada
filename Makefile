CC = cc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Isrc
# Hardening: stack protection, fortified libc, position-independent code.
# These add negligible overhead at -O2 and close common exploit classes.
HARDEN_CFLAGS  = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
CFLAGS += $(HARDEN_CFLAGS)

# Linux needs _DEFAULT_SOURCE for clock_gettime + MAP_ANONYMOUS
UNAME := $(shell uname)
HARDEN_LDFLAGS =
ifeq ($(UNAME),Linux)
  CFLAGS += -D_DEFAULT_SOURCE -Wno-stringop-truncation -Wno-format-truncation -Wno-stringop-overflow
  # Linux: PIE link + RELRO + immediate binding. (macOS clang makes
  # executables position-independent by default and warns on -pie, so
  # we keep these Linux-only.)
  HARDEN_LDFLAGS += -pie -Wl,-z,relro,-z,now
endif
LDFLAGS = $(HARDEN_LDFLAGS)

# Monocypher compiled separately (no -pedantic, it uses extensions).
# Apply the same hardening flags to keep the whole binary consistent.
MONOCYPHER_FLAGS = -Wall -O2 -std=c11 -Isrc $(HARDEN_CFLAGS)
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
      src/verify/llm_ground.c \
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
      src/coordinate/bridge.c \
      src/daemon.c \
      src/daemon_client.c \
      src/mcp_bridge.c \
      src/memory/palace.c

OBJ = $(SRC:.c=.o) src/vm/monocypher.o
BIN = tardygrada

.PHONY: all clean run size bench test dashboard tardy

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm
	@echo "Built: $@ ($$(wc -c < $@ | tr -d ' ') bytes)"
	@# The CLI's help text and conversational form is `tardy`. Provide a
	@# symlink so both names work without diverging.
	@ln -sf $(BIN) tardy
	@echo "Symlink: tardy -> $(BIN)"

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
		src/mcp/json.c src/ontology/bridge.c -lm
	./bench
	rm -f bench

# Smoke tests — fast assertions on the parts that already work.
# Builds main binary and evaluation binaries on demand, then runs
# tests/smoke.sh which asserts F1/timing thresholds and rename invariants.
test: unit-tests $(BIN)
	@$(MAKE) -C evaluation >/dev/null
	@./tests/smoke.sh

# Standalone unit tests for the verification maths. These pin the Layer 4
# aggregation (geometric mean + weakest-link floor; the arithmetic mean let
# one weak conjunct hide behind many strong ones) and the Beta-posterior rule
# confidence (repetition alone can never reach certainty; contradicting
# observations lower it).
unit-tests:
	@$(CC) $(CFLAGS) -o /tmp/tardy_test_layer4 tests/test_layer4_math.c src/verify/pipeline.c -lm
	@/tmp/tardy_test_layer4
	@$(CC) $(CFLAGS) -o /tmp/tardy_test_rulconf tests/test_rule_confidence.c src/ontology/inference.c -lm
	@/tmp/tardy_test_rulconf

.PHONY: unit-tests

# Live dashboard — d3 treemap of the bundled ontology + a verify panel
# that lights up the cells a claim grounds against. Requires the daemon
# to be running. Opens on http://127.0.0.1:8765 by default.
dashboard: $(BIN)
	@if [ ! -S /tmp/tardygrada.sock ]; then \
		echo "[!] Daemon not running. Start it first:"; \
		echo "    ./$(BIN) daemon start"; \
		exit 1; \
	fi
	@python3 dashboard/server.py

clean:
	rm -f $(OBJ) $(BIN) tardy src/vm/monocypher.o
