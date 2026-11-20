# CST-405 Project 4: Loops and Optimization

**Author:** Alissa Head (worked alone, so every task below is mine)<br>
**Course:** CST-405 Principles of Compiler Design, Topic 4 (Nov 2 – Nov 22, 2026)

This project extends the Project 3 compiler with loops (`while` and `for`),
`break` and `continue`, the full order of operations, and an optimizer whose
effect is measured. It compiles a C-like language through six phases into MIPS
assembly that runs in SPIM / QtSPIM.

```
source.cm ──► scanner ──► parser ──► semantic ──► TAC ──► optimizer ──► codegen ──► peephole ──► source.s
             (tokens)     (AST)      (verdict)   (.tac)  (.optimized.tac)  (MIPS)               (file)
```

### ▶ Video walkthrough

[![Video: the compiler running, a loop through the pipeline, and the measurements](https://img.youtube.com/vi/OixiLcWLxkc/hqdefault.jpg)](https://youtu.be/OixiLcWLxkc)

**Watch on YouTube: https://youtu.be/OixiLcWLxkc**

---

## 1. Quick start

Everything builds and runs on Linux. On Windows I use **WSL (Ubuntu 24.04)**.

```bash
sudo apt install flex bison gcc make spim     # one time

cd compiler
make                 # build ./minicompiler
make check           # unit tests + regression tests (all must PASS)
make bench           # optimized vs unoptimized, code size and work done

./minicompiler tests/t4_01_while.cm out.s      # full trace of all six phases
./minicompiler tests/t4_01_while.cm out.s -q   # quiet: errors + summary only
spim -file out.s                               # run the generated MIPS
```

| Option     | Effect |
|------------|--------|
| `-q`       | Quiet. Prints only errors and the final summary. |
| `-O0`      | Skips Phase 5 (TAC optimizer). The back end translates the raw TAC. |
| `-onepass` | Stops the TAC optimizer after one pass, to show what a fixed point adds. |
| `-nopeep`  | Skips the peephole pass over the generated MIPS. |

Each compile writes `out.s`, `out.tac` (the IR before optimization) and
`out.optimized.tac` (the IR the back end used). The exit status is **0** on
success and **1** if any phase reports an error.

---

## 2. What this milestone adds

| Requirement | How | Test |
|---|---|---|
| At least one loop form, through all six phases | `while` **and** `for` | `t4_01`, `t4_02`, `t4_11` |
| The test is at the top: a false condition runs the body zero times | the exit test is emitted before the body | `t4_07` |
| Nested loops with distinct labels | one program-wide label counter | `t4_04`, `t4_08` |
| All six relational operators, each yielding 1 or 0 | `slt`, `sgt`, `sle`, `sge`, `seq`, `sne` | `t4_05` |
| Order of operations, demonstrated | full precedence table | `t4_09`, `t3_10`, `t3_17` |
| `break` leaves the innermost loop | a stack of exit labels | `t4_03`, `t4_08` |
| `break` outside a loop is a semantic error with a line number | a loop-depth counter | `t4_20`, `t4_21` |
| At least four optimizations, each named and counted | eleven, with a scorecard | every compile |
| Optimizer runs to a fixed point and reports the pass count | passes repeat until nothing changes | `t4_14` |
| Optimizer forgets everything at a label | facts cleared at every label | `t4_13`, unit tests |
| Measured performance comparison | `make bench` | section 8 |
| All Topic 2 and Topic 3 tests still pass | unchanged, run on every `make test` | `t2_*`, `t3_*` |

**Optional extensions claimed:**

- **A second loop form.** `for` is lowered directly, not desugared into `while`. Section 6 explains why.
- **`continue`.** In a `for` loop, it jumps to the update, so the loop variable still advances (`t4_10`).

Earlier milestones' features are unchanged: arrays, functions, recursion, scope,
activation records, `if`/`else`, `void` functions and the peephole optimizer.

---

## 3. The language

```
stmt        ->  decl  |  assign  |  print_stmt  |  return_stmt
             |  if_stmt  |  while_stmt  |  for_stmt  |  jump_stmt
             |  block  |  func_call ';'
while_stmt  ->  'while' '(' expr ')' stmt
for_stmt    ->  'for' '(' for_assign ';' for_cond ';' for_assign ')' stmt
for_assign  ->  (empty)  |  ID '=' expr  |  ID '[' expr ']' '=' expr
for_cond    ->  (empty)  |  expr              (empty means always true)
jump_stmt   ->  'break' ';'  |  'continue' ';'
expr        ->  expr ('==' | '!=') expr
             |  expr ('<' | '>' | '<=' | '>=') expr
             |  expr ('+' | '-') expr
             |  expr ('*' | '/') expr
             |  '-' expr  |  '(' expr ')'  |  NUM  |  ID  |  ID '[' expr ']'  |  func_call
```

The full grammar, including functions and arrays, is documented at the top of
`compiler/parser.y`.

**Order of operations**, loosest to tightest, all binary operators left-associative:

| Level | Operators | Example |
|---|---|---|
| 1 | `==` `!=` | `a < b == b < c` is `(a < b) == (b < c)` |
| 2 | `<` `>` `<=` `>=` | `a + 1 < b * 2` is `(a + 1) < (b * 2)` |
| 3 | `+` `-` | `10 - 4 - 3` is `(10 - 4) - 3` |
| 4 | `*` `/` | `a + b * c` is `a + (b * c)` |
| 5 | unary `-` | `-a * b` is `(-a) * b` |

`t4_09` checks ten expressions, using parameters so the arithmetic runs on the
machine. Each one prints a different value if the precedence is wrong. For
example, `a + 1 < b * 2` prints 1, but would print 4 if `<` bound tighter than `+`.

---

## 4. A loop through the whole pipeline

`tests/t4_01_while.cm`:
```c
i = 1; s = 0;
while (i <= 10) { s = s + i; i = i + 1; }
print(s);       /* 55 */
print(i - 1);   /* 10 */
```

**AST (Phase 2):**
```
WHILE
  BINOP '<='
    VAR i
    NUM 10
  BLOCK
    ASSIGN s
      BINOP '+'  (VAR s, VAR i)
    ASSIGN i
      BINOP '+'  (VAR i, NUM 1)
```

**TAC (Phase 4).** The labels are what make it a loop:
```
    i = 1
    s = 0
L0:                         <- loop top: the back edge returns here
    t0 = i <= 10
    IF_FALSE t0 GOTO L1     <- test FIRST: a false condition skips the body
    t0 = s + i
    s = t0
    t0 = i + 1
    i = t0
    GOTO L0                 <- the back edge
L1:                         <- loop exit: break jumps here
    PRINT s
```

**MIPS (Phase 6):**
```asm
L0:
    lw   $t0, 0($sp)        # i
    li   $t1, 10
    sle  $t2, $t0, $t1      # t0 = i <= 10
    beqz $t2, L1            # leave the loop when the condition is 0
    lw   $t0, 4($sp)        # s
    lw   $t1, 0($sp)        # i
    addu $t2, $t0, $t1      # t0 = s + i
    move $t0, $t2           # s = t0
    li   $t3, 1
    addu $t2, $t1, $t3      # t0 = i + 1
    move $t1, $t2           # i = t0
    sw   $t0, 4($sp)        # s     values go back to memory before the jump,
    sw   $t1, 0($sp)        # i     because L0 is also reached from above
    j    L0
L1:
```

The program prints 55 and 10.

**Lowering patterns:**
```
while (C) B              for (I; C; U) B
                             I
Ltop:                    Ltop:
    t = C                    t = C          (left out if C is empty)
    IF_FALSE t GOTO Lexit    IF_FALSE t GOTO Lexit
    B                        B
    GOTO Ltop            Lnext:             <- continue lands here
Lexit:                       U
                             GOTO Ltop
                         Lexit:
```

---

## 5. `break`, `continue`, and the semantic check

**Which loop do they leave?** When the TAC generator starts lowering a loop, it
pushes that loop's exit label (for `break`) and next-iteration label (for
`continue`) on a stack, and pops them when the body is finished. A `break`
anywhere in between jumps to whatever is on top, which is always the innermost
loop. Nesting needs no further analysis (`tac.c`, the loop label stack).

**Is it legal?** No grammar can say "only inside a loop": the rule
`'break' ';'` matches wherever the statement appears, because a context-free
production can't see what surrounds it. So semantic analysis keeps a loop-depth
counter. It's increased around loop **bodies** only (not the condition, init or
update), and any `break` or `continue` at depth 0 is an error:

```
╔════════════════════════════════════════════════════════════╗
║ SEMANTIC ERROR - Break Outside Loop                        ║
╚════════════════════════════════════════════════════════════╝
  📍 Location: Line 8
  ❌ Error: 'break' is not inside any loop
```

The depth resets at each function, because a loop around a *call* doesn't enclose
the function being called.

---

## 6. Language design choices

Each choice removes a class of parsing or lowering problems, and each gives
something up.

| # | Choice | What it simplified | What it gave up |
|---|---|---|---|
| 1 | **Assignment is a statement, never an expression.** No `x = y = 0`, no `i++`, no `i += 1`. | Expressions have no side effects of their own, so there are no order-of-evaluation surprises inside one expression, and the optimizer can move or fold any expression freely. The grammar needs no rules for `++` or compound assignment. | The `for` update must be written `i = i + 1`. |
| 2 | **`for` parts are assignments only, with no declarations.** `for (i = 0; ...)`, not `for (int i = 0; ...)`. | A `for` loop doesn't open its own scope, so the loop variable is an ordinary local and needs no new scope rule. | The loop variable must be declared before the loop, and it stays visible afterward (`t4_11` relies on this). |
| 3 | **No comma operator.** | `for` has exactly one assignment in each part, and the grammar has one fewer ambiguity. | `for (i = 0, j = 10; ...)` isn't possible. |
| 4 | **`for` is lowered directly, not desugared into `while`.** | It keeps a separate update label, so `continue` in a `for` loop still runs the update. The alternative rewrites `for (I; C; U) B` as `I; while (C) { B; U; }`, which puts the update at the end of the body, where a `continue` jumps over it and the loop never ends. Desugaring would have meant one less case in the TAC generator, but it breaks the moment `continue` exists. | A second loop case in `tac.c`. The unused update label is removed by the optimizer when nothing jumps to it. |
| 5 | **The test is at the top of the loop.** | A loop whose condition starts false runs zero times, which is what `while` means. Testing at the bottom would silently implement `do-while` (`t4_07` would fail). | One extra jump per iteration compared with a rotated loop (test at the bottom). |
| 6 | **Braces on loop bodies are optional.** | Matches C, and `while` needs no dangling-else handling because it has no optional tail. | A single unbraced statement is easy to misread. Requiring braces would have been a reasonable alternative. |
| 7 | **Comparisons produce an `int` (1 or 0); there is no boolean type.** | Conditions are ordinary expressions, so `while (n)` works and no type checking is needed. | No compile-time check that a condition is really a condition. |

---

## 7. The optimizer

**Eleven techniques, each named and counted by the compiler** (`t4_01`'s
scorecard, printed on every compile without `-q`):

| Value techniques | Flow techniques |
|---|---|
| Constant folding | Branch simplification |
| Constant propagation | Jump threading |
| Copy propagation | Jump-to-next removal |
| Algebraic simplification | Unreachable code removal |
| Dead code elimination | Unused label removal |
| Dead store elimination | |

The peephole optimizer then cleans the MIPS: self moves, store/load pairs, dead
register writes and jump chains.

**Fixed point.** The passes repeat until one changes nothing, and the compiler
reports how many it took. The class activity's listing (`t4_14`) shows why a
single pass isn't enough:

| | TAC instructions | Passes |
|---|---|---|
| `-onepass` | 12 | 1 |
| Fixed point | **7** | 5 |

One pass folds, propagates, simplifies the branch and removes `print(999)`, but
`print(k)` sits behind a label, where the optimizer must forget that `k` is 20.
The label disappears only after its `goto` has been removed, which takes later passes.

**Forgetting at a label.** A label starts a basic block, so control can arrive
from more than one place, and a value true on one path may be false on another.
At a loop's top label, that's the entry path *and* the back edge:

```c
x = 0;
while (x < 3) { print(x); x = x + 1; }
```

Without the rule, `x = 0` is carried into the loop, `x < 3` folds to `0 < 3`, the
test disappears, and the loop never ends. `t4_13` and a unit test cover this.

**One more improvement in the code generator.** At a label or jump, the register
cache writes changed values back to memory. A compiler temporary that nothing
reads afterward is now dropped instead, which removes a dead store from every
loop iteration. The first version assumed no temporary is ever read after a
branch. Random testing against `gcc` proved that wrong: copy propagation can
rewrite `print(c)` as `print(t0)` past an `if`. The final version checks each
temporary individually (`tempReadAhead` in `codegen.c`), and `t4_15` is a
regression test for the case that failed.

---

## 8. Measured performance

**Method:**
- **Baseline:** the same compiler with every optimization off (`-O0 -nopeep`), on the same program.
- **Benchmark:** `compiler/benchmarks/hot_loop.cm`, a nested loop running its inner body 3,000 times.
- **Code size:** TAC instructions (reported by the compiler) and MIPS instructions in the `.s` file. Each instruction counts once.
- **Work done:** MIPS instructions SPIM actually executes. SPIM 8 has no statistics option, so `benchmarks/measure.sh` counts executed instructions with SPIM's `step` command. SPIM's 6 startup instructions are measured separately and subtracted. I verified the counter against a program counted by hand (24 instructions).
- **Repeats:** three runs of each build.

Run it with `make bench`.

**Results**

| Metric | Unoptimized | Optimized | Reduction |
|---|---|---|---|
| Code size: TAC instructions | 45 | 41 | 8.9% |
| Code size: MIPS instructions | 80 | 58 | 27.5% |
| **Work done: MIPS instructions executed** | **102,997** | **60,990** | **40.8%** |

| Runs | Unoptimized | Optimized | Spread |
|---|---|---|---|
| Executed instructions | 102,997 · 102,997 · 102,997 | 60,990 · 60,990 · 60,990 | 0 |

**Why the numbers disagree.** Code size counts each instruction once, while work
done counts it once *per execution*. Most of what the optimizer removed was
inside the inner loop: `i * 1 + 0`, `2 * 4` and `3 * 3 - 9` are simplified or
folded there. So each instruction saved is saved 3,000 times, and a small
code-size saving (8.9% of TAC) becomes a large work saving (40.8%).

A straight-line program shows the opposite. On `t4_06`, every instruction runs
exactly once, so the two numbers agree:

| `t4_06_optimize` (no loop) | Unoptimized | Optimized | Reduction |
|---|---|---|---|
| TAC instructions | 15 | 7 | 53.3% |
| MIPS instructions executed | 26 | 12 | 53.8% |

**Threshold.** The spread is 0 because SPIM is a deterministic simulator: the
count depends only on the generated code, not on the machine or what else is
running. So any difference in executed instructions is real, and I treat a
change of more than 1% as significant. That only rules out trivial differences
such as one instruction of setup code. Wall-clock time on the host would vary
from run to run and would need a much larger threshold. That's why
instructions executed is the metric used here.

---

## 9. Integrating the new components

The work went in the order the lecture recommends: relational operators, then
`while` through all six phases, then `for`, `break` and `continue`, then
measurement. The interfaces didn't change. The TAC instruction set already had
`LABEL`, `GOTO` and `IF_FALSE`, and the code generator already flushed its
register cache at labels for `if`. Most of the loop work was new cases in
existing switch statements.

**Testing found two real bugs:**

| Bug | Found by | Fix |
|---|---|---|
| A loop syntax error's recovery skipped to the next `;`, hiding the errors in the loop body | `t4_23` | Error rules that resume at the loop header's `)` |
| Dropping a dead temporary at a branch also dropped one copy propagation had made live | 600 random programs compared with `gcc` | Per-temporary check (`t4_15`) |

---

## 10. Known limitations

- **No data-flow analysis.** The optimizer forgets everything at every label, so
  it can't propagate a constant into a loop even when that would be safe. Building
  a flow graph and computing reaching definitions is the next improvement.
- **Dead-store elimination for variables** applies only to functions with no labels.
- **No loop-invariant code motion, strength reduction or loop unrolling.**
- From earlier milestones: no array bounds checking, at most four parameters, no
  global initializers, global data limited by SPIM's 64 KB default, and `main`
  cannot be called.

---

## 11. Testing

```bash
make check        # = make unittest + make test
```

**Current result: 152 / 152 passing** (41 unit tests, 111 regression runs). The
compiler also runs clean under AddressSanitizer and UBSan on every test program.

**Previous topics' tests still pass:** every Project 2 test (`t2_*`, `ast_demo`)
and Project 3 test (`t3_*`) is unchanged and runs on every `make test`.

**Random differential testing:** a generator produced 600 programs with nested
`while` and `for` loops, `break`, `continue`, `if`, functions and arrays. Each
ran through this compiler in all four optimization modes and through `gcc`, and
every output matched.

| Test | Checks |
|---|---|
| `t4_01`–`t4_06` | Supplied: while, for, break, nested loops, relational operators, optimizer |
| `t4_07_zero_iterations` | A false condition runs the body zero times, for both loop forms |
| `t4_08_nested_break` | `break` leaves only the innermost loop, up to three levels |
| `t4_09_order_of_operations` | Ten expressions whose value changes under the wrong precedence |
| `t4_10_continue` | `continue` in `while`, in `for` (runs the update), and nested |
| `t4_11_for_forms` | Empty parts, `for (;;)`, an array element as the loop variable |
| `t4_12_loops_and_functions` | Loops with recursion, arrays, and `return` from inside a loop |
| `t4_13_optimizer_loops` | Values that would be wrong if facts crossed a loop label |
| `t4_14_fixed_point` | The activity listing: one pass versus a fixed point |
| `t4_15_temp_across_branch` | Regression test for the bug found by random testing |
| `t4_20`–`t4_23` | `break`/`continue` outside a loop, errors in loop parts, loop syntax errors |

The `.out` files for my tests come from the hand-worked values in each file's
header, and I checked those values against `gcc`.

---

## 12. CST-405 Compiler Design Checklist

| Checklist item | Where |
|---|---|
| Lexical analyzer | `scanner.l` (loop keywords and relational operators added) |
| Syntax analyzer: scans tokens, verifies the grammar | `parser.y` (Bison LALR(1), no conflicts) |
| Symbol table | `symtab.c` (storage) and the scope stack in `semantic.c` (visibility) |
| Parse tree creation functions and display | `ast.c`, including `WHILE`, `FOR`, `BREAK`, `CONTINUE` nodes |
| Syntax errors with location | Error productions with line, column and caret (`t4_23`) |
| Certifies correct code when there are no lexical or syntax errors | `main.c`: "✓ Parse succeeded" |
| Semantic analyzer (separate file) | `semantic.c`, including the loop-depth check |
| Semantic errors with location | `t4_20`–`t4_22` |
| Generates 3-address code, saved to a file | `tac.c`; `<output>.tac`, `<output>.optimized.tac` |
| Flow optimization | Branch simplification, jump threading, jump-to-next, unreachable code, unused labels |
| Peephole optimization | `peephole.c` |
| Other optimizations | Folding, propagation, algebraic, dead code, dead stores |
| Code generator: MIPS for SPIM/QtSPIM, saved to a file | `codegen.c`; `<output>.s` |

---

## 13. Project layout

```
compiler/
  scanner.l            Phase 1   tokens, lexical errors
  parser.y             Phase 2   grammar, AST-building actions, syntax errors
  ast.c / ast.h        Phase 2   AST node constructors and printer
  semantic.c / .h      Phase 3   scope stack, two-pass checks, loop-depth check
  tac.c / tac.h        Phase 4/5 AST -> TAC (loop lowering), optimizer
  codegen.c / .h       Phase 6   TAC -> MIPS, register cache, frames, calls
  symtab.c / .h        Phase 6   name -> storage location
  peephole.c / .h      Phase 6   peephole optimizer over the MIPS
  main.c                         the six-phase driver
  benchmarks/                    hot_loop.cm and measure.sh (make bench)
  tests/*.cm                     test programs
  tests/expected/                expected output (.out) or errors (.err)
  tests/run_tests.sh             regression runner
  tests/unit/                    optimizer and peephole unit tests
```

## 14. Credits

The course starter code supplied the headers, the reference scanner, the
`parser.y` prologue, the TAC list and temporary-allocation machinery, Parts 1–3
of `codegen.c`, the `main.c` driver, and tests `t2_01`–`t2_06`, `t3_01`–`t3_05`
and `t4_01`–`t4_06`. Everything else is my own work. Each source file's header
comment lists its credits.
