#!/bin/bash
# ============================================================================
# Measure what optimization buys:  ./benchmarks/measure.sh [program.cm]
# ----------------------------------------------------------------------------
# Compiles one program twice and reports two different numbers:
#
#   CODE SIZE   TAC instructions (from the compiler's own report) and MIPS
#               instructions in the .s file.  Each instruction counted ONCE.
#   WORK DONE   MIPS instructions SPIM actually executes.  An instruction
#               inside a loop counts once per iteration.
#
# Baseline:  the same compiler with every optimization off (-O0 -nopeep).
# Optimized: the default build (TAC optimizer + peephole).
#
# SPIM 8 has no statistics option, so instructions are counted with its
# `step` command, which prints one line per executed instruction.  SPIM only
# reads its commands one line at a time from a terminal, so it runs under
# `script` to give it one.  The count includes SPIM's own startup code
# (__start in exceptions.s), which is identical in both builds; it is
# measured separately and subtracted.
#
# Every measurement is repeated three times and the spread is reported.
# Instruction counts come from a simulator, so the spread should be zero: a
# deterministic metric is the point of using it instead of wall-clock time.
# ============================================================================
cd "$(dirname "$0")/.." || exit 1
SRC=${1:-benchmarks/hot_loop.cm}
RUNS=3
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

command -v script >/dev/null || { echo "needs 'script' (util-linux)"; exit 1; }
[ -x ./minicompiler ] || make -s >/dev/null || exit 1

# Number of instructions SPIM executes for one .s file.  Each executed
# instruction prints as "[0xADDRESS] ...".  The first one shares a line with
# SPIM's "(spim)" prompt, so occurrences are counted rather than lines.
executed() {
    { echo "load \"$1\""; echo "step 100000000"; echo "exit"; } |
        script -qfc spim /dev/null 2>/dev/null | tr -d '\r' |
        grep -o '\[0x[0-9a-f]*\]' | wc -l
}

# SPIM's startup code: executed by every program, so measure it once
cat > "$TMP/empty.s" <<'EOF'
.text
.globl main
main:
    li   $v0, 10
    syscall
EOF
STARTUP=$(( $(executed "$TMP/empty.s") - 2 ))

measure() {   # measure LABEL FLAGS...
    local label=$1; shift
    local report
    report=$(./minicompiler "$SRC" "$TMP/$label.s" -q "$@") || { echo "compile failed"; exit 1; }
    TAC[$label]=$(echo "$report" | sed -n 's/.*TAC instructions *: *[0-9]* -> \([0-9]*\).*/\1/p')
    MIPS[$label]=$(echo "$report" | sed -n 's/.*peephole (MIPS) *: *[0-9]* -> \([0-9]*\).*/\1/p')
    OUT[$label]=$(spim -file "$TMP/$label.s" | grep -v -e '^SPIM' -e '^Copyright' \
                  -e '^All Rights' -e '^See the' -e '^Loaded' | tr '\n' ' ')
    local runs=""
    for r in $(seq $RUNS); do runs="$runs $(( $(executed "$TMP/$label.s") - STARTUP ))"; done
    RUNS_OF[$label]=$runs
    EXEC[$label]=$(echo $runs | tr ' ' '\n' | sort -n | head -1)
    SPREAD[$label]=$(( $(echo $runs | tr ' ' '\n' | sort -n | tail -1) - EXEC[$label] ))
}

declare -A TAC MIPS EXEC SPREAD OUT RUNS_OF
measure unoptimized -O0 -nopeep
measure optimized

pct() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f%%", (a - b) * 100 / a }'; }

echo "Benchmark : $SRC"
echo "Baseline  : -O0 -nopeep (no TAC optimizer, no peephole)"
echo "Output    : unoptimized [${OUT[unoptimized]}]  optimized [${OUT[optimized]}]"
echo "SPIM startup instructions subtracted: $STARTUP"
echo
printf "%-36s %12s %12s %10s\n" "Metric" "Unoptimized" "Optimized" "Reduction"
printf "%-36s %12s %12s %10s\n" "Code size: TAC instructions" "${TAC[unoptimized]}" "${TAC[optimized]}" "$(pct ${TAC[unoptimized]} ${TAC[optimized]})"
printf "%-36s %12s %12s %10s\n" "Code size: MIPS instructions" "${MIPS[unoptimized]}" "${MIPS[optimized]}" "$(pct ${MIPS[unoptimized]} ${MIPS[optimized]})"
printf "%-36s %12s %12s %10s\n" "Work done: MIPS instructions executed" "${EXEC[unoptimized]}" "${EXEC[optimized]}" "$(pct ${EXEC[unoptimized]} ${EXEC[optimized]})"
echo
echo "Executed-instruction runs:  unoptimized:${RUNS_OF[unoptimized]}   optimized:${RUNS_OF[optimized]}"
echo "Spread across $RUNS runs:    unoptimized: ${SPREAD[unoptimized]}   optimized: ${SPREAD[optimized]}"
[ "${OUT[unoptimized]}" = "${OUT[optimized]}" ] || { echo "OUTPUTS DIFFER"; exit 1; }
