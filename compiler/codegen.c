/* =========================================================================
 * CST-405  ·  Project 4  ·  Loops and Optimization
 * FILE: codegen.c   —   Phase 6 — MIPS code generation
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                                                  ^^^^^^^  this file
 *
 * PHASE
 *   Phase 6, MIPS code generation.
 *
 * RECEIVES
 *   The optimized TAC from Phase 5.
 *
 * PRODUCES
 *   A MIPS assembly file for SPIM, after the peephole pass in peephole.c.
 *
 * CREDITS
 *   Parts 1-3 (register cache, addressing, frame layout) from the CST-405 course starter code.
 *   Array addressing, instruction selection, the calling sequence and
 *   the peephole hookup: Alissa Head.
 * ========================================================================= */

/* ============================================================================
 * PHASE 6 — MIPS CODE GENERATION
 * ----------------------------------------------------------------------------
 * Reads the optimized three-address code and writes MIPS assembly.
 *
 * The structure of this file mirrors the two problems named in codegen.h:
 *
 *   PART 1  Register cache   — allocate, spill, reload  ($t0-$t9)
 *   PART 2  Addressing       — turn a name into an address (global / local /
 *                              array element / array passed by reference)
 *   PART 3  Frame layout     — measure a function, emit prologue and epilogue
 *   PART 4  Instruction emit — one case per TAC opcode
 *
 * Read it in that order.  Nothing in PART 4 is complicated once PARTS 1-3
 * are understood; that is the whole point of separating them.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen.h"
#include "peephole.h"
#include "symtab.h"
#include "trace.h"

static FILE* out;                 /* The .s file being written              */
RegisterAllocator regAlloc;       /* The $t0-$t9 cache                      */


/* The register-clock value when the current TAC instruction began.  Anything
 * used since then is an operand of that instruction: see selectVictimReg. */
static int instrStart = 0;

static int frameSize  = 0;        /* Size of the frame being generated       */
static const char* currentFunc = NULL;

/* ==========================================================================
 * NAME MANGLING
 * --------------------------------------------------------------------------
 * Source identifiers and assembler labels share one namespace, and the
 * assembler's namespace already contains every instruction mnemonic.  A user
 * who writes
 *         int add(int a, int b) { return a + b; }
 * would otherwise make us emit the label `add:` — and SPIM would reject the
 * file, pointing at a line the programmer never wrote.
 *
 * The fix every real compiler uses is to keep the two namespaces apart by
 * decorating names on the way out:
 *
 *      function add   ->   fn_add          global counter  ->  g_counter
 *      function main  ->   main            (the entry point must keep its name)
 *
 * Two static buffers are enough because no call site needs more than two
 * mangled names alive at once.
 * ========================================================================*/
static const char* funcLabel(const char* name) {
    static char buf[2][MAX_VAR_NAME + 8];
    static int which = 0;
    if (strcmp(name, "main") == 0) return "main";   /* the entry point */
    which ^= 1;
    snprintf(buf[which], sizeof buf[which], "fn_%s", name);
    return buf[which];
}

static const char* dataLabel(const char* name) {
    static char buf[2][MAX_VAR_NAME + 8];
    static int which = 0;
    which ^= 1;
    snprintf(buf[which], sizeof buf[which], "g_%s", name);
    return buf[which];
}

/* ==========================================================================
 * Small predicates used throughout
 * ========================================================================*/

/* Is this TAC operand a literal integer rather than a name? */
static int isConstant(const char* s) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Is this a compiler-generated temporary (t0, t1, ...)?  Temporaries are
 * reserved identifiers — semantic.c rejects user variables of this shape —
 * so the test is safe. */
static int isTemp(const char* s) {
    if (!s || s[0] != 't' || !s[1]) return 0;
    for (int i = 1; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* ==========================================================================
 * PART 1 — REGISTER CACHE
 * --------------------------------------------------------------------------
 * Ten registers stand in for an unbounded set of TAC names.  The rules:
 *
 *   ensureReg(name)  the register must hold the CURRENT value of `name`
 *                    (load it from memory if it is not already cached)
 *   defReg(name)     the register is about to be OVERWRITTEN with a new
 *                    value of `name` (no load needed; mark it dirty)
 *   scratchReg()     an anonymous register for address arithmetic
 *
 * When no register is free the least-recently-used one is evicted.  Eviction
 * writes the value back first if it is dirty AND the name has a memory home.
 * Literals and scratch values have no home, so evicting them is free.
 * ========================================================================*/

static void emitStoreHome(int reg, const char* name);
static void emitLoadHome(int reg, const char* name);

/* Start a function with an empty cache.  The spill and load counts keep
 * running, because the report at the end covers the whole program. */
void initRegAlloc(void) {
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        regAlloc.regs[i].varName[0] = '\0';
        regAlloc.regs[i].isDirty    = 0;
        regAlloc.regs[i].inUse      = 0;
        regAlloc.regs[i].hasHome    = 0;
        regAlloc.regs[i].lastUsed   = 0;
    }
    regAlloc.timestamp = 0;
    instrStart = 0;
}

/* Which register currently caches `name`?  -1 if none. */
static int findVarReg(const char* name) {
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        if (regAlloc.regs[i].inUse && strcmp(regAlloc.regs[i].varName, name) == 0) {
            regAlloc.regs[i].lastUsed = ++regAlloc.timestamp;
            return i;
        }
    }
    return -1;
}

/* Least-recently-used register — the one whose value we can most afford
 * to lose.  A real compiler would use liveness information here; LRU is a
 * good approximation and is easy to reason about. */
static int selectVictimReg(void) {
    int victim = -1, oldest = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        /* A register touched by the instruction being generated is holding
         * one of its operands, and evicting it would corrupt that very
         * instruction.  One instruction uses far fewer than ten registers,
         * so a victim always exists. */
        if (regAlloc.regs[i].lastUsed > instrStart) continue;
        /* Values with no memory home are the cheapest to discard */
        if (!regAlloc.regs[i].hasHome && regAlloc.regs[i].inUse) return i;
        if (victim == -1 || regAlloc.regs[i].lastUsed < oldest) {
            oldest = regAlloc.regs[i].lastUsed;
            victim = i;
        }
    }
    return victim;
}

/* Write a register back to its memory home and mark it clean. */
static void spillReg(int r) {
    if (regAlloc.regs[r].inUse && regAlloc.regs[r].isDirty && regAlloc.regs[r].hasHome) {
        fprintf(out, "    # spill: %s -> memory\n", regAlloc.regs[r].varName);
        emitStoreHome(r, regAlloc.regs[r].varName);
        regAlloc.spillCount++;
    }
    regAlloc.regs[r].isDirty = 0;
}

/* Grab a register for `name`.  `load` says whether the register must be
 * primed with the value already in memory. */
static int getReg(const char* name, int hasHome, int load) {
    int r = findVarReg(name);
    if (r != -1) return r;                       /* Already cached          */

    for (r = 0; r < NUM_TEMP_REGS; r++)          /* A free register?        */
        if (!regAlloc.regs[r].inUse) break;

    if (r == NUM_TEMP_REGS) {                    /* None free — evict LRU   */
        r = selectVictimReg();
        spillReg(r);
    }

    strncpy(regAlloc.regs[r].varName, name, MAX_VAR_NAME - 1);
    regAlloc.regs[r].varName[MAX_VAR_NAME - 1] = '\0';
    regAlloc.regs[r].inUse    = 1;
    regAlloc.regs[r].isDirty  = 0;
    regAlloc.regs[r].hasHome  = hasHome;
    regAlloc.regs[r].lastUsed = ++regAlloc.timestamp;

    if (load) {
        emitLoadHome(r, name);
        regAlloc.loadCount++;
    }
    return r;
}

/* Register holding the current value of `name` (loading it if necessary). */
static int ensureReg(const char* name) { return getReg(name, 1, 1); }

/* Register that will RECEIVE a new value for `name`. */
static int defReg(const char* name) {
    int r = getReg(name, 1, 0);
    regAlloc.regs[r].isDirty = 1;
    return r;
}

static void releaseReg(int r) {
    if (r < 0) return;
    regAlloc.regs[r].inUse      = 0;
    regAlloc.regs[r].isDirty    = 0;
    regAlloc.regs[r].hasHome    = 0;
    regAlloc.regs[r].varName[0] = '\0';
}

/* Materialise any TAC operand — literal or name — into a register. */
static int operandReg(const char* s) {
    if (isConstant(s)) {
        char n[MAX_VAR_NAME];
        snprintf(n, sizeof n, "#k%s", s);
        int r = findVarReg(n);
        if (r != -1) return r;
        r = getReg(n, 0, 0);
        fprintf(out, "    li   $t%d, %s\n", r, s);
        return r;
    }
    return ensureReg(s);
}

/* Write every dirty register back to memory, then forget all cached values.
 * Called before `jal` (the callee owns $t0-$t9) and before any label, since
 * control may reach a label from somewhere with a different cache state. */
static void flushRegisters(const char* why) {
    int any = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++)
        if (regAlloc.regs[i].inUse && regAlloc.regs[i].isDirty && regAlloc.regs[i].hasHome) any = 1;
    if (any) fprintf(out, "    # write back live values (%s)\n", why);
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        spillReg(i);
        releaseReg(i);
    }
}

/* Is temporary `name` read by any instruction from `from` up to the next
 * label, jump or end of function?  That is the only stretch where a value
 * left in a temporary can still be used: every path into a label is a fresh
 * start, because the optimizer forgets what it knew at a label and the TAC
 * generator never carries a temporary from one statement into the next. */
static int tempReadAhead(TACInstr* from, const char* name) {
    for (TACInstr* c = from; c; c = c->next) {
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_END) return 0;
        if (c->arg1 && strcmp(c->arg1, name) == 0) return 1;
        if (c->arg2 && strcmp(c->arg2, name) == 0) return 1;
        if (c->op == TAC_ARRAY_STORE && c->result && strcmp(c->result, name) == 0) return 1;
        /* overwritten before anything read it: the old value is dead.  (The
         * reads of this same instruction were checked just above, so
         * t0 = t0 + 1 still counts as a read.) */
        if (c->op != TAC_ARRAY_STORE && c->result && strcmp(c->result, name) == 0) return 0;
        if (c->op == TAC_GOTO) return 0;
    }
    return 0;
}

/* The same flush at a LABEL or a JUMP, except that a temporary nothing will
 * read again is dropped instead of written back to memory.  In a loop this
 * removes a dead store on every iteration (the loop condition's temporary).
 *
 * The check is per temporary, not a blanket rule, for a reason found by
 * testing: copy propagation can rewrite `c = t3; ... print(c)` into
 * `print(t3)` past an if-branch, and on the fall-through path t3 IS still
 * needed.  (Calls keep using flushRegisters: in f(1) + g(2) the result of f
 * must survive the call to g.) */
static void flushAtBranch(const char* why, TACInstr* next) {
    for (int i = 0; i < NUM_TEMP_REGS; i++)
        if (regAlloc.regs[i].inUse && isTemp(regAlloc.regs[i].varName) &&
            !tempReadAhead(next, regAlloc.regs[i].varName))
            regAlloc.regs[i].isDirty = 0;
    flushRegisters(why);
}

void printRegAllocStats(void) {
    printf("  Register spills (store to memory) : %d\n", regAlloc.spillCount);
    printf("  Register reloads (load from memory): %d\n", regAlloc.loadCount);
}

/* ==========================================================================
 * PART 2 — ADDRESSING
 * --------------------------------------------------------------------------
 * Turning a name into an address is the whole difference between a global,
 * a local, and an array parameter.  All three cases live here so that the
 * instruction cases in PART 4 never have to think about it.
 * ========================================================================*/

/* Emit `lw` or `sw` of register `reg` at `offset` bytes into the frame.  An
 * offset over 32767 does not fit the instruction's 16-bit field, and SPIM
 * miscompiles such an operand rather than rejecting it, so the address is
 * computed in $v1 first.  $v1 is free: this compiler uses it for nothing else,
 * and it never holds a value across an instruction. */
static void emitFrameAccess(const char* op, const char* reg, int offset, const char* note) {
    if (offset <= 32767) {
        fprintf(out, "    %s   %s, %d($sp)        # %s\n", op, reg, offset, note);
    } else {
        fprintf(out, "    li   $v1, %d\n", offset);
        fprintf(out, "    add  $v1, $v1, $sp\n");
        fprintf(out, "    %s   %s, 0($v1)        # %s\n", op, reg, note);
    }
}

static void emitLoadHome(int reg, const char* name) {
    Symbol* s = lookupSymbol(name);
    if (!s) {
        fprintf(stderr, "codegen: '%s' has no storage assigned "
                        "(internal error in function '%s')\n",
                name, currentFunc ? currentFunc : "?");
        exit(1);
    }
    if (s->isGlobal) fprintf(out, "    lw   $t%d, %s\n", reg, dataLabel(s->name));
    else {
        char r[8];
        snprintf(r, sizeof r, "$t%d", reg);
        emitFrameAccess("lw", r, s->offset, s->name);
    }
}

static void emitStoreHome(int reg, const char* name) {
    Symbol* s = lookupSymbol(name);
    if (!s) {
        fprintf(stderr, "codegen: '%s' has no storage assigned "
                        "(internal error in function '%s')\n",
                name, currentFunc ? currentFunc : "?");
        exit(1);
    }
    if (s->isGlobal) fprintf(out, "    sw   $t%d, %s\n", reg, dataLabel(s->name));
    else {
        char r[8];
        snprintf(r, sizeof r, "$t%d", reg);
        emitFrameAccess("sw", r, s->offset, s->name);
    }
}

/* Copy the base address of a declared array (not an array parameter) into
 * register `reg`, which may be an $a register when passing it to a function.
 * Where the base comes from is the only real difference between the two:
 *   global   la   $t, g_name      the elements are at a label in .data
 *   local    addi $t, $sp, off    the elements are in this frame            */
static void emitArrayBase(const char* reg, Symbol* s) {
    if (s->isGlobal) {
        fprintf(out, "    la   %s, %s\n", reg, dataLabel(s->name));
    } else if (s->offset <= 32767) {
        fprintf(out, "    addi %s, $sp, %d        # base of %s\n", reg, s->offset, s->name);
    } else {
        /* The offset doesn't fit addi's 16-bit immediate. */
        fprintf(out, "    li   %s, %d\n", reg, s->offset);
        fprintf(out, "    add  %s, %s, $sp        # base of %s\n", reg, reg, s->name);
    }
}

/* Return a register holding the address of array[index].  The third kind of
 * array, a parameter, is different: its slot holds someone ELSE'S base
 * address, so its base is simply the slot's VALUE and ensureReg finds it.
 *
 * The element address is base + index*4.  The multiply is a shift, because
 * the element width is a power of two.  The caller gives the register back
 * with releaseReg once the load or store has been written. */
static int elementAddrReg(const char* array, const char* index) {
    Symbol* s = lookupSymbol(array);
    if (!s) {
        fprintf(stderr, "codegen: array '%s' has no storage assigned\n", array);
        exit(1);
    }

    int i = operandReg(index);
    int base, scratchBase = 0;
    if (s->isParamArray) {
        base = ensureReg(array);
    } else {
        char reg[8];
        base = getReg("#base", 0, 0);
        scratchBase = 1;
        snprintf(reg, sizeof reg, "$t%d", base);
        emitArrayBase(reg, s);
    }

    int addr = getReg("#addr", 0, 0);
    fprintf(out, "    sll  $t%d, $t%d, 2\n", addr, i);
    fprintf(out, "    add  $t%d, $t%d, $t%d      # address of %s[%s]\n",
            addr, addr, base, array, index);
    if (scratchBase) releaseReg(base);
    return addr;
}

/* ==========================================================================
 * PART 3 — FRAME LAYOUT
 * --------------------------------------------------------------------------
 * Before emitting a single instruction of a function we walk its TAC once and
 * hand a memory home to every name it mentions.  Doing this up front is what
 * lets the register allocator spill anything at any moment.
 * ========================================================================*/

/* Give `name` a home if it does not have one yet (used for temporaries). */
static void reserveTemp(const char* name) {
    if (!name || isConstant(name)) return;
    if (!isTemp(name)) return;              /* declared names get homes from
                                             * their DECL/PARAM instruction */
    if (lookupSymbol(name)) return;
    addVar((char*)name, "int");
}

/* Walk one function's TAC (FUNC_BEGIN .. FUNC_END), populate the local symbol
 * table, and return the total frame size in bytes. */
static int layoutFrame(TACInstr* funcBegin) {
    initSymTab();

    for (TACInstr* c = funcBegin->next; c && c->op != TAC_FUNC_END; c = c->next) {
        switch (c->op) {
            case TAC_PARAM:
                /* arg1 carries "int[]" for arrays passed by reference */
                if (c->arg1 && strcmp(c->arg1, "int[]") == 0) addArrayParam(c->result);
                else                                          addVar(c->result, "int");
                break;
            case TAC_DECL:
                addVar(c->result, c->arg1 ? c->arg1 : "int");
                break;
            case TAC_ARRAY_DECL:
                addArray(c->result, atoi(c->arg1));
                break;
            default: break;
        }
        /* Any temporary mentioned anywhere needs a home too */
        reserveTemp(c->result);
        reserveTemp(c->arg1);
        reserveTemp(c->arg2);
    }

    /* locals + saved $ra + alignment padding, rounded up to a multiple of 8 */
    int size = getLocalBytes() + 8;
    if (size % 8) size += 8 - (size % 8);
    return size;
}

/* Move the stack pointer by `delta` bytes.  `addi` holds only a 16-bit
 * immediate, and a frame with a large local array can exceed that, so a big
 * frame goes through $v1, the one register this compiler never otherwise uses.
 * ($v0 can't be used: in an epilogue it holds the return value.) */
static void emitAdjustSp(int delta, const char* why) {
    if (delta >= -32768 && delta <= 32767) {
        fprintf(out, "    addi $sp, $sp, %d        # %s\n", delta, why);
    } else {
        fprintf(out, "    li   $v1, %d\n", delta);
        fprintf(out, "    add  $sp, $sp, $v1        # %s\n", why);
    }
}

static void emitPrologue(const char* name, int size) {
    fprintf(out, "\n# ==== function %s ====\n", name);
    fprintf(out, "%s:\n", funcLabel(name));
    emitAdjustSp(-size, "build activation record");
    emitFrameAccess("sw", "$ra", size - 4, "save return address");
}

static void emitEpilogue(const char* name, int size) {
    fprintf(out, "%s__epilogue:\n", funcLabel(name));
    emitFrameAccess("lw", "$ra", size - 4, "restore return address");
    emitAdjustSp(size, "discard activation record");
    if (strcmp(name, "main") == 0) {
        fprintf(out, "    li   $v0, 10             # syscall 10 = exit\n");
        fprintf(out, "    syscall\n");
    } else {
        fprintf(out, "    jr   $ra                 # return to caller\n");
    }
}

/* ==========================================================================
 * PART 4 — INSTRUCTION SELECTION
 * ========================================================================*/

/* MIPS mnemonic for each arithmetic / relational / logical TAC opcode.
 * SPIM accepts the three-operand pseudo-instructions used here (mul, div,
 * slt, sgt, sle, sge, seq, sne), expanding them to real instructions. */
static const char* mnemonicFor(TACOp op) {
    switch (op) {
        /* addu and subu, not add and sub: the signed forms TRAP on overflow.
         * The optimizer folds constants with 32-bit wraparound, so wrapping
         * here too means optimized and unoptimized builds agree. */
        case TAC_ADD: return "addu";
        case TAC_SUB: return "subu";
        case TAC_MUL: return "mul";
        case TAC_DIV: return "div";
        case TAC_LT:  return "slt";
        case TAC_GT:  return "sgt";
        case TAC_LE:  return "sle";
        case TAC_GE:  return "sge";
        case TAC_EQ:  return "seq";
        case TAC_NE:  return "sne";
        default:      return NULL;
    }
}

/* The source operator, for the comments in the .s file */
static const char* symbolFor(TACOp op) {
    switch (op) {
        case TAC_ADD: return "+";
        case TAC_SUB: return "-";
        case TAC_MUL: return "*";
        case TAC_DIV: return "/";
        case TAC_LT:  return "<";
        case TAC_GT:  return ">";
        case TAC_LE:  return "<=";
        case TAC_GE:  return ">=";
        case TAC_EQ:  return "==";
        case TAC_NE:  return "!=";
        default:      return "?";
    }
}

/* Emit the .data section: one entry per global declaration. */
static void emitDataSection(TACInstr* head) {
    initGlobalScope();
    fprintf(out, "# ============ CST-405 generated MIPS ============\n");
    fprintf(out, ".data\n");
    /* The newline string is emitted first and everything after it is word
     * aligned explicitly: .asciiz leaves the location counter on an odd byte,
     * and `lw`/`sw` on a misaligned address raises an address-error exception
     * in SPIM.  Forgetting .align here is a classic first bug. */
    fprintf(out, "__nl: .asciiz \"\\n\"\n");

    int inFunction = 0;
    for (TACInstr* c = head; c; c = c->next) {
        if (c->op == TAC_FUNC_BEGIN) { inFunction = 1; continue; }
        if (c->op == TAC_FUNC_END)   { inFunction = 0; continue; }
        if (inFunction) continue;                 /* only file-scope declarations */

        if (c->op == TAC_DECL) {
            addGlobalVar(c->result, c->arg1 ? c->arg1 : "int");
            fprintf(out, ".align 2\n%s: .word 0            # int %s\n",
                    dataLabel(c->result), c->result);
        } else if (c->op == TAC_ARRAY_DECL) {
            int n = atoi(c->arg1);
            addGlobalArray(c->result, n);
            /* Limitation: SPIM's default data segment is 64 KB, and an element
             * beyond that faults at run time, in hand-written assembly too.
             * Local arrays have no such limit: the stack grows on demand. */
            fprintf(out, ".align 2\n%s: .space %d          # int %s[%d]\n",
                    dataLabel(c->result), n * 4, c->result, n);
        }
    }
    fprintf(out, "\n.text\n.globl main\n");
}

/* ==========================================================================
 * PART 5 — PEEPHOLE PASS AND OUTPUT
 * --------------------------------------------------------------------------
 * Instruction selection writes into a temporary file rather than straight
 * into the .s file.  When it is finished the text is read back as an array
 * of lines, handed to peepholeOptimize() (peephole.c), and only then written
 * to the real output.  Keeping the two steps apart means instruction
 * selection can stay simple — one TAC instruction at a time — and leave the
 * clean-up of redundancies across instruction boundaries to the peephole.
 * ========================================================================*/
int peepholeEnabled = 1;          /* cleared by the -nopeep flag in main.c   */
static PeepStats peepStats;

PeepStats getPeepStats(void) { return peepStats; }

static void peepholeAndWrite(FILE* tmp, const char* filename) {
    /* read the generated assembly back, one malloc'd string per line */
    int cap = 256, n = 0;
    char** lines = malloc(cap * sizeof *lines);
    char buf[512];
    rewind(tmp);
    while (fgets(buf, sizeof buf, tmp)) {
        buf[strcspn(buf, "\n")] = '\0';
        if (n == cap) { cap *= 2; lines = realloc(lines, cap * sizeof *lines); }
        lines[n++] = strdup(buf);
    }
    fclose(tmp);

    memset(&peepStats, 0, sizeof peepStats);
    if (peepholeEnabled) {
        n = peepholeOptimize(lines, n, &peepStats);
        trace("\n  Peephole optimization (%d sweep(s))\n", peepStats.passes);
        trace("    self moves removed        %4d\n", peepStats.selfMove);
        trace("    store->load forwarded     %4d\n", peepStats.storeLoad);
        trace("    load->store removed       %4d\n", peepStats.loadStore);
        trace("    dead register writes      %4d\n", peepStats.deadWrite);
        trace("    jumps to next removed     %4d\n", peepStats.jumpToNext);
        trace("    jumps threaded            %4d\n", peepStats.jumpThread);
        trace("    MIPS instructions  %d -> %d\n", peepStats.linesBefore, peepStats.linesAfter);
    } else {
        peepStats.linesBefore = peepStats.linesAfter = countInstructions(lines, n);
        trace("\n  Peephole optimization disabled (-nopeep)\n");
    }

    FILE* f = fopen(filename, "w");
    if (!f) { fprintf(stderr, "Cannot open output file %s\n", filename); exit(1); }
    for (int i = 0; i < n; i++) { fprintf(f, "%s\n", lines[i]); free(lines[i]); }
    free(lines);
    fclose(f);
}

void generateMIPSFromTAC(const char* filename) {
    /* everything below writes to `out`, which is a scratch file until the
     * peephole pass has run (see PART 5) */
    out = tmpfile();
    if (!out) { fprintf(stderr, "Cannot create a temporary file for %s\n", filename); exit(1); }

    regAlloc.spillCount = 0;
    regAlloc.loadCount  = 0;
    initRegAlloc();

    TACList* tac = getOptimizedTAC();
    if (!tac || !tac->head) {
        fprintf(stderr, "Error: no TAC to translate\n");
        fclose(out);
        return;
    }

    /* Pass 1: globals -> .data */
    emitDataSection(tac->head);

    /* Pass 2: one function at a time */
    for (TACInstr* c = tac->head; c; c = c->next) {

        if (c->op != TAC_FUNC_BEGIN) continue;   /* file-scope decls already done */

        currentFunc = c->result;
        frameSize   = layoutFrame(c);
        trace("\n  Activation record for '%s': %d bytes\n", currentFunc, frameSize);
        printSymTab();
        initRegAlloc();

        emitPrologue(currentFunc, frameSize);

        int paramIndex = 0;     /* Which $a register the next PARAM arrived in */
        int argIndex   = 0;     /* Which $a register the next ARG goes into    */

        for (TACInstr* i = c->next; i && i->op != TAC_FUNC_END; i = i->next) {
            instrStart = regAlloc.timestamp;

            switch (i->op) {

                case TAC_PARAM: {
                    /* Copy the argument into its frame slot straight away:
                     * $a0-$a3 are needed again the moment this function
                     * prints or calls anything. */
                    Symbol* s = lookupSymbol(i->result);
                    char reg[8], note[MAX_VAR_NAME + 16];
                    snprintf(reg, sizeof reg, "$a%d", paramIndex++);
                    snprintf(note, sizeof note, "parameter %s", i->result);
                    emitFrameAccess("sw", reg, s->offset, note);
                    break;
                }

                case TAC_ARRAY_DECL: {
                    /* No instruction: layoutFrame() reserved the elements. */
                    Symbol* s = lookupSymbol(i->result);
                    fprintf(out, "    # int %s[%s] lives at %d($sp)\n",
                            i->result, i->arg1, s ? s->offset : -1);
                    break;
                }

                case TAC_ARRAY_LOAD: {
                    int addr = elementAddrReg(i->arg1, i->arg2);
                    int d    = defReg(i->result);
                    fprintf(out, "    lw   $t%d, 0($t%d)        # %s = %s[%s]\n",
                            d, addr, i->result, i->arg1, i->arg2);
                    releaseReg(addr);
                    break;
                }

                case TAC_ARRAY_STORE: {
                    /* The value is in `result`, the one field here that is
                     * read rather than written. */
                    int v    = operandReg(i->result);
                    int addr = elementAddrReg(i->arg1, i->arg2);
                    fprintf(out, "    sw   $t%d, 0($t%d)        # %s[%s] = %s\n",
                            v, addr, i->arg1, i->arg2, i->result);
                    releaseReg(addr);
                    break;
                }

                case TAC_ARG: {
                    /* Fill $a0-$a3 while the $t registers are still valid.
                     * The flush happens at the CALL, after all of these.
                     * Doing it first would forget where the values are.
                     * An array argument is its base ADDRESS, never its
                     * contents.  An array parameter passed along already
                     * holds an address, so it is an ordinary value here. */
                    Symbol* s = isConstant(i->arg1) ? NULL : lookupSymbol(i->arg1);
                    char reg[8];
                    snprintf(reg, sizeof reg, "$a%d", argIndex++);
                    if (s && s->isArray && !s->isParamArray) {
                        emitArrayBase(reg, s);
                    } else if (isConstant(i->arg1)) {
                        fprintf(out, "    li   %s, %s\n", reg, i->arg1);
                    } else {
                        fprintf(out, "    move %s, $t%d          # argument %s\n",
                                reg, ensureReg(i->arg1), i->arg1);
                    }
                    break;
                }

                case TAC_CALL: {
                    /* $t0-$t9 are caller-saved: the callee may destroy every
                     * one of them, including in its own recursive calls.  So
                     * everything live goes back to THIS frame before the jal,
                     * and the cache starts empty afterwards. */
                    flushRegisters("before call");
                    fprintf(out, "    jal  %s\n", funcLabel(i->arg1));
                    argIndex = 0;
                    if (i->result) {
                        int d = defReg(i->result);
                        fprintf(out, "    move $t%d, $v0          # %s = return value\n",
                                d, i->result);
                    }
                    break;
                }

                case TAC_LABEL:
                    /* Control can arrive here from elsewhere, with a different
                     * idea of what the registers hold.  Memory is the one
                     * thing every path agrees on. */
                    flushAtBranch("before label", i->next);
                    fprintf(out, "%s:\n", i->result);
                    break;

                case TAC_GOTO:
                    flushAtBranch("before jump", i->next);
                    fprintf(out, "    j    %s\n", i->arg1);
                    break;

                case TAC_IF_FALSE:
                case TAC_IF_TRUE: {
                    /* Read the condition first, flush second: flushing frees
                     * the cache entry but leaves the value in the register. */
                    int a = operandReg(i->arg1);
                    flushAtBranch("before branch", i->next);
                    fprintf(out, "    %s $t%d, %s\n",
                            i->op == TAC_IF_FALSE ? "beqz" : "bnez", a, i->arg2);
                    break;
                }

                case TAC_NEG: {
                    int a = operandReg(i->arg1);
                    int d = defReg(i->result);
                    fprintf(out, "    subu $t%d, $zero, $t%d      # %s = -%s\n",
                            d, a, i->result, i->arg1);
                    break;
                }

                case TAC_DECL: {
                    /* No instruction: layoutFrame() already reserved the slot.
                     * The comment makes the generated assembly readable:
                     * every variable's stack slot is named where it is declared. */
                    Symbol* s = lookupSymbol(i->result);
                    fprintf(out, "    # %s lives at %d($sp)\n",
                            i->result, s ? s->offset : -1);
                    break;
                }

                case TAC_ASSIGN: {
                    if (isConstant(i->arg1)) {
                        /* x = 5  ->  ONE instruction, straight into x's register.
                         * (Going through a scratch register first costs a second
                         * instruction and a register for nothing.) */
                        int d = defReg(i->result);
                        fprintf(out, "    li   $t%d, %s              # %s = %s\n",
                                d, i->arg1, i->result, i->arg1);
                    } else {
                        int a = operandReg(i->arg1);
                        int d = defReg(i->result);
                        fprintf(out, "    move $t%d, $t%d          # %s = %s\n",
                                d, a, i->result, i->arg1);
                    }
                    break;
                }

                case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV:
                case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
                case TAC_EQ:  case TAC_NE: {
                    int a = operandReg(i->arg1);
                    int b = operandReg(i->arg2);
                    int d = defReg(i->result);
                    /* the comment repeats the TAC instruction, with the
                     * source operator, so the .s file reads against the .tac */
                    fprintf(out, "    %-4s $t%d, $t%d, $t%d      # %s = %s %s %s\n",
                            mnemonicFor(i->op), d, a, b,
                            i->result, i->arg1, symbolFor(i->op), i->arg2);
                    break;
                }

                case TAC_PRINT: {
                    if (isConstant(i->arg1)) {
                        fprintf(out, "    li   $a0, %s              # print %s\n", i->arg1, i->arg1);
                    } else {
                        int a = operandReg(i->arg1);
                        fprintf(out, "    move $a0, $t%d          # print %s\n", a, i->arg1);
                    }
                    fprintf(out, "    li   $v0, 1               # syscall 1 = print integer\n");
                    fprintf(out, "    syscall\n");
                    fprintf(out, "    la   $a0, __nl\n");
                    fprintf(out, "    li   $v0, 4               # syscall 4 = print string\n");
                    fprintf(out, "    syscall\n");
                    break;
                }

                case TAC_RETURN: {
                    if (i->arg1) {
                        if (isConstant(i->arg1)) {
                            fprintf(out, "    li   $v0, %s              # return %s\n",
                                    i->arg1, i->arg1);
                        } else {
                            int a = operandReg(i->arg1);
                            fprintf(out, "    move $v0, $t%d          # return %s\n", a, i->arg1);
                        }
                    }
                    /* Locals die with the frame the epilogue is about to discard,
                     * so writing them back would be a store nobody ever reads.
                     * Only GLOBALS outlive the function and must reach memory. */
                    for (int r = 0; r < NUM_TEMP_REGS; r++) {
                        if (regAlloc.regs[r].inUse && regAlloc.regs[r].isDirty &&
                            regAlloc.regs[r].hasHome) {
                            Symbol* s = lookupSymbol(regAlloc.regs[r].varName);
                            if (s && !s->isGlobal) regAlloc.regs[r].isDirty = 0;
                        }
                    }
                    flushRegisters("globals before return");
                    /* Falling through is correct when this is the last
                     * instruction: the epilogue label is the very next line. */
                    if (i->next && i->next->op != TAC_FUNC_END) {
                        /* A return in the middle of a function must skip what
                         * follows, and the epilogue is the only place that
                         * restores $ra and pops the frame. */
                        fprintf(out, "    j    %s__epilogue\n", funcLabel(currentFunc));
                    }
                    break;
                }

                default:
                    /* Logical operators are not part of this language yet. */
                    fprintf(out, "    # TAC opcode %d not yet lowered (later topic)\n", i->op);
                    break;
            }
        }

        flushRegisters("end of function body");
        emitEpilogue(currentFunc, frameSize);
    }

    peepholeAndWrite(out, filename);   /* closes the scratch file */
    out = NULL;
}
