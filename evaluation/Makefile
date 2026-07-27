CC = cc
CFLAGS = -std=c11 -O2 -Wall
UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
  CFLAGS += -D_DEFAULT_SOURCE
endif
TARDY = ../src

TARDY_SRC = $(filter-out $(TARDY)/main.c, $(wildcard $(TARDY)/vm/*.c $(TARDY)/coordinate/*.c $(TARDY)/compiler/*.c $(TARDY)/ontology/*.c $(TARDY)/verify/*.c $(TARDY)/mcp/*.c))

all: laziness_bench scaling_bench ablation_bench hallucination_bench halueval_bench contradoc_bench vitaminc_bench agenthallu_bench

laziness_bench: laziness_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@ -lm

scaling_bench: scaling_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@ -lm

ablation_bench: ablation_bench.c
	$(CC) $(CFLAGS) -I$(TARDY) $< $(TARDY_SRC) -o $@ -lm

hallucination_bench: hallucination_bench.c hallucination_data.h baselines.h baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) hallucination_bench.c baselines.c $(TARDY_SRC) -o $@ -lm

halueval_bench: halueval_bench.c baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) halueval_bench.c baselines.c $(TARDY_SRC) -o $@ -lm

contradoc_bench: contradoc_bench.c baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) contradoc_bench.c baselines.c $(TARDY_SRC) -o $@ -lm

vitaminc_bench: vitaminc_bench.c baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) vitaminc_bench.c baselines.c $(TARDY_SRC) -o $@ -lm

agenthallu_bench: agenthallu_bench.c baselines.c
	$(CC) $(CFLAGS) -I$(TARDY) agenthallu_bench.c baselines.c $(TARDY_SRC) -o $@ -lm

run: all
	./laziness_bench
	./scaling_bench
	./ablation_bench
	./hallucination_bench
	./halueval_bench
	./contradoc_bench
	./vitaminc_bench

clean:
	rm -f laziness_bench scaling_bench ablation_bench hallucination_bench halueval_bench contradoc_bench vitaminc_bench agenthallu_bench
