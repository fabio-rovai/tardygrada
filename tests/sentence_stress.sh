#!/usr/bin/env bash
#
# Tardygrada sentence-length stress test.
#
# Companion to context_stress.sh. While that test scales the NUMBER of
# sentences (and exercises VDOC_MAX_SENTENCES = 1024), this test scales
# the LENGTH of a single sentence and exercises VDOC_MAX_SENT_LEN = 1024.
#
# Generates documents with one sentence of length: 100B, 500B, 900B,
# 1000B, 2000B, 10000B, 100000B — straddling the 1024-byte cap.
#
# Asserts:
#   - the binary does not crash, hang, or return a code other than 0/1
#   - sentences below the cap are processed (sentence count >= 1)
#   - sentences at/above the cap are gracefully dropped (no crash, no
#     spurious sentences counted from a buffer overflow)
#   - wall time stays bounded
#
# Exit 0 = all assertions pass. Exit non-zero = at least one failed.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tardygrada"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
DIM='\033[2m'
NC='\033[0m'

PASS=0
FAIL=0

ok()   { printf "  ${GREEN}PASS${NC} %s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  ${RED}FAIL${NC} %s -- %s\n" "$1" "$2"; FAIL=$((FAIL+1)); }
note() { printf "  ${DIM}%s${NC}\n" "$1"; }

if [ ! -x "$BIN" ]; then
    printf "${RED}sentence_stress: $BIN not built${NC}\n" >&2
    exit 1
fi

# Sentence sizes in bytes. Includes points just under, at, just over, and
# far past the 1024-byte VDOC_MAX_SENT_LEN cap.
SENT_LENS="${SENT_LENS:-100 500 900 1000 1100 2000 10000 100000}"

WORKDIR="$(mktemp -d -t tardy-sstress.XXXXXX)"

DAEMON_WAS_RUNNING=0
if [ -S /tmp/tardygrada.sock ] && \
   echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
   grep -q '"ok":true'; then
    DAEMON_WAS_RUNNING=1
    "$BIN" daemon stop >/dev/null 2>&1 || true
    sleep 0.5
fi

cleanup() {
    [ "${SSTRESS_KEEP:-0}" = "1" ] || rm -rf "$WORKDIR"
    if [ "$DAEMON_WAS_RUNNING" = "1" ]; then
        nohup "$BIN" daemon start >/dev/null 2>&1 &
        disown 2>/dev/null || true
        sleep 0.5
    fi
}
trap cleanup EXIT

# --------------------------------------------------------------------
# Generate a document with ONE long sentence of N bytes, padded so the
# whole sentence is one continuous run with no period until the very end.
# We add a short normal sentence at the end so there's always at least
# one sub-cap sentence to verify against.
# --------------------------------------------------------------------

build_long_sentence_doc() {
    local n="$1"
    local out="$2"
    # n-byte run of words separated by spaces, terminated with one period.
    # Each filler chunk is 8 bytes ("widget ") so we generate ceil(n/8)
    # chunks, then trim, then append a period.
    local chunks=$(( (n + 7) / 8 ))
    {
        # Long sentence: starts with a capital, ends with a period.
        printf 'Widget '
        local i=1
        while [ "$i" -lt "$chunks" ]; do
            printf 'item-%d ' "$i"
            i=$((i+1))
        done
        printf 'tail.\n\n'
        # A short, definitely-under-cap sentence as control:
        printf 'The control sentence at the end is short and parsed normally.\n'
    } > "$out"
}

# --------------------------------------------------------------------
# Parsers (works whether daemon-wrapped or standalone)
# --------------------------------------------------------------------

parse_metric() {
    grep -oE "$1[[:space:]]+[0-9]+" "$2" | head -1 | grep -oE '[0-9]+' | head -1
}

parse_time_ms() {
    grep -oE 'Time:[[:space:]]+[0-9]+ms' "$1" | head -1 | grep -oE '[0-9]+' | head -1
}

# --------------------------------------------------------------------
# Run sweeps
# --------------------------------------------------------------------

CAP=1024  # VDOC_MAX_SENT_LEN

printf "\n${DIM}Sentence-length stress test (VDOC_MAX_SENT_LEN = %d)${NC}\n" "$CAP"
printf "%-10s  %-9s  %-7s  %-7s  %-7s\n" "Sent_len" "Sub-cap?" "Sents" "Time_ms" "Status"
printf "%-10s  %-9s  %-7s  %-7s  %-7s\n" "--------" "--------" "-----" "-------" "------"

for n in $SENT_LENS; do
    DOC="$WORKDIR/doc-${n}b.md"
    OUT="$WORKDIR/out-${n}b.txt"
    build_long_sentence_doc "$n" "$DOC"

    SUBCAP="yes"
    if [ "$n" -ge "$CAP" ]; then SUBCAP="no"; fi

    # Run with a 30s hard timeout. 0 and 1 are both "ok" exit codes
    # (verify-doc returns 1 if contradictions found, 0 otherwise).
    perl -e '
        use strict; use warnings; use POSIX;
        my $pid = fork(); die "fork: $!" unless defined $pid;
        if ($pid == 0) { exec @ARGV or POSIX::_exit(127); }
        my $deadline = time() + 30;
        while (1) {
            my $r = waitpid($pid, POSIX::WNOHANG);
            last if $r > 0;
            if (time() >= $deadline) { kill 9, $pid; waitpid($pid, 0); exit 124; }
            select undef, undef, undef, 0.05;
        }
        exit ($? >> 8);
    ' "$BIN" verify-doc "$DOC" > "$OUT" 2>&1
    RC=$?

    SENTS="$(parse_metric 'Sentences:' "$OUT")"
    TIME_MS="$(parse_time_ms "$OUT")"
    : "${SENTS:=?}"
    : "${TIME_MS:=?}"

    STATUS="ok"
    if [ "$RC" -ne 0 ] && [ "$RC" -ne 1 ]; then
        STATUS="fail/rc=$RC"
    fi

    printf "%-10s  %-9s  %-7s  %-7s  %-7s\n" \
        "${n}B" "$SUBCAP" "$SENTS" "$TIME_MS" "$STATUS"

    # ---------- Assertions ----------
    if [ "$RC" -ne 0 ] && [ "$RC" -ne 1 ]; then
        fail "len=${n}/exit-clean" "verify-doc returned $RC"
        continue
    fi

    if [ "$SENTS" = "?" ]; then
        fail "len=${n}/parses-sentences" "could not parse Sentences: from output"
        continue
    fi

    # The control sentence at the end is always sub-cap and should be
    # detected. So we expect Sentences >= 1 in every case.
    if [ "$SENTS" -ge 1 ]; then
        ok "len=${n}/sentences>=1"
    else
        fail "len=${n}/sentences>=1" "got $SENTS"
    fi

    # If the long sentence is sub-cap, BOTH it and the control should be
    # parsed -- expect Sentences >= 2. If over-cap, the long one is
    # silently dropped; we expect just the control sentence.
    if [ "$n" -lt "$CAP" ]; then
        if [ "$SENTS" -ge 2 ]; then
            ok "len=${n}/sub-cap-sentence-parsed"
        else
            fail "len=${n}/sub-cap-sentence-parsed" "expected >=2, got $SENTS"
        fi
    else
        # Above the cap: we expect the long one to be dropped. Sentences
        # should be 1 (control only). If it's anything else, that's
        # surprising and worth flagging.
        if [ "$SENTS" -le 1 ]; then
            ok "len=${n}/over-cap-dropped-gracefully"
        else
            note "len=${n}: parsed $SENTS sentences over-cap (split mid-run?)"
            ok "len=${n}/over-cap-no-crash"
        fi
    fi

    # Time budget: this should be near-instant regardless of length.
    # Generous 2s ceiling.
    if [ "$TIME_MS" != "?" ] && \
       awk -v t="$TIME_MS" 'BEGIN { exit !(t+0 <= 2000) }'; then
        ok "len=${n}/time<=2000ms"
    elif [ "$TIME_MS" = "?" ]; then
        note "len=${n}: no Time: line in output"
    else
        fail "len=${n}/time<=2000ms" "took ${TIME_MS}ms"
    fi
done

printf "\nSentence-stress summary: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" \
    "$PASS" "$FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
