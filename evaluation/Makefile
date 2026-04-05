CC = cc
CFLAGS = -std=c11 -O2 -Wall
TARDY = ../src

TARDY_SRC = $(filter-out $(TARDY)/main.c, $(wildcard $(TARDY)/vm/*.c $(TARDY)/coordinate/*.c $(TARDY)/compiler/*.c $(TARDY)/ontology/*.c $(TARDY)/verify/*.c $(TARDY)/mcp/*.c))

all: laziness_bench scaling_bench ablation_bench hallucination_bench

laziness_bench: laziness_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@

scaling_bench: scaling_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@

ablation_bench: ablation_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@

hallucination_bench: hallucination_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@

run: all
	./laziness_bench
	./scaling_bench
	./ablation_bench
	./hallucination_bench

clean:
	rm -f laziness_bench scaling_bench ablation_bench hallucination_bench
