CC = cc
CFLAGS = -std=c11 -O2 -Wall
TARDY = ../src

TARDY_SRC = $(filter-out $(TARDY)/main.c, $(wildcard $(TARDY)/vm/*.c $(TARDY)/coordinate/*.c $(TARDY)/compiler/*.c $(TARDY)/ontology/*.c $(TARDY)/verify/*.c $(TARDY)/mcp/*.c))

all: laziness_bench scaling_bench ablation_bench hallucination_bench halueval_bench contradoc_bench vitaminc_bench

laziness_bench: laziness_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@

scaling_bench: scaling_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@

ablation_bench: ablation_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@

hallucination_bench: hallucination_bench.c hallucination_data.h baselines.h baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) hallucination_bench.c baselines.c $(TARDY_SRC) -o $@

halueval_bench: halueval_bench.c baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) halueval_bench.c baselines.c $(TARDY_SRC) -o $@

contradoc_bench: contradoc_bench.c baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) contradoc_bench.c baselines.c $(TARDY_SRC) -o $@

vitaminc_bench: vitaminc_bench.c baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) vitaminc_bench.c baselines.c $(TARDY_SRC) -o $@

run: all
	./laziness_bench
	./scaling_bench
	./ablation_bench
	./hallucination_bench
	./halueval_bench
	./contradoc_bench
	./vitaminc_bench

clean:
	rm -f laziness_bench scaling_bench ablation_bench hallucination_bench halueval_bench contradoc_bench vitaminc_bench
