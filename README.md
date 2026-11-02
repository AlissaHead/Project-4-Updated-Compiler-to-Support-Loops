# CST-405 Project 3: Complex Variables and Functions

**Author:** Alissa Head (worked alone, so every task below is mine)<br>
**Course:** CST-405 Principles of Compiler Design, Topic 3 (Oct 12 – Nov 1, 2026)

This project extends the Project 2 compiler with arrays, full arithmetic,
functions, recursion, scope and real activation records. It compiles a C-like
language through six phases into MIPS assembly that runs in SPIM / QtSPIM.

```
source.cm ──► scanner ──► parser ──► semantic ──► TAC ──► optimizer ──► codegen ──► peephole ──► source.s
             (tokens)     (AST)      (verdict)   (.tac)  (.optimized.tac)  (MIPS)               (file)
```

### ▶ Video walkthrough

[![Video: the compiler running, how it works, and a live demo](https://img.youtube.com/vi/d2nYQScGFUs/hqdefault.jpg)](https://youtu.be/d2nYQScGFUs)

**Watch on YouTube: https://youtu.be/d2nYQScGFUs**

---

## 1. Quick start

Everything builds and runs on Linux. On Windows I use **WSL (Ubuntu 24.04)**.

```bash
sudo apt install flex bison gcc make spim     # one time

cd compiler
make                 # build ./minicompiler
make check           # unit tests + regression tests (all must PASS)

./minicompiler tests/t3_06_recursion.cm out.s      # full trace of all six phases
./minicompiler tests/t3_06_recursion.cm out.s -q   # quiet: errors + summary only
spim -file out.s                                   # run the generated MIPS
```

| Option    | Effect |
|-----------|--------|
| `-q`      | Quiet. Prints only errors and the final summary. |
| `-O0`     | Skips Phase 5 (TAC optimizer). The back end translates the raw TAC. |
| `-nopeep` | Skips the peephole pass over the generated MIPS. |

Each compile writes `out.s`, `out.tac` (the IR before optimization) and
`out.optimized.tac` (the IR the back end used). The exit status is **0** on
success and **1** if any phase reports an error.

---

## 2. What this milestone adds

| Feature | Example | Test |
|---------|---------|------|
| All four operators, precedence, unary minus | `-(a + b) * c / 2` | `t3_01`, `t3_10` |
| Arrays, read and write | `arr[i] = arr[j] + 1;` | `t3_03`, `t3_09` |
| Global and local arrays | `int g[4];` / `int l[3];` | `t3_03`, `t3_09` |
| Arrays passed by reference | `int sum3(int a[])` | `t3_03`, `t3_09`, `t3_16` |
| Functions, parameters, return values | `int add3(int a, int b, int c)` | `t3_04` |
| Recursion | `fact(5)` is 120 | `t3_06`, `t3_16` |
| Nested calls | `f(1, g(2))`, `f(g(1), 2)` | `t3_04`, `t3_07` |
| Scope, shadowing, block scope | `int x; { int x; }` | `t3_05`, `t3_11` |
| Calls before definitions (two passes) | `main` above its callees | `t3_06`, `t3_12` |
| Real activation records | locals survive calls | `t3_08` |

**Two additions the feature list doesn't name:**

- **`if`/`else` and comparisons (`< > <= >= == !=`).** The project requires
  `factorial(5)`, but recursion cannot stop without a conditional, and `if`
  isn't scheduled until Topic 5. This is the smallest addition that meets the
  requirement. It also gives the flow optimizer real jumps to work on.
- **`void` functions** (an optional extension): `void f(void)`, `return;`. See
  `t3_14`.

---

## 3. The language

```
program     ->  top_list
top_list    ->  top_item  |  top_list top_item
top_item    ->  func_def  |  stmt
func_def    ->  ('int' | 'void') ID '(' params ')' block
params      ->  (empty)  |  'void'  |  param_list
param_list  ->  param  |  param_list ',' param
param       ->  'int' ID  |  'int' ID '[' ']'
block       ->  '{' '}'  |  '{' stmt_list '}'
stmt_list   ->  stmt  |  stmt_list stmt
stmt        ->  decl  |  assign  |  print_stmt  |  return_stmt
             |  if_stmt  |  block  |  func_call ';'
decl        ->  'int' ID ';'  |  'int' ID '=' expr ';'  |  'int' ID '[' NUM ']' ';'
assign      ->  ID '=' expr ';'  |  ID '[' expr ']' '=' expr ';'
return_stmt ->  'return' expr ';'  |  'return' ';'
if_stmt     ->  'if' '(' expr ')' stmt  [ 'else' stmt ]
expr        ->  NUM  |  ID  |  ID '[' expr ']'  |  func_call
             |  expr ('+' | '-' | '*' | '/') expr
             |  expr ('<' | '>' | '<=' | '>=' | '==' | '!=') expr
             |  '-' expr  |  '(' expr ')'
func_call   ->  ID '(' args ')'
args        ->  (empty)  |  expr  |  args ',' expr
print_stmt  ->  'print' '(' expr ')' ';'
```

Precedence, lowest to highest: `== !=`, then `< > <= >=`, then `+ -`, then
`* /`, then unary minus, matching the course grammar reference. All the binary
operators are left-associative. Bare semicolons (empty statements) are not part
of the grammar reference, so `;` alone is a syntax error.

**Keeping Project 2 programs working.** A file with **no functions** is still a
flat list of statements inside an implicit `main()`, exactly as in Project 2,
which is why every Project 2 test passes unchanged. In a file that **does**
define a function, executable statements must be inside a function, and `main`
is required.

---

## 4. The six phases

| # | Phase | File(s) | Receives | Produces |
|---|-------|---------|----------|----------|
| 1 | Lexical analysis | `scanner.l` | characters | tokens with line and column |
| 2 | Syntax analysis | `parser.y`, `ast.c` | tokens | abstract syntax tree |
| 3 | Semantic analysis | `semantic.c` | AST | resolved AST + verdict |
| 4 | Intermediate code | `tac.c` | AST | three-address code (`.tac`) |
| 5 | Optimization | `tac.c` | TAC | smaller TAC (`.optimized.tac`) |
| 6 | Code generation | `codegen.c`, `symtab.c`, `peephole.c` | optimized TAC | MIPS (`.s`) |

The driver, `main.c`, stops at the first phase that reports an error, so an
incorrect program never produces assembly.

### Phase 1: Scanner
New tokens: `return if else void`, `/`, the comparisons, and `{ } [ ] ,`.
**Longest match** keeps `/` from swallowing a comment (`//` and `/*` match more
characters) and makes `<=` beat `<`. **First match** puts keywords above the
identifier rule. Later keywords (`while`, `for`, `break`, `switch`, `case`,
`default`) are already reserved, so using one as a name is a clear syntax error.

### Phase 2: Parser and AST
New nodes: `FUNC_DEF`, `PARAM`, `PARAM_LIST`, `FUNC_CALL`, `ARG_LIST`, `RETURN`,
`BLOCK`, `ARRAY_DECL`, `ARRAY_INDEX`, `IF`.

- **Variable or function?** Both start `int ID`. An LALR(1) parser shifts that
  shared prefix and lets the next token decide: `(` means a function, and `;`,
  `=` or `[` means a variable.
- **Precedence** comes from the `%left` table, not from term/factor rules.
  `%prec UMINUS` makes `-2 * 3` parse as `(-2) * 3`.
- **Unary minus** is a `BINOP` with op `'u'` and no right operand.
- **Parentheses build no node.** The tree's shape already records the grouping.
- Each node records the line its construct **started** on (`%locations`),
  because by reduce time Bison may already have read the next line.
- Error productions name the mistake and give the line, the column and a caret.

### Phase 3: Semantic analysis

Two passes, so a function can be called above its definition:

1. **Pass 1** records every function's signature (name, parameter count, which
   parameters are arrays, return type).
2. **Pass 2** declares globals and checks every body against those signatures.

| Check | Example | Test |
|-------|---------|------|
| Undeclared variable or array | `ghost[0] = 1;` | `t3_20` |
| Call to an undeclared function | `missing(a)` | `t3_20` |
| Wrong argument count | `one(1, 2)` | `t3_21` |
| Duplicate in the same scope | `{ int c; int c; }` | `t3_22` |
| Duplicate function | two `f`s | `t3_22` |
| Array size of zero or less | `int a[0];`, `int a[-3];` | `t3_23` |
| Array and scalar misuse | `x = a + 1;`, `x[0] = 1;` | `t3_24` |
| Wrong argument kind | array where a value belongs | `t3_24` |
| `return` type mismatch, void value used | `int f() { return; }` | `t3_25` |
| More than 4 parameters | `f(a, b, c, d, e)` | `t3_26` |
| Statement outside a function | `total = 1;` at file scope | `t3_26` |
| Missing `main`, or `main` with parameters | | `t3_27`, `t3_28` |
| A call to `main` | `main();` | `t3_32` |
| `return` outside a function | | `t3_29` |

**Scope management: two symbol tables, two questions.**

| | Scope stack (`semantic.c`) | Storage table (`symtab.c`) |
|---|---|---|
| Question | Is this name **visible** here, and what does it mean? | What **address** is this name? |
| Phase | 3 | 6 |
| Shape | A stack of scopes, pushed and popped | One table per function, plus globals |
| Holds | Names, is-it-an-array | Names, offsets, sizes, is-it-an-array |
| Lifetime | Discarded after analysis | Rebuilt for each function |

Lookup searches the stack innermost-first, so shadowing needs no special code.
Function-level scope holds the parameters and the body's top-level declarations
together, which is why `int f(int a) { int a; }` is a duplicate. Variables and
functions are separate namespaces, so `int total;` and `int total(...)` can
coexist (`t3_12`).

**The link between the two tables.** The storage table is flat, one per
function, but a function can contain several variables with the same spelling.
So Phase 3 resolves each use and rewrites the AST to a spelling unique within
the function, such as `x.2`. A `.` can't appear in a source identifier, so it
can't collide with a real name. This also handles a local declared after a line
that used a global of the same name (`t3_11`).

### Phase 4: Three-address code
`generateTACExpr` walks an expression **post-order** and returns the **name** of
the location holding the result.

| Source | TAC |
|--------|-----|
| `a[i]` | `t0 = a[i]` |
| `a[i] = v` | `a[i] = v` |
| `-x` | `t0 = -x` |
| `f(x, y)` | `ARG x` · `ARG y` · `t0 = CALL f, 2` |
| `return e` | `RETURN e` |
| `if (c) s` | `IF_FALSE c GOTO L0` · `s` · `L0:` |

**A design decision: when ARG instructions are emitted.** The alternative was
to emit an `ARG` right after each argument and have the back end keep a stack of
pending arguments, so that `f(1, g(2))` doesn't mix up whose arguments are whose.
Instead, every argument is evaluated first, and then all the `ARG`s are emitted
in one unbroken run just before the `CALL`. `g`'s whole call finishes before any
`ARG` of `f` exists, so nesting needs no extra machinery at all.

### Phase 5: Optimization
Passes repeat until nothing changes (a fixed point).

| Group | Techniques |
|-------|-----------|
| Value | constant folding, constant and copy propagation, algebraic simplification, dead code and dead store elimination |
| Flow | branch simplification, jump threading, jump-to-next removal, unreachable code removal, unused label removal |

Functions added new safety rules:

- **Known values are forgotten at `FUNC_BEGIN` and at labels.** A function can
  be entered from anywhere, so nothing learned elsewhere holds inside it.
- **A call forgets everything about globals**, including "this temporary is a
  copy of that global", because the callee may assign any global.
- **Propagation only touches operands that are values.** A `CALL`'s function
  name and an array's name are left alone, even if a variable has the same spelling.
- **An unused division is kept**, because it can still trap on a zero divisor.

`t3_13` shows the flow passes working: `if (2 > 3) { print(777); }` and the else
branch of `if (1)` are removed, so `777` doesn't appear in the assembly at all.

### Phase 6: Code generation
Every name has a permanent home in memory, and `$t0`–`$t9` act as a **write-back
cache** over those homes. The cache is flushed before every `jal`, label, jump
and branch, because memory is the one thing every path agrees on.

**The peephole pass** then cleans the finished assembly: self-moves, store/load
and load/store pairs, jumps to the next line, jump chains and dead register writes.

---

## 5. Arrays

**Arrays are passed by reference.** The callee receives the base **address** of
the caller's array, one word, not a copy. That is why an array parameter is
written `int a[]` with **no size**: the callee's slot is four bytes however long
the array is, so there's nothing a size would change. It also means a write
through the parameter changes the caller's array (`t3_09`), and that a function
taking an array usually needs the count passed as well.

An element's address is `base + index * 4`, with the multiply done as a shift.
The only real difference between array kinds is where `base` comes from:

| Kind | Elements live in | Base obtained by |
|------|-----------------|-----------------|
| Global | `.data` | `la $t, g_name` |
| Local | this frame | `addi $t, $sp, offset` |
| Parameter | someone else's frame or `.data` | `lw $t, offset($sp)`: the slot **holds** the address |

```asm
# int f(int a[], int i) { return a[i]; }
    sll  $t2, $t1, 2         # index * 4
    add  $t2, $t2, $t0       # base + index * 4
    lw   $t3, 0($t2)
```

---

## 6. Functions and the activation record

```
    high address
    ┌──────────────────────────┐
    │ saved $ra                │  frameSize-4($sp)
    ├──────────────────────────┤
    │ (reserved / alignment)   │  frameSize-8($sp)
    ├──────────────────────────┤
    │ parameters, locals,      │
    │ arrays, temporaries      │  0($sp) ...
    └──────────────────────────┘  <- $sp
    low address
```

**The calling sequence:**

| Caller | Callee |
|--------|--------|
| 1. Evaluate the arguments | 1. Push the frame: `addi $sp, $sp, -size` |
| 2. Put them in `$a0`–`$a3` | 2. Save `$ra` in the frame |
| 3. Flush live registers to this frame | 3. Copy parameters from `$a` registers into the frame |
| 4. `jal` | 4. Run the body |
| 5. Take the result from `$v0` | 5. Put the result in `$v0`, jump to the epilogue |
| | 6. Restore `$ra`, pop the frame, `jr $ra` |

Steps 2 and 3 of the caller must stay in that order: the arguments are read
while the registers are still valid. `$t0`–`$t9` are caller-saved, so every live
value goes back to **this activation's** frame before the `jal`. That is the
whole reason recursion works: each call's `n` lives in its own frame.

`return` **jumps to the epilogue**, the one place that restores `$ra` and pops
the frame, so a `return` in the middle of a function skips everything after it.

**`fact`, as compiled.** The compiler prints this frame during Phase 6:

```
Activation record for 'fact': 24 bytes
    n              int        0($sp)
    t0             int        4($sp)
    t1             int        8($sp)
```
```asm
fn_fact:
    addi $sp, $sp, -24        # build activation record
    sw   $ra, 20($sp)         # save return address
    sw   $a0, 0($sp)          # parameter n
    ...
    jal  fn_fact              # n stays safe in THIS frame
    move $t0, $v0             # t0 = return value
    lw   $t1, 0($sp)          # reload n
    mul  $t2, $t1, $t0        # t1 = n * t0
    move $v0, $t2             # return t1
fn_fact__epilogue:
    lw   $ra, 20($sp)         # restore return address
    addi $sp, $sp, 24         # discard activation record
    jr   $ra                  # return to caller
```

**The stack at maximum depth of `fact(5)`:** one `main` frame (16 bytes) and five
`fact` frames (24 bytes each, with `n` = 5, 4, 3, 2, 1), 136 bytes in total. Each
frame holds its own `n` and the `$ra` of the call that created it. As the calls
return, each frame is popped and the multiplications happen on the way back up.

---

## 7. Integrating more complex components

The features went in cheapest first: arithmetic, then arrays, then functions.
The approach worked because the interfaces between phases didn't have to change:

- **The phase boundaries:** each phase still receives and produces the same kind of thing.
- **The AST structure:** new features are new node kinds, and the existing nodes
  gained only one field (`arrayLHS`).
- **The TAC instruction set:** it already included the function, array and jump
  opcodes. Phase 4 started emitting them and Phase 6 started lowering them.
- **The Project 2 tests:** they ran after every change, and they run in every
  `make test`.

The risky work wasn't in the new code. It was in **old code meeting new
assumptions**: Project 2's optimizer had only ever seen one function, and its
register cache had rarely been under pressure. New tests found real bugs that
way, and each now has a regression test:

| Bug | Symptom | Found by |
|-----|---------|----------|
| Optimizer facts carried into the next function | Compile failure, or a wrong value printed | `t3_11`, `t3_14` |
| `freeTemp("total")` read the variable as temporary `t0` | A live temporary was reused | `t3_15` |
| The register cache could evict an operand of the instruction being generated | Wrong arithmetic under pressure | `t3_15` |
| A dead division was deleted | A divide-by-zero trap could vanish | Unit test |
| A propagated constant could replace a function's name | A call to the wrong label | `t3_12`, unit test |
| Frames over 32 KB exceeded `addi`/`lw`/`sw` immediates | SPIM rejected or misread the frame | `t3_18` |
| Identifiers over 63 characters were truncated in the register cache | Internal compiler error | `t3_19` |
| Trapping `add` vs wrapping constant folding | Optimized and unoptimized builds disagreed | `t3_19` |

---

## 8. Known limitations

- **No array bounds checking.** An out-of-range index silently reads or writes
  the wrong memory. It can't be known at compile time in general, and there's no
  run-time check.
- **At most four parameters.** Arguments use `$a0`–`$a3` only. A fifth is a
  reported error, not silent corruption.
- **No global initializers.** `int x = 5;` at file scope is reported as an error.
- **Global data is limited to SPIM's default 64 KB data segment.** A global array
  past that faults at run time, in hand-written assembly too. Local arrays have
  no such limit, and frames too large for a 16-bit offset are handled (`t3_18`).
- **`main` cannot be called.** It ends with the exit syscall, not `jr $ra`, so
  a call could never return. This is a reported error.
- **Division by zero** is not detected at compile time. SPIM reports a breakpoint
  exception and keeps running.
- **Arithmetic wraps at 32 bits**, without an overflow trap, so optimized and
  unoptimized builds agree (`t3_19`).
- **One-dimensional arrays only.**
- **Array sizes must be literals.**
- **No loops yet**, so repetition is written with recursion (`t3_16`).
- **Order of evaluation.** In `g + f()`, a global `g` is read after the call, so
  a function that changes `g` affects the result. C leaves this order unspecified.

---

## 9. Testing

```bash
make check        # = make unittest + make test
```

**Current result: 116 / 116 passing** (39 unit tests, 77 regression runs). The
compiler also runs clean under AddressSanitizer and UBSan on every test program.

**Differential testing.** A random-program generator produced 500 programs using
functions, arrays, recursion, `if`, blocks and mixed operators. Each ran through
this compiler in all four optimization modes, and through `gcc`, and every output
matched.

**Class activities.** The programs from activity 1 (precedence), activity 3
(`addUp`), activity 5 (scope and shadowing) and the lecture notes' forward-call
example all give the expected results. Activities 1 and 5 are permanent tests.

**Previous topics' tests still pass.** All Project 2 programs (`t2_01`–`t2_14`,
`ast_demo`) are unchanged and run in every `make test`.

**Regression tests** (`tests/run_tests.sh`). For each `tests/NAME.cm`:
`tests/expected/NAME.out` holds the output SPIM must print, or
`tests/expected/NAME.err` holds the exact messages the compiler must produce.
Programs that compile run **twice**, optimized and with `-O0 -nopeep`, because
an optimizer must never change a program's output. The `.out` files for the
`t3_*` tests come from the hand-worked values in each file's header comment, not
from the compiler's own output.

| Test | Checks |
|------|--------|
| `t3_01`–`t3_05` | Supplied: arithmetic, globals, arrays, functions, scope |
| `t3_06_recursion` | `fact`, `fib` (two calls in one expression), mutual recursion |
| `t3_07_nested_calls` | `f(1, g(2))`, `f(g(1), 2)`, argument order, a local surviving calls |
| `t3_08_activation_records` | A callee can't disturb its caller; early `return` doesn't fall through |
| `t3_09_arrays` | Reads and writes, by-reference, passing a parameter on, calls in subscripts |
| `t3_10_math` | Run-time values that differ under wrong precedence or associativity |
| `t3_11_scope` | Nested and sibling blocks, a global used before a same-named local |
| `t3_12_namespaces` | A variable and function with one name, `add` as a function name |
| `t3_13_if_flow` | Dangling else, every comparison, flow optimizer output |
| `t3_14_void` | `void` functions, `return;`, a discarded result |
| `t3_15_register_pressure` | Regressions for the temporary and register-cache bugs |
| `t3_16_programs` | Recursive sum, max, selection sort, gcd, power |
| `t3_17_comparisons` | The two comparison precedence levels, on run-time values |
| `t3_18_large_frames` | Frames past the 16-bit offset limit, large arrays in recursion |
| `t3_19_limits` | Identifiers over 63 characters, 32-bit wraparound, deep nesting |
| `t3_20`–`t3_32` | Every error in section 4, plus syntax errors and reserved keywords |
| `t3_33`, `t3_34` | The programs from class activities 5 and 1 |

**Unit tests** (`tests/unit/`): 18 for the TAC optimizer and 21 for the peephole
pass, on hand-built input the language can't produce, including cases where a
rule must **not** fire.

---

## 10. CST-405 Compiler Design Checklist

| Checklist item | Where |
|----------------|-------|
| Lexical analyzer | `scanner.l` |
| Syntax analyzer: scans tokens, verifies the grammar | `parser.y` (Bison LALR(1)) |
| Symbol table | `symtab.c` (storage), scope stack in `semantic.c` (visibility) |
| Parse tree creation functions and display | `ast.c` constructors and `printAST`, shown in the trace |
| AST functions in a separate file | `ast.c` / `ast.h` |
| Syntax errors with location | Error productions in `parser.y`: line, column and a caret |
| Certifies correct code when there are no lexical or syntax errors | `main.c`: "✓ Parse succeeded" |
| Semantic analyzer traverses the AST (separate file) | `semantic.c`, two passes |
| Semantic errors with location | Every check in section 4 |
| Certifies semantic correctness | "✓ Semantic analysis passed" |
| Generates 3-address code | `tac.c` `generateTAC` |
| Saves IR to a file | `<output>.tac`, `<output>.optimized.tac` |
| Flow optimization | `tac.c` `flowPass`; see `t3_13` |
| Peephole optimization | `peephole.c` |
| Other optimizations | Folding, propagation, algebraic, dead code, dead stores |
| Code generator: MIPS for SPIM/QtSPIM (separate file) | `codegen.c` |
| Saves the assembly to a file | `<output>.s` |

---

## 11. Project layout

```
compiler/
  scanner.l            Phase 1   tokens, lexical errors
  parser.y             Phase 2   grammar, AST-building actions, syntax errors
  ast.c / ast.h        Phase 2   AST node constructors and printer
  semantic.c / .h      Phase 3   scope stack, two-pass semantic checks
  tac.c / tac.h        Phase 4/5 AST -> TAC, optimizer (value + flow passes)
  codegen.c / .h       Phase 6   TAC -> MIPS, register cache, frames, calls
  symtab.c / .h        Phase 6   name -> storage location
  peephole.c / .h      Phase 6   peephole optimizer over the MIPS
  main.c                         the six-phase driver
  trace.h                        trace() helper (silenced by -q)
  Makefile                       make / test / unittest / check / clean
  tests/*.cm                     test programs
  tests/expected/                expected output (.out) or errors (.err)
  tests/run_tests.sh             regression runner
  tests/unit/                    optimizer and peephole unit tests
screenshots/                     AST tree and terminal output
```

## 12. Credits

The course starter code supplied the headers, the reference scanner, the
`parser.y` prologue, the TAC list and temporary-allocation machinery, Parts 1–3
of `codegen.c` (register cache, addressing, frame layout), the `main.c` driver,
and tests `t2_01`–`t2_06` and `t3_01`–`t3_05`. Everything else is my own work.
Each source file's header comment lists its credits.
