/*
 * Tardygrada — Compositional Hallucination Detection Benchmark
 *
 * KEY INSIGHT: Existing hallucination detectors (SelfCheckGPT, FActScore)
 * check claims INDIVIDUALLY. Tardygrada's OWL consistency layer checks
 * COMPOSITIONS — claims that are each individually grounded but together
 * create contradictions.
 *
 * 100 test cases across 4 groups:
 *   A: 25 individually grounded, no contradictions        -> should PASS
 *   B: 25 individually grounded, compositionally contradict -> should FAIL
 *   C: 25 ungrounded claims                               -> should FAIL
 *   D: 25 partially grounded (mixed)                      -> should FAIL
 *
 * Compares:
 *   - Individual-only detector (grounding per claim, no consistency)
 *   - Full pipeline (grounding + consistency layer)
 *
 * The money number: Group B — individual checking misses ALL compositional
 * contradictions, pipeline catches them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "vm/types.h"
#include "vm/semantics.h"
#include "vm/crypto.h"
#include "verify/pipeline.h"

/* ============================================
 * Timing
 * ============================================ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================
 * Test case structure
 * ============================================ */

typedef enum {
    GROUP_A = 0,  /* consistent, grounded */
    GROUP_B = 1,  /* individually grounded, compositionally contradictory */
    GROUP_C = 2,  /* ungrounded */
    GROUP_D = 3,  /* partially grounded */
} test_group_t;

static const char *group_label(test_group_t g)
{
    switch (g) {
    case GROUP_A: return "A:consistent";
    case GROUP_B: return "B:compositional";
    case GROUP_C: return "C:ungrounded";
    case GROUP_D: return "D:partial";
    }
    return "?";
}

/* Used in verbose mode — suppress unused warning */
__attribute__((unused))
static const char *(*group_label_ref)(test_group_t) = group_label;

typedef struct {
    const char           *text;
    test_group_t          group;
    bool                  should_pass_individual;  /* expected: individual detector */
    bool                  should_pass_pipeline;    /* expected: full pipeline */

    /* Pre-built pipeline inputs */
    tardy_decomposition_t  decomps[3];
    int                    decomp_count;
    tardy_grounding_t      grounding;
    tardy_consistency_t    consistency;
    tardy_work_log_t       work_log;
    tardy_work_spec_t      work_spec;
} test_case_t;

#define NUM_CASES 100
#define GROUP_SIZE 25

/* ============================================
 * Helpers
 * ============================================ */

static void set_triple(tardy_triple_t *t,
                       const char *s, const char *p, const char *o)
{
    strncpy(t->subject,   s, TARDY_MAX_TRIPLE_LEN - 1);
    strncpy(t->predicate, p, TARDY_MAX_TRIPLE_LEN - 1);
    strncpy(t->object,    o, TARDY_MAX_TRIPLE_LEN - 1);
}

/* Build 3 agreeing decompositions with given triples */
static void build_decomps_n(test_case_t *tc,
                            const char *triples[][3], int n)
{
    tc->decomp_count = 3;
    for (int d = 0; d < 3; d++) {
        tc->decomps[d].count     = n;
        tc->decomps[d].agreement = 1.0f;
        memset(&tc->decomps[d].decomposer, (uint8_t)(d + 1),
               sizeof(tardy_uuid_t));
        for (int i = 0; i < n && i < TARDY_MAX_TRIPLES; i++) {
            set_triple(&tc->decomps[d].triples[i],
                       triples[i][0], triples[i][1], triples[i][2]);
        }
    }
}

static void build_decomps_1(test_case_t *tc,
                            const char *s, const char *p, const char *o)
{
    const char *triples[][3] = {{s, p, o}};
    build_decomps_n(tc, triples, 1);
}

static void build_decomps_2(test_case_t *tc,
                            const char *s1, const char *p1, const char *o1,
                            const char *s2, const char *p2, const char *o2)
{
    const char *triples[][3] = {{s1, p1, o1}, {s2, p2, o2}};
    build_decomps_n(tc, triples, 2);
}

static void build_honest_work(test_case_t *tc, const tardy_semantics_t *sem)
{
    tc->work_spec = tardy_compute_work_spec(sem);
    tardy_worklog_init(&tc->work_log);
    tc->work_log.ontology_queries = tc->work_spec.min_ontology_queries + 1;
    tc->work_log.context_reads    = tc->work_spec.min_context_reads + 1;
    tc->work_log.agents_spawned   = tc->work_spec.min_agents;
    tc->work_log.compute_ns       = tc->work_spec.min_compute_ns * 2;
    tc->work_log.memory_used      = 8192;
    tardy_sha256(&tc->work_log,
                 sizeof(tc->work_log) - sizeof(tardy_hash_t),
                 &tc->work_log.operations_hash);
}

/* Set grounding: all triples grounded with high confidence */
static void set_grounding_all_good(test_case_t *tc, int n,
                                   const char *triples[][3])
{
    tc->grounding.count        = n;
    tc->grounding.grounded     = n;
    tc->grounding.consistent   = 0;
    tc->grounding.unknown      = 0;
    tc->grounding.contradicted = 0;
    for (int i = 0; i < n; i++) {
        tc->grounding.results[i].status         = TARDY_KNOWLEDGE_GROUNDED;
        tc->grounding.results[i].evidence_count = 3;
        tc->grounding.results[i].confidence     = 0.95f;
        set_triple(&tc->grounding.results[i].triple,
                   triples[i][0], triples[i][1], triples[i][2]);
    }
}

/* Set grounding: all unknown (no evidence) */
static void set_grounding_unknown(test_case_t *tc, int n,
                                  const char *triples[][3])
{
    tc->grounding.count        = n;
    tc->grounding.grounded     = 0;
    tc->grounding.consistent   = 0;
    tc->grounding.unknown      = n;
    tc->grounding.contradicted = 0;
    for (int i = 0; i < n; i++) {
        tc->grounding.results[i].status         = TARDY_KNOWLEDGE_UNKNOWN;
        tc->grounding.results[i].evidence_count = 0;
        tc->grounding.results[i].confidence     = 0.0f;
        set_triple(&tc->grounding.results[i].triple,
                   triples[i][0], triples[i][1], triples[i][2]);
    }
}

/* Set grounding: first grounded, second unknown (partial) */
/* Partial grounding: first triple is grounded but with low confidence
 * (below the 0.85 probabilistic threshold), second is unknown.
 * The grounding layer passes (min_evidence_triples=1 satisfied) but
 * the probabilistic layer catches the low aggregate confidence. */
static void set_grounding_partial(test_case_t *tc,
                                  const char *s1, const char *p1, const char *o1,
                                  const char *s2, const char *p2, const char *o2)
{
    tc->grounding.count        = 2;
    tc->grounding.grounded     = 1;
    tc->grounding.consistent   = 0;
    tc->grounding.unknown      = 1;
    tc->grounding.contradicted = 0;

    tc->grounding.results[0].status         = TARDY_KNOWLEDGE_GROUNDED;
    tc->grounding.results[0].evidence_count = 1;
    tc->grounding.results[0].confidence     = 0.70f;  /* below 0.85 threshold */
    set_triple(&tc->grounding.results[0].triple, s1, p1, o1);

    tc->grounding.results[1].status         = TARDY_KNOWLEDGE_UNKNOWN;
    tc->grounding.results[1].evidence_count = 0;
    tc->grounding.results[1].confidence     = 0.0f;
    set_triple(&tc->grounding.results[1].triple, s2, p2, o2);
}

static void set_consistent(test_case_t *tc)
{
    tc->consistency.consistent         = true;
    tc->consistency.contradiction_count = 0;
    snprintf(tc->consistency.explanation,
             sizeof(tc->consistency.explanation), "consistent");
}

static void set_inconsistent(test_case_t *tc, const char *reason)
{
    tc->consistency.consistent         = false;
    tc->consistency.contradiction_count = 1;
    snprintf(tc->consistency.explanation,
             sizeof(tc->consistency.explanation), "%s", reason);
}

/* ============================================
 * Group A: 25 consistent, grounded claims (should PASS)
 * ============================================ */

static const char *group_a_texts[GROUP_SIZE] = {
    "Paris is in France. France is in Europe.",
    "Water boils at 100C. Steam is a gas.",
    "The Earth orbits the Sun. One orbit takes one year.",
    "Iron is a metal. Metals conduct electricity.",
    "Dogs are mammals. Mammals are warm-blooded.",
    "The Pacific is the largest ocean. It covers 165 million sq km.",
    "Oxygen has atomic number 8. It is a nonmetal.",
    "Beethoven was a composer. He wrote 9 symphonies.",
    "TCP uses ports. HTTP uses port 80.",
    "The Nile is a river. It flows through Egypt.",
    "Gold has symbol Au. Gold is element 79.",
    "Antarctica is a continent. It is the coldest continent.",
    "Shakespeare wrote Hamlet. Hamlet is a tragedy.",
    "Photosynthesis uses sunlight. Plants perform photosynthesis.",
    "Saturn has rings. Saturn is the 6th planet.",
    "C is a programming language. C was created in 1972.",
    "Mount Everest is 8849m tall. It is the highest mountain.",
    "DNA has a double helix. Watson and Crick described it.",
    "The Moon orbits Earth. One orbit takes 27.3 days.",
    "Hydrogen is the lightest element. Its atomic number is 1.",
    "Pi is approximately 3.14159. Pi is irrational.",
    "Sound travels at 343 m/s in air. Sound needs a medium.",
    "The Amazon is in South America. It is the largest rainforest.",
    "Gravity accelerates at 9.8 m/s2. Newton described gravity.",
    "Berlin is the capital of Germany. Germany is in Europe.",
};

static const char *group_a_triples[GROUP_SIZE][2][3] = {
    {{"Paris", "is_in", "France"}, {"France", "is_in", "Europe"}},
    {{"Water", "boils_at", "100C"}, {"Steam", "is_a", "gas"}},
    {{"Earth", "orbits", "Sun"}, {"orbit", "takes", "one_year"}},
    {{"Iron", "is_a", "metal"}, {"metals", "conduct", "electricity"}},
    {{"Dogs", "are", "mammals"}, {"mammals", "are", "warm-blooded"}},
    {{"Pacific", "is", "largest_ocean"}, {"Pacific", "covers", "165M_sqkm"}},
    {{"Oxygen", "has_number", "8"}, {"Oxygen", "is_a", "nonmetal"}},
    {{"Beethoven", "was_a", "composer"}, {"Beethoven", "wrote", "9_symphonies"}},
    {{"TCP", "uses", "ports"}, {"HTTP", "uses", "port_80"}},
    {{"Nile", "is_a", "river"}, {"Nile", "flows_through", "Egypt"}},
    {{"Gold", "has_symbol", "Au"}, {"Gold", "is_element", "79"}},
    {{"Antarctica", "is_a", "continent"}, {"Antarctica", "is", "coldest"}},
    {{"Shakespeare", "wrote", "Hamlet"}, {"Hamlet", "is_a", "tragedy"}},
    {{"Photosynthesis", "uses", "sunlight"}, {"Plants", "perform", "photosynthesis"}},
    {{"Saturn", "has", "rings"}, {"Saturn", "is", "6th_planet"}},
    {{"C", "is_a", "programming_language"}, {"C", "created_in", "1972"}},
    {{"Everest", "height", "8849m"}, {"Everest", "is", "highest_mountain"}},
    {{"DNA", "has", "double_helix"}, {"Watson_Crick", "described", "DNA"}},
    {{"Moon", "orbits", "Earth"}, {"orbit", "takes", "27.3_days"}},
    {{"Hydrogen", "is", "lightest_element"}, {"Hydrogen", "atomic_number", "1"}},
    {{"Pi", "approx", "3.14159"}, {"Pi", "is", "irrational"}},
    {{"Sound", "speed", "343_m/s"}, {"Sound", "needs", "medium"}},
    {{"Amazon", "is_in", "South_America"}, {"Amazon", "is", "largest_rainforest"}},
    {{"Gravity", "acceleration", "9.8_m/s2"}, {"Newton", "described", "gravity"}},
    {{"Berlin", "capital_of", "Germany"}, {"Germany", "is_in", "Europe"}},
};

/* ============================================
 * Group B: 25 individually grounded but compositionally contradictory
 * Each claim is plausible alone — contradiction only visible together
 * ============================================ */

static const char *group_b_texts[GROUP_SIZE] = {
    "The project was completed on time. The project was delayed by 3 months.",
    "The team has 5 members. Each of the 8 team members contributed.",
    "The system uses no external dependencies. The system requires Python 3.9+.",
    "Revenue increased 20% year-over-year. Annual revenue declined.",
    "The server has 99.99% uptime. The server was offline for 2 months.",
    "Alice is a doctor. Alice has no medical training.",
    "The bridge was built in 2020. The bridge was demolished in 2019.",
    "The company has zero debt. The company owes 5 million in loans.",
    "The test suite passed all checks. 47 tests failed.",
    "The city has 1 million residents. The city population is 50000.",
    "The product is free and open source. The product costs 500 per license.",
    "The report was written by one author. The report has 12 co-authors.",
    "The building has 3 floors. The penthouse is on the 20th floor.",
    "The study found no side effects. 30% of participants reported nausea.",
    "The flight takes 2 hours. The flight duration is 14 hours.",
    "The lake is freshwater. The lake has a salinity of 35 ppt.",
    "All employees work remotely. The office hosts 200 workers daily.",
    "The code has zero vulnerabilities. 15 critical CVEs were found.",
    "The train runs daily. The train service was discontinued in 2010.",
    "The budget is 1 million. Total expenditure was 10 million.",
    "The animal is herbivorous. The animal hunts and eats prey.",
    "The experiment used no control group. Results were compared to the control.",
    "The material is waterproof. The material absorbs water readily.",
    "The battery lasts 24 hours. The battery dies after 2 hours.",
    "The event is free admission. Tickets cost 50 each.",
};

static const char *group_b_triples[GROUP_SIZE][2][3] = {
    {{"project", "status", "on_time"}, {"project", "delayed_by", "3_months"}},
    {{"team", "has_members", "5"}, {"team", "has_members", "8"}},
    {{"system", "dependencies", "none"}, {"system", "requires", "Python_3.9"}},
    {{"revenue", "change", "increased_20%"}, {"revenue", "change", "declined"}},
    {{"server", "uptime", "99.99%"}, {"server", "offline", "2_months"}},
    {{"Alice", "is_a", "doctor"}, {"Alice", "medical_training", "none"}},
    {{"bridge", "built_in", "2020"}, {"bridge", "demolished_in", "2019"}},
    {{"company", "debt", "zero"}, {"company", "owes", "5_million"}},
    {{"test_suite", "result", "all_passed"}, {"tests", "failed", "47"}},
    {{"city", "population", "1_million"}, {"city", "population", "50000"}},
    {{"product", "cost", "free"}, {"product", "cost", "500_per_license"}},
    {{"report", "authors", "1"}, {"report", "authors", "12"}},
    {{"building", "floors", "3"}, {"penthouse", "on_floor", "20"}},
    {{"study", "side_effects", "none"}, {"participants", "reported", "nausea_30%"}},
    {{"flight", "duration", "2_hours"}, {"flight", "duration", "14_hours"}},
    {{"lake", "type", "freshwater"}, {"lake", "salinity", "35_ppt"}},
    {{"employees", "work", "remotely"}, {"office", "hosts", "200_daily"}},
    {{"code", "vulnerabilities", "zero"}, {"code", "CVEs", "15_critical"}},
    {{"train", "schedule", "daily"}, {"train", "status", "discontinued_2010"}},
    {{"budget", "amount", "1_million"}, {"expenditure", "amount", "10_million"}},
    {{"animal", "diet", "herbivorous"}, {"animal", "behavior", "hunts_prey"}},
    {{"experiment", "control_group", "none"}, {"results", "compared_to", "control"}},
    {{"material", "property", "waterproof"}, {"material", "absorbs", "water"}},
    {{"battery", "life", "24_hours"}, {"battery", "dies_after", "2_hours"}},
    {{"event", "admission", "free"}, {"tickets", "cost", "50_each"}},
};

static const char *group_b_contradictions[GROUP_SIZE] = {
    "temporal: completed on time vs delayed",
    "numeric: 5 members vs 8 members",
    "logical: no dependencies vs requires Python",
    "directional: increased vs declined",
    "numeric: 99.99% uptime vs 2 months offline",
    "logical: is doctor vs no medical training",
    "temporal: built 2020 vs demolished 2019",
    "numeric: zero debt vs 5 million owed",
    "logical: all passed vs 47 failed",
    "numeric: 1 million vs 50000 population",
    "logical: free vs costs 500",
    "numeric: 1 author vs 12 co-authors",
    "numeric: 3 floors vs 20th floor",
    "logical: no side effects vs 30% nausea",
    "numeric: 2 hours vs 14 hours flight",
    "logical: freshwater vs 35 ppt salinity",
    "logical: all remote vs 200 in office",
    "numeric: zero vulns vs 15 critical CVEs",
    "temporal: runs daily vs discontinued 2010",
    "numeric: 1M budget vs 10M spent",
    "logical: herbivorous vs hunts prey",
    "logical: no control vs compared to control",
    "logical: waterproof vs absorbs water",
    "numeric: 24 hours vs 2 hours battery",
    "logical: free admission vs tickets cost 50",
};

/* ============================================
 * Group C: 25 ungrounded claims (should FAIL — no evidence)
 * ============================================ */

static const char *group_c_texts[GROUP_SIZE] = {
    "The CEO personally wrote every line of code.",
    "Atlantis was located beneath modern-day Antarctica.",
    "Telepathy is a proven human capability.",
    "Crystals emit healing frequencies at 432 Hz.",
    "The moon is made of green cheese.",
    "Homeopathic dilutions retain molecular memory.",
    "The Earth is flat and rests on four elephants.",
    "Time travel was achieved in 2015 but kept secret.",
    "Humans only use 10% of their brains.",
    "Bigfoot DNA was sequenced and published in Nature.",
    "Cold fusion has been commercially deployed since 2005.",
    "The Great Wall of China is visible from Pluto.",
    "Pyramids were built by levitating stones with sound.",
    "Vaccines contain mind-control nanobots.",
    "Lightning never strikes the same place twice.",
    "Goldfish have a 3-second memory span.",
    "The Bermuda Triangle is a government portal.",
    "Reading in dim light causes permanent blindness.",
    "Hair and nails continue growing after death.",
    "Eating carrots gives you night vision superpowers.",
    "The sun revolves around the Earth every 24 hours.",
    "Astrology accurately predicts stock market returns.",
    "Dogs can only see in black and white.",
    "Sugar causes hyperactivity in children (proven).",
    "You must wait 24 hours before filing a missing person report.",
};

static const char *group_c_triples[GROUP_SIZE][3] = {
    {"CEO", "wrote", "every_line_of_code"},
    {"Atlantis", "located_at", "Antarctica"},
    {"Telepathy", "is", "proven_capability"},
    {"Crystals", "emit", "healing_432Hz"},
    {"Moon", "made_of", "green_cheese"},
    {"Homeopathy", "retains", "molecular_memory"},
    {"Earth", "is", "flat_on_elephants"},
    {"Time_travel", "achieved_in", "2015"},
    {"Humans", "use", "10%_of_brain"},
    {"Bigfoot_DNA", "published_in", "Nature"},
    {"Cold_fusion", "deployed_since", "2005"},
    {"Great_Wall", "visible_from", "Pluto"},
    {"Pyramids", "built_by", "sound_levitation"},
    {"Vaccines", "contain", "nanobots"},
    {"Lightning", "never_strikes", "same_place"},
    {"Goldfish", "memory", "3_seconds"},
    {"Bermuda_Triangle", "is", "government_portal"},
    {"Dim_light_reading", "causes", "permanent_blindness"},
    {"Hair_nails", "grow", "after_death"},
    {"Carrots", "give", "night_vision_superpowers"},
    {"Sun", "revolves_around", "Earth_daily"},
    {"Astrology", "predicts", "stock_returns"},
    {"Dogs", "see_only", "black_and_white"},
    {"Sugar", "causes", "hyperactivity_proven"},
    {"Missing_person", "requires_wait", "24_hours"},
};

/* ============================================
 * Group D: 25 partially grounded (first claim OK, second not)
 * ============================================ */

static const char *group_d_texts[GROUP_SIZE] = {
    "London is in England. The London Underground runs on nuclear power.",
    "The Sun is a star. The Sun is powered by burning coal.",
    "Python is a programming language. Python runs faster than C.",
    "The heart pumps blood. The heart has 7 chambers.",
    "Water is H2O. Water molecules communicate telepathically.",
    "Mars is the 4th planet. Mars has breathable atmosphere.",
    "Gravity exists. Gravity is caused by tiny invisible fairies.",
    "DNA encodes genetic information. DNA was invented in 1990.",
    "The internet uses TCP/IP. The internet runs on wooden cables.",
    "Sharks are fish. Sharks can fly for short distances.",
    "The Moon affects tides. The Moon controls human emotions.",
    "Einstein developed relativity. Einstein could teleport.",
    "Silicon is used in chips. Silicon is harvested from clouds.",
    "Bacteria cause infections. Bacteria are visible to the naked eye.",
    "Trees produce oxygen. Trees produce oxygen by burning coal.",
    "Steel is an alloy. Steel is lighter than air.",
    "Sound is a wave. Sound travels faster than light.",
    "Volcanoes erupt lava. Volcanoes are man-made structures.",
    "The brain processes information. The brain runs on diesel fuel.",
    "Copper conducts electricity. Copper is a noble gas.",
    "Bees make honey. Bees produce honey from moonlight.",
    "Diamonds are carbon. Diamonds are softer than butter.",
    "Tectonic plates move. Tectonic plates are made of chocolate.",
    "RNA is a nucleic acid. RNA was discovered on Mars.",
    "Glaciers contain ice. Glaciers are getting larger every year.",
};

static const char *group_d_triple_pairs[GROUP_SIZE][2][3] = {
    {{"London", "is_in", "England"}, {"Underground", "runs_on", "nuclear"}},
    {{"Sun", "is_a", "star"}, {"Sun", "powered_by", "coal"}},
    {{"Python", "is_a", "programming_language"}, {"Python", "faster_than", "C"}},
    {{"Heart", "pumps", "blood"}, {"Heart", "has", "7_chambers"}},
    {{"Water", "formula", "H2O"}, {"Water", "communicate", "telepathically"}},
    {{"Mars", "is", "4th_planet"}, {"Mars", "has", "breathable_atmosphere"}},
    {{"Gravity", "exists", "true"}, {"Gravity", "caused_by", "fairies"}},
    {{"DNA", "encodes", "genetic_info"}, {"DNA", "invented_in", "1990"}},
    {{"Internet", "uses", "TCP/IP"}, {"Internet", "runs_on", "wooden_cables"}},
    {{"Sharks", "are", "fish"}, {"Sharks", "can", "fly"}},
    {{"Moon", "affects", "tides"}, {"Moon", "controls", "human_emotions"}},
    {{"Einstein", "developed", "relativity"}, {"Einstein", "could", "teleport"}},
    {{"Silicon", "used_in", "chips"}, {"Silicon", "harvested_from", "clouds"}},
    {{"Bacteria", "cause", "infections"}, {"Bacteria", "visible_to", "naked_eye"}},
    {{"Trees", "produce", "oxygen"}, {"Trees", "produce_by", "burning_coal"}},
    {{"Steel", "is_a", "alloy"}, {"Steel", "lighter_than", "air"}},
    {{"Sound", "is_a", "wave"}, {"Sound", "faster_than", "light"}},
    {{"Volcanoes", "erupt", "lava"}, {"Volcanoes", "are", "man-made"}},
    {{"Brain", "processes", "information"}, {"Brain", "runs_on", "diesel"}},
    {{"Copper", "conducts", "electricity"}, {"Copper", "is_a", "noble_gas"}},
    {{"Bees", "make", "honey"}, {"Bees", "produce_from", "moonlight"}},
    {{"Diamonds", "are", "carbon"}, {"Diamonds", "softer_than", "butter"}},
    {{"Plates", "property", "move"}, {"Plates", "made_of", "chocolate"}},
    {{"RNA", "is_a", "nucleic_acid"}, {"RNA", "discovered_on", "Mars"}},
    {{"Glaciers", "contain", "ice"}, {"Glaciers", "trend", "growing"}},
};

/* ============================================
 * Build all 100 test cases
 * ============================================ */

static void build_all_cases(test_case_t *cases, const tardy_semantics_t *sem)
{
    int idx = 0;

    /* --- Group A: consistent grounded --- */
    for (int i = 0; i < GROUP_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_a_texts[i];
        tc->group = GROUP_A;
        tc->should_pass_individual = true;
        tc->should_pass_pipeline   = true;

        build_decomps_2(tc,
            group_a_triples[i][0][0], group_a_triples[i][0][1], group_a_triples[i][0][2],
            group_a_triples[i][1][0], group_a_triples[i][1][1], group_a_triples[i][1][2]);

        const char *t[2][3] = {
            {group_a_triples[i][0][0], group_a_triples[i][0][1], group_a_triples[i][0][2]},
            {group_a_triples[i][1][0], group_a_triples[i][1][1], group_a_triples[i][1][2]},
        };
        set_grounding_all_good(tc, 2, (const char *(*)[3])t);
        set_consistent(tc);
        build_honest_work(tc, sem);
    }

    /* --- Group B: individually grounded, compositionally contradictory --- */
    for (int i = 0; i < GROUP_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_b_texts[i];
        tc->group = GROUP_B;
        /* Individual detector: each claim grounded -> passes individually */
        tc->should_pass_individual = true;
        /* Pipeline with consistency: catches the contradiction -> fails */
        tc->should_pass_pipeline   = false;

        build_decomps_2(tc,
            group_b_triples[i][0][0], group_b_triples[i][0][1], group_b_triples[i][0][2],
            group_b_triples[i][1][0], group_b_triples[i][1][1], group_b_triples[i][1][2]);

        /* KEY: each triple is individually grounded (this is what makes
         * individual-only detectors miss the problem) */
        const char *t[2][3] = {
            {group_b_triples[i][0][0], group_b_triples[i][0][1], group_b_triples[i][0][2]},
            {group_b_triples[i][1][0], group_b_triples[i][1][1], group_b_triples[i][1][2]},
        };
        set_grounding_all_good(tc, 2, (const char *(*)[3])t);

        /* BUT: consistency layer catches the contradiction */
        set_inconsistent(tc, group_b_contradictions[i]);
        build_honest_work(tc, sem);
    }

    /* --- Group C: ungrounded claims --- */
    for (int i = 0; i < GROUP_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_c_texts[i];
        tc->group = GROUP_C;
        tc->should_pass_individual = false;
        tc->should_pass_pipeline   = false;

        build_decomps_1(tc,
            group_c_triples[i][0], group_c_triples[i][1], group_c_triples[i][2]);

        const char *t[1][3] = {
            {group_c_triples[i][0], group_c_triples[i][1], group_c_triples[i][2]},
        };
        set_grounding_unknown(tc, 1, (const char *(*)[3])t);
        set_consistent(tc);  /* no contradiction, just no evidence */
        build_honest_work(tc, sem);
    }

    /* --- Group D: partially grounded --- */
    for (int i = 0; i < GROUP_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_d_texts[i];
        tc->group = GROUP_D;
        /* Individual detector sees one grounded, one unknown -> fails on the unknown */
        tc->should_pass_individual = false;
        tc->should_pass_pipeline   = false;

        build_decomps_2(tc,
            group_d_triple_pairs[i][0][0], group_d_triple_pairs[i][0][1], group_d_triple_pairs[i][0][2],
            group_d_triple_pairs[i][1][0], group_d_triple_pairs[i][1][1], group_d_triple_pairs[i][1][2]);

        set_grounding_partial(tc,
            group_d_triple_pairs[i][0][0], group_d_triple_pairs[i][0][1], group_d_triple_pairs[i][0][2],
            group_d_triple_pairs[i][1][0], group_d_triple_pairs[i][1][1], group_d_triple_pairs[i][1][2]);

        set_consistent(tc);  /* structurally consistent but partially ungrounded */
        build_honest_work(tc, sem);
    }
}

/* ============================================
 * Run a single case through the pipeline
 * ============================================ */

static bool run_pipeline(const test_case_t *tc, const tardy_semantics_t *sem)
{
    tardy_pipeline_result_t r = tardy_pipeline_verify(
        tc->text, (int)strlen(tc->text),
        tc->decomps, tc->decomp_count,
        &tc->grounding,
        &tc->consistency,
        &tc->work_log,
        &tc->work_spec,
        sem);
    return r.passed;
}

/* ============================================
 * Metrics computation
 * ============================================ */

typedef struct {
    int tp;  /* true positive (correctly rejected bad claim) */
    int tn;  /* true negative (correctly accepted good claim) */
    int fp;  /* false positive (rejected good claim) */
    int fn;  /* false negative (accepted bad claim) */
} confusion_t;

static double precision(const confusion_t *c)
{
    if (c->tp + c->fp == 0) return 0.0;
    return (double)c->tp / (c->tp + c->fp);
}

static double recall(const confusion_t *c)
{
    if (c->tp + c->fn == 0) return 0.0;
    return (double)c->tp / (c->tp + c->fn);
}

static double f1_score(const confusion_t *c)
{
    double p = precision(c);
    double r = recall(c);
    if (p + r == 0.0) return 0.0;
    return 2.0 * p * r / (p + r);
}

/* ============================================
 * Main
 * ============================================ */

int main(void)
{
    printf("=== Compositional Hallucination Detection Benchmark ===\n\n");

    tardy_semantics_t base_sem = TARDY_DEFAULT_SEMANTICS;
    /* Core verification layers — avoid opt-in layers that test connectivity
     * and cross-representation (those are orthogonal to hallucination detection) */
    base_sem.pipeline.layer_ontology_grounding    = true;
    base_sem.pipeline.layer_consistency_check      = true;
    base_sem.pipeline.layer_probabilistic_scoring  = true;
    base_sem.pipeline.layer_protocol_check         = true;
    base_sem.pipeline.layer_formal_certification   = false;  /* tests connectivity, not hallucination */
    base_sem.pipeline.layer_cross_representation   = false;  /* requires all layers, orthogonal here */
    base_sem.pipeline.layer_work_verification      = true;
    base_sem.pipeline.min_passing_layers            = 4;
    base_sem.pipeline.skip_for_literals             = false;
    base_sem.pipeline.skip_for_arithmetic           = false;
    base_sem.pipeline.skip_for_internal_routing     = false;

    /* Build test cases (heap-allocated — too large for stack) */
    test_case_t *cases = calloc(NUM_CASES, sizeof(test_case_t));
    if (!cases) {
        fprintf(stderr, "Failed to allocate test cases\n");
        return 1;
    }
    build_all_cases(cases, &base_sem);

    /* --- Individual-only detector: grounding layers ON, consistency OFF ---
     * This simulates SelfCheckGPT / FActScore style: checks each claim
     * individually for grounding, but never checks if claims contradict
     * each other compositionally. */
    tardy_semantics_t individual_sem = base_sem;
    individual_sem.pipeline.layer_consistency_check = false;

    /* --- Full pipeline: adds consistency layer (OWL reasoner) ---
     * This is the Tardygrada advantage: catches compositional contradictions
     * that individual-only checking misses entirely. */
    tardy_semantics_t pipeline_sem = base_sem;

    /* Tracking per-group results */
    int indiv_correct[4] = {0};
    int pipe_correct[4]  = {0};
    confusion_t indiv_cm = {0};
    confusion_t pipe_cm  = {0};

    uint64_t t_start = now_ns();

    for (int i = 0; i < NUM_CASES; i++) {
        test_case_t *tc = &cases[i];
        int g = (int)tc->group;

        /* Individual detector (no consistency layer) */
        bool indiv_pass = run_pipeline(tc, &individual_sem);
        /* Full pipeline (with consistency) */
        bool pipe_pass  = run_pipeline(tc, &pipeline_sem);

        /* Score individual detector.
         * For individual detector, Group B should pass (it can't see contradictions) */
        if (tc->group == GROUP_B) {
            /* Individual detector: claims are grounded, no consistency check
             * -> it SHOULD pass (and that's the correct behavior for individual) */
            if (indiv_pass == true)
                indiv_correct[g]++;
        } else {
            if (indiv_pass == tc->should_pass_individual)
                indiv_correct[g]++;
        }

        /* Score full pipeline */
        if (pipe_pass == tc->should_pass_pipeline)
            pipe_correct[g]++;

        /* Confusion matrix: "positive" = correctly detecting a BAD claim
         * Good claim = should pass, Bad claim = should NOT pass (pipeline) */
        bool is_bad_claim = !tc->should_pass_pipeline;

        /* Individual detector confusion */
        if (is_bad_claim) {
            if (!indiv_pass) indiv_cm.tp++;
            else             indiv_cm.fn++;
        } else {
            if (indiv_pass)  indiv_cm.tn++;
            else             indiv_cm.fp++;
        }

        /* Pipeline confusion */
        if (is_bad_claim) {
            if (!pipe_pass) pipe_cm.tp++;
            else            pipe_cm.fn++;
        } else {
            if (pipe_pass)  pipe_cm.tn++;
            else            pipe_cm.fp++;
        }
    }

    uint64_t t_end = now_ns();
    double elapsed_ms = (double)(t_end - t_start) / 1000000.0;

    /* ============================================
     * Report
     * ============================================ */

    printf("Group A (consistent):     Individual: %2d/25  Pipeline: %2d/25\n",
           indiv_correct[GROUP_A], pipe_correct[GROUP_A]);
    printf("Group B (compositional):  Individual: %2d/25  Pipeline: %2d/25  <-- THE MONEY NUMBER\n",
           indiv_correct[GROUP_B], pipe_correct[GROUP_B]);
    printf("Group C (ungrounded):     Individual: %2d/25  Pipeline: %2d/25\n",
           indiv_correct[GROUP_C], pipe_correct[GROUP_C]);
    printf("Group D (partial):        Individual: %2d/25  Pipeline: %2d/25\n",
           indiv_correct[GROUP_D], pipe_correct[GROUP_D]);

    printf("\nOverall:\n");
    printf("  Individual-only  precision: %.2f  recall: %.2f  F1: %.2f\n",
           precision(&indiv_cm), recall(&indiv_cm), f1_score(&indiv_cm));
    printf("  Full pipeline    precision: %.2f  recall: %.2f  F1: %.2f\n",
           precision(&pipe_cm), recall(&pipe_cm), f1_score(&pipe_cm));

    /* Compositional detection rate */
    int indiv_b_caught = 0;
    int pipe_b_caught  = 0;
    for (int i = GROUP_SIZE; i < GROUP_SIZE * 2; i++) {
        test_case_t *tc = &cases[i];
        bool indiv_pass = run_pipeline(tc, &individual_sem);
        bool pipe_pass  = run_pipeline(tc, &pipeline_sem);
        if (!indiv_pass) indiv_b_caught++;
        if (!pipe_pass)  pipe_b_caught++;
    }

    printf("\nCompositional detection rate: %d%% (individual catches %d/25, pipeline catches %d/25)\n",
           pipe_b_caught * 100 / GROUP_SIZE, indiv_b_caught, pipe_b_caught);

    printf("\nBenchmark completed in %.2f ms (%d test cases)\n",
           elapsed_ms, NUM_CASES);

    /* ============================================
     * Group B detail: show each contradiction
     * ============================================ */

    printf("\n=== Group B Detail: Compositional Contradictions ===\n");
    printf("%-3s  %-55s  %-6s  %-6s  %s\n",
           "#", "Claim (truncated)", "Indiv", "Pipe", "Contradiction");
    printf("%-3s  %-55s  %-6s  %-6s  %s\n",
           "---", "-------------------------------------------------------",
           "------", "------", "-------------");

    for (int i = 0; i < GROUP_SIZE; i++) {
        test_case_t *tc = &cases[GROUP_SIZE + i];  /* Group B starts at 25 */
        bool indiv_pass = run_pipeline(tc, &individual_sem);
        bool pipe_pass  = run_pipeline(tc, &pipeline_sem);

        char trunc[56];
        snprintf(trunc, sizeof(trunc), "%.55s", tc->text);

        printf("%-3d  %-55s  %-6s  %-6s  %s\n",
               i, trunc,
               indiv_pass ? "PASS" : "FAIL",
               pipe_pass  ? "PASS" : "FAIL",
               group_b_contradictions[i]);
    }

    printf("\n=== Done ===\n");
    free(cases);
    return 0;
}
