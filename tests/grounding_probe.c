/*
 * Direct probe of the self-ontology grounding pipeline.
 *
 * Reproduces what the daemon's handle_run does, minus the daemon
 * machinery, so we can see exactly what facts exist in the Datalog
 * KB after loading wikidata_common.nt and what tardy_dl_query returns
 * for "Paris located_in France".
 *
 * Build:
 *   cc -O2 -Wall -I../src -o tests/grounding_probe tests/grounding_probe.c \
 *       ../src/vm/*.c ../src/ontology/*.c ../src/coordinate/bridge.c \
 *       ../src/verify/decompose.c ../src/verify/preprocess.c \
 *       ../src/verify/numeric.c ../src/verify/llm_decompose.c \
 *       ../src/verify/llm_ground.c ../src/verify/pipeline.c \
 *       ../src/compiler/*.c ../src/mcp/json.c ../src/mcp/server.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "vm/vm.h"
#include "vm/util.h"
#include "ontology/self.h"
#include "ontology/datalog.h"
#include "verify/pipeline.h"
#include "verify/decompose.h"

int main(void)
{
    /* Allocate VM */
    tardy_vm_t *vm = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
        PROT_READ | PROT_WRITE, TARDY_MAP_LAZY, -1, 0);
    if (vm == MAP_FAILED) { perror("mmap"); return 1; }
    tardy_vm_init(vm, NULL);

    /* Initialize self-ontology with backbone rules */
    tardy_self_ontology_t ont;
    if (tardy_self_ontology_init(&ont, vm) != 0) {
        fprintf(stderr, "ontology init failed\n");
        return 1;
    }

    fprintf(stderr, "[probe] backbone rules loaded: %d\n", ont.datalog.rule_count);
    fprintf(stderr, "[probe] backbone facts: %d\n", ont.datalog.fact_count);

    /* Load the wikidata file */
    int loaded = tardy_self_ontology_load_ttl(&ont, "tests/wikidata_common.nt");
    fprintf(stderr, "[probe] loaded %d triples from wikidata_common.nt\n", loaded);
    fprintf(stderr, "[probe] datalog facts after load: %d\n", ont.datalog.fact_count);
    fprintf(stderr, "[probe] datalog rules: %d\n", ont.datalog.rule_count);

    /* Force evaluation */
    int derived = tardy_dl_evaluate(&ont.datalog);
    fprintf(stderr, "[probe] tardy_dl_evaluate derived %d new facts\n", derived);
    fprintf(stderr, "[probe] datalog facts after evaluate: %d\n", ont.datalog.fact_count);

    /* Dump all facts that mention Paris or France */
    fprintf(stderr, "\n[probe] facts mentioning Paris or France:\n");
    for (int i = 0; i < ont.datalog.fact_count; i++) {
        const char *p = ont.datalog.facts[i].pred;
        const char *a = ont.datalog.facts[i].arg1;
        const char *b = ont.datalog.facts[i].arg2;
        if (strstr(a, "Paris") || strstr(a, "France") ||
            strstr(b, "Paris") || strstr(b, "France")) {
            fprintf(stderr, "    %s(%s, %s)\n", p, a, b);
        }
    }

    /* Now query exactly what handle_run would query */
    fprintf(stderr, "\n[probe] queries:\n");
    int q1 = tardy_dl_query(&ont.datalog, "located_in", "Paris", "France");
    fprintf(stderr, "    query located_in(Paris, France) = %d\n", q1);
    int q2 = tardy_dl_query(&ont.datalog, "locatedIn", "Paris", "France");
    fprintf(stderr, "    query locatedIn(Paris, France)  = %d\n", q2);
    int q3 = tardy_dl_query(&ont.datalog, "capitalOf", "Paris", "France");
    fprintf(stderr, "    query capitalOf(Paris, France)  = %d\n", q3);
    int q4 = tardy_dl_query(&ont.datalog, "located_in", "paris", "france");
    fprintf(stderr, "    query located_in(paris, france) = %d (lowercase)\n", q4);

    /* Now run the full grounding path */
    tardy_triple_t triples[1] = {0};
    strncpy(triples[0].subject, "Paris", sizeof(triples[0].subject)-1);
    strncpy(triples[0].predicate, "located_in", sizeof(triples[0].predicate)-1);
    strncpy(triples[0].object, "France", sizeof(triples[0].object)-1);

    tardy_grounding_t grounding;
    memset(&grounding, 0, sizeof(grounding));
    int g = tardy_self_ontology_ground(&ont, triples, 1, &grounding);
    fprintf(stderr, "\n[probe] tardy_self_ontology_ground returned %d\n", g);
    fprintf(stderr, "[probe] grounding.count = %d\n", grounding.count);
    fprintf(stderr, "[probe] grounded = %d, unknown = %d, contradicted = %d\n",
            grounding.grounded, grounding.unknown, grounding.contradicted);
    if (grounding.count > 0) {
        fprintf(stderr, "[probe] result[0] status=%d confidence=%.2f evidence=%d\n",
                grounding.results[0].status,
                (double)grounding.results[0].confidence,
                grounding.results[0].evidence_count);
    }

    /* Also: what does the decomposer produce for "Paris is in France"? */
    fprintf(stderr, "\n[probe] decomposing 'Paris is in France':\n");
    tardy_decomposition_t decomp;
    memset(&decomp, 0, sizeof(decomp));
    int n = tardy_decompose("Paris is in France", 18, decomp.triples, 8);
    fprintf(stderr, "    %d triples extracted:\n", n);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "        (%s, %s, %s)\n",
                decomp.triples[i].subject,
                decomp.triples[i].predicate,
                decomp.triples[i].object);
    }

    return 0;
}
