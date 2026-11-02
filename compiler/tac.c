/* =========================================================================
 * CST-405  ·  Project 3  ·  Complex Variables and Functions
 * FILE: tac.c   —   Phases 4 & 5 — Intermediate code and optimization
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                                           ^^^  this file
 *
 * PHASE
 *   Phases 4 and 5, intermediate code and optimization.
 *
 * RECEIVES
 *   Phase 4 receives the checked AST.  Phase 5 receives Phase 4's TAC.
 *
 * PRODUCES
 *   Three-address code (<output>.tac) and optimized TAC (<output>.optimized.tac).
 *   The optimized list is what Phase 6 translates.
 *
 * CREDITS
 *   Temporary allocation and list handling from the CST-405 course starter code.
 *   AST -> TAC translation and the optimizer: Alissa Head.
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tac.h"
#include "trace.h"

TACList tacList;
TACList optimizedList;
TempAllocator tempAlloc;

void initTAC() {
    tacList.head = NULL;
    tacList.tail = NULL;
    tacList.tempCount = 0;
    tacList.labelCount = 0;
    optimizedList.head = NULL;
    optimizedList.tail = NULL;

    /* Initialize temporary allocator */
    for (int i = 0; i < MAX_TEMPS; i++) {
        tempAlloc.allocated[i] = 0;
    }
    tempAlloc.maxUsed = 0;
    tempAlloc.freeCount = 0;
    trace("TAC: Temporary allocator initialized\n");
}

char* newTemp() {
    char* temp = malloc(10);
    sprintf(temp, "t%d", tacList.tempCount++);
    return temp;
}

char* allocTemp() {
    int tempNum;

    if (tempAlloc.freeCount > 0) {
        tempNum = tempAlloc.freeList[--tempAlloc.freeCount];
        trace("    [TAC ALLOC] Reusing temporary t%d\n", tempNum);
    } else {
        tempNum = tempAlloc.maxUsed++;
        if (tempNum >= MAX_TEMPS) {
            fprintf(stderr, "ERROR: Exceeded maximum temporaries\n");
            exit(1);
        }
        trace("    [TAC ALLOC] Allocating new temporary t%d\n", tempNum);
    }

    tempAlloc.allocated[tempNum] = 1;
    char* temp = malloc(10);
    sprintf(temp, "t%d", tempNum);
    return temp;
}

/* A fresh label name (L0, L1, ...).  Labels are global in the assembler, so
 * the counter runs across the whole program, not per function. */
char* newLabel() {
    char* label = malloc(16);
    sprintf(label, "L%d", tacList.labelCount++);
    return label;
}

void freeTemp(char* temp) {
    /* Callers pass any operand, so this must test the WHOLE name.  Checking
     * only the first letter would treat the variable `total` as temporary
     * number atoi("otal") == 0, and free t0 while it was still in use. */
    if (!temp || temp[0] != 't' || !temp[1]) return;
    for (int i = 1; temp[i]; i++)
        if (temp[i] < '0' || temp[i] > '9') return;

    int tempNum = atoi(temp + 1);
    if (!tempAlloc.allocated[tempNum]) return;

    tempAlloc.allocated[tempNum] = 0;
    if (tempAlloc.freeCount < MAX_TEMPS) {
        tempAlloc.freeList[tempAlloc.freeCount++] = tempNum;
        trace("    [TAC FREE] Released temporary %s\n", temp);
    }
}


void printTempAllocatorState() {
    trace("\n┌──────────────────────────────────────────────────────────┐\n");
    trace("│ TEMPORARY ALLOCATOR STATISTICS                           │\n");
    trace("├──────────────────────────────────────────────────────────┤\n");
    trace("│ Total temporaries used:      %3d                         │\n", tempAlloc.maxUsed);
    trace("│ Currently allocated:         %3d                         │\n",
           tempAlloc.maxUsed - tempAlloc.freeCount);
    trace("│ Available for reuse:         %3d                         │\n", tempAlloc.freeCount);
    trace("└──────────────────────────────────────────────────────────┘\n\n");
}

TACInstr* createTAC(TACOp op, char* arg1, char* arg2, char* result) {
    TACInstr* instr = malloc(sizeof(TACInstr));
    instr->op = op;
    instr->arg1 = arg1 ? strdup(arg1) : NULL;
    instr->arg2 = arg2 ? strdup(arg2) : NULL;
    instr->result = result ? strdup(result) : NULL;
    instr->next = NULL;
    return instr;
}

void appendTAC(TACInstr* instr) {
    if (!tacList.head) {
        tacList.head = tacList.tail = instr;
    } else {
        tacList.tail->next = instr;
        tacList.tail = instr;
    }
}

void appendOptimizedTAC(TACInstr* instr) {
    if (!optimizedList.head) {
        optimizedList.head = optimizedList.tail = instr;
    } else {
        optimizedList.tail->next = instr;
        optimizedList.tail = instr;
    }
}

/* ── BREAK LABEL STACK ──────────────────────────────────────────────────────
 * When entering a switch, for, or while, push the "end" label so that any
 * nested NODE_BREAK can emit GOTO to the right exit point.
 */

/* Forward declarations */
static void generateTACStmt(ASTNode* node);

/* More than four is rejected in semantic.c, so this only bounds the array. */
#define MAX_CALL_ARGS 32


/* Map an AST operator character to its TAC opcode. */
static TACOp binOpToTAC(char op) {
    switch (op) {
        case '+': return TAC_ADD;
        case '-': return TAC_SUB;
        case '*': return TAC_MUL;
        case '/': return TAC_DIV;
        case '<': return TAC_LT;
        case '>': return TAC_GT;
        case 'L': return TAC_LE;
        case 'G': return TAC_GE;
        case 'E': return TAC_EQ;
        case 'N': return TAC_NE;
        default:
            fprintf(stderr, "TAC ERROR: unknown binary operator '%c'\n", op);
            return TAC_ADD;
    }
}

/* Generate TAC for a call, leaving the value in `result` (NULL to discard it).
 *
 * Every argument is evaluated FIRST, and only then are the ARG instructions
 * emitted, in one unbroken run right before the CALL.  That is what makes
 * f(1, g(2)) correct: g's whole call finishes while no ARG of f has been
 * issued yet, so the back end never has to keep track of which pending
 * arguments belong to which call.  The alternative, an ARG after each
 * argument, needs a stack of pending arguments in the code generator. */
static void generateCall(ASTNode* node, char* result) {
    char* values[MAX_CALL_ARGS];
    int count = 0;

    for (ASTNode* a = node->data.call.args; a; a = a->data.list.next) {
        char* value = generateTACExpr(a->data.list.item);
        if (count < MAX_CALL_ARGS) values[count++] = value;
    }
    for (int i = 0; i < count; i++) {
        appendTAC(createTAC(TAC_ARG, values[i], NULL, NULL));
        freeTemp(values[i]);
        free(values[i]);
    }

    char countText[16];
    snprintf(countText, sizeof countText, "%d", count);
    appendTAC(createTAC(TAC_CALL, node->data.call.name, countText, result));
}

/* Generate TAC for expression.  Returns the NAME of the location holding the
 * result: a temporary, a variable, or the text of a literal.  The caller owns
 * the returned string, and freeTemp()s it once the value has been used. */
char* generateTACExpr(ASTNode* node) {
    if (!node) return NULL;

    switch (node->type) {
        case NODE_NUM: {
            /* The literal itself, as text — TAC operands are always strings,
             * so a constant is just a string that happens to look like one. */
            char buf[24];
            snprintf(buf, sizeof buf, "%d", node->data.num);
            return strdup(buf);
        }

        case NODE_VAR:
            /* The caller gets a copy it owns and must free; the AST node
             * keeps its own copy of the name. */
            return strdup(node->data.name);

        case NODE_BINOP: {
            /* Unary minus has no right operand, and its own opcode. */
            if (node->data.binop.op == 'u') {
                char* operand = generateTACExpr(node->data.binop.left);
                char* t       = allocTemp();
                appendTAC(createTAC(TAC_NEG, operand, NULL, t));
                freeTemp(operand);
                free(operand);
                return t;
            }

            /* Limitation: a variable operand is returned by NAME and read when
             * the operation runs, so in `g + f()` a global g is read AFTER the
             * call.  C leaves this order unspecified; a program can only
             * tell if f assigns g. */
            char* left  = generateTACExpr(node->data.binop.left);
            char* right = generateTACExpr(node->data.binop.right);
            char* t     = allocTemp();

            appendTAC(createTAC(binOpToTAC(node->data.binop.op), left, right, t));

            /* Free the operand temporaries AFTER emitting, never before. */
            freeTemp(left);
            freeTemp(right);
            free(left);
            free(right);
            return t;
        }

        case NODE_ARRAY_INDEX: {
            /* a[i] becomes one instruction.  The address arithmetic is not
             * written out here because TAC has no idea what a word is; the
             * width of an element is the back end's knowledge. */
            char* index = generateTACExpr(node->data.arrayindex.index);
            char* t     = allocTemp();
            appendTAC(createTAC(TAC_ARRAY_LOAD, node->data.arrayindex.name, index, t));
            freeTemp(index);
            free(index);
            return t;
        }

        case NODE_FUNC_CALL: {
            char* t = allocTemp();
            generateCall(node, t);
            return t;
        }

        default:
            fprintf(stderr, "TAC ERROR: node type %d is not an expression\n", node->type);
            return strdup("0");
    }
}

/* Generate TAC for statement list */
static void generateTACStmtList(ASTNode* node) {
    if (!node) return;

    if (node->type == NODE_STMT_LIST) {
        generateTACStmt(node->data.stmtlist.stmt);
        generateTACStmtList(node->data.stmtlist.next);
    } else {
        generateTACStmt(node);
    }
}

/* Generate TAC for a statement */
static void generateTACStmt(ASTNode* node) {
    if (!node) return;

    switch (node->type) {
        case NODE_DECL:
            /* No code runs, but the back end needs to know the variable
             * exists so layoutFrame() can reserve a slot for it. */
            appendTAC(createTAC(TAC_DECL, node->data.decl.varType, NULL,
                                 node->data.decl.name));
            break;

        case NODE_ARRAY_DECL: {
            char size[16];
            snprintf(size, sizeof size, "%d", node->data.arraydecl.size);
            appendTAC(createTAC(TAC_ARRAY_DECL, size, NULL, node->data.arraydecl.name));
            break;
        }

        case NODE_ASSIGN: {
            /* Two shapes.  For a[i] = v the index is evaluated first, then
             * the value, which is the order they appear in the source. */
            ASTNode* target = node->data.assign.arrayLHS;
            if (target) {
                char* index = generateTACExpr(target->data.arrayindex.index);
                char* value = generateTACExpr(node->data.assign.value);
                appendTAC(createTAC(TAC_ARRAY_STORE, target->data.arrayindex.name,
                                    index, value));
                freeTemp(index);
                freeTemp(value);
                free(index);
                free(value);
            } else {
                char* value = generateTACExpr(node->data.assign.value);
                appendTAC(createTAC(TAC_ASSIGN, value, NULL, node->data.assign.var));
                freeTemp(value);
                free(value);
            }
            break;
        }

        case NODE_FUNC_CALL:
            generateCall(node, NULL);      /* a statement discards the value */
            break;

        case NODE_RETURN: {
            /* Emitting RETURN is the easy half.  Making it actually LEAVE the
             * function, skipping whatever follows, is codegen.c's job. */
            char* value = node->data.ret ? generateTACExpr(node->data.ret) : NULL;
            appendTAC(createTAC(TAC_RETURN, value, NULL, NULL));
            freeTemp(value);
            free(value);
            break;
        }

        case NODE_BLOCK:
            /* The scope this opened mattered to semantic analysis and is
             * finished with by now: names were already made unique there. */
            generateTACStmtList(node->data.block);
            break;

        case NODE_IF: {
            /*      if_false c goto Lelse
             *      ...then...
             *      goto Lend            (only when there is an else)
             *  Lelse:
             *      ...else...
             *  Lend:                                                        */
            char* cond      = generateTACExpr(node->data.ifstmt.cond);
            char* elseLabel = newLabel();
            appendTAC(createTAC(TAC_IF_FALSE, cond, elseLabel, NULL));
            freeTemp(cond);
            free(cond);

            generateTACStmt(node->data.ifstmt.thenStmt);

            if (node->data.ifstmt.elseStmt) {
                char* endLabel = newLabel();
                appendTAC(createTAC(TAC_GOTO, endLabel, NULL, NULL));
                appendTAC(createTAC(TAC_LABEL, NULL, NULL, elseLabel));
                generateTACStmt(node->data.ifstmt.elseStmt);
                appendTAC(createTAC(TAC_LABEL, NULL, NULL, endLabel));
                free(endLabel);
            } else {
                appendTAC(createTAC(TAC_LABEL, NULL, NULL, elseLabel));
            }
            free(elseLabel);
            break;
        }

        case NODE_PRINT: {
            char* value = generateTACExpr(node->data.expr);
            appendTAC(createTAC(TAC_PRINT, value, NULL, NULL));
            freeTemp(value);
            free(value);
            break;
        }

        case NODE_STMT_LIST:
            generateTACStmtList(node);
            break;

        default:
            fprintf(stderr, "TAC ERROR: node type %d is not a statement\n", node->type);
            break;
    }
}

/* A function becomes  FUNC_BEGIN f ; PARAM ... ; body ; RETURN ; FUNC_END f.
 * An array parameter is marked "int[]", which tells the code generator its
 * slot holds an address rather than a value.
 *
 * The RETURN added at the end covers a body that runs off its closing brace.
 * When the body already ends in a return, it is unreachable and the optimizer
 * removes it. */
static void generateTACFuncDef(ASTNode* node) {
    appendTAC(createTAC(TAC_FUNC_BEGIN, NULL, NULL, node->data.funcdef.name));

    for (ASTNode* p = node->data.funcdef.params; p; p = p->data.list.next) {
        ASTNode* param = p->data.list.item;
        appendTAC(createTAC(TAC_PARAM, param->data.param.isArray ? "int[]" : "int",
                            NULL, param->data.param.name));
    }

    generateTACStmtList(node->data.funcdef.body);

    appendTAC(createTAC(TAC_RETURN, node->data.funcdef.isVoid ? NULL : "0", NULL, NULL));
    appendTAC(createTAC(TAC_FUNC_END, NULL, NULL, node->data.funcdef.name));
}

/* Walk the top level of a program that defines functions.  Declarations here
 * are globals: they are emitted before any FUNC_BEGIN, which is how the code
 * generator tells them apart from locals. */
static void generateTACList(ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_STMT_LIST) {
        generateTACList(node->data.stmtlist.stmt);
        generateTACList(node->data.stmtlist.next);
    } else if (node->type == NODE_FUNC_DEF) {
        generateTACFuncDef(node);
    } else {
        generateTACStmt(node);
    }
}

/* Does the program define at least one function? */
static int definesFunction(ASTNode* node) {
    if (!node) return 0;
    if (node->type == NODE_FUNC_DEF) return 1;
    if (node->type == NODE_STMT_LIST)
        return definesFunction(node->data.stmtlist.stmt) ||
               definesFunction(node->data.stmtlist.next);
    return 0;
}

void generateTAC(ASTNode* node) {
    if (!node) return;

    if (definesFunction(node)) {
        generateTACList(node);
        return;
    }

    /* A program with no functions is a Project 2 program, and the code
     * generator still emits code into MIPS functions: every program needs an
     * entry point called `main`.  So the whole statement list becomes the body
     * of an implicit main(). */
    appendTAC(createTAC(TAC_FUNC_BEGIN, NULL, NULL, "main"));
    generateTACStmt(node);
    appendTAC(createTAC(TAC_RETURN, "0", NULL, NULL));
    appendTAC(createTAC(TAC_FUNC_END, NULL, NULL, "main"));
}

/* Get operation name for printing */
static const char* getOpName(TACOp op) {
    switch(op) {
        case TAC_ADD: return "+";
        case TAC_SUB: return "-";
        case TAC_MUL: return "*";
        case TAC_DIV: return "/";
        case TAC_NEG: return "NEG";
        case TAC_LT: return "<";
        case TAC_GT: return ">";
        case TAC_LE: return "<=";
        case TAC_GE: return ">=";
        case TAC_EQ: return "==";
        case TAC_NE: return "!=";
        case TAC_AND: return "&&";
        case TAC_OR: return "||";
        case TAC_NOT: return "!";
        default: return "?";
    }
}


/* ==========================================================================
 * FORMATTING
 * --------------------------------------------------------------------------
 * One function renders a TAC instruction as text.  Everything that displays
 * or saves three-address code goes through it, so the listing you read on
 * screen and the listing saved to the .tac file can never drift apart.
 * ========================================================================*/
void formatTAC(const TACInstr* i, char* buf, size_t n) {
    if (!i) { snprintf(buf, n, "(null)"); return; }
    switch (i->op) {
        case TAC_FUNC_BEGIN: snprintf(buf, n, "FUNC_BEGIN %s", i->result); break;
        case TAC_FUNC_END:   snprintf(buf, n, "FUNC_END %s", i->result); break;
        case TAC_PARAM:      snprintf(buf, n, "PARAM %s %s",
                                      i->arg1 ? i->arg1 : "int", i->result); break;
        case TAC_DECL:       snprintf(buf, n, "DECL %s %s",
                                      i->arg1 ? i->arg1 : "int", i->result); break;

        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR:
            snprintf(buf, n, "%s = %s %s %s", i->result, i->arg1,
                     getOpName(i->op), i->arg2); break;

        case TAC_NEG:        snprintf(buf, n, "%s = -%s", i->result, i->arg1); break;
        case TAC_NOT:        snprintf(buf, n, "%s = !%s", i->result, i->arg1); break;
        case TAC_ASSIGN:     snprintf(buf, n, "%s = %s", i->result, i->arg1); break;
        case TAC_PRINT:      snprintf(buf, n, "PRINT %s", i->arg1); break;
        case TAC_ARG:        snprintf(buf, n, "ARG %s", i->arg1); break;
        case TAC_CALL:       if (i->result) snprintf(buf, n, "%s = CALL %s, %s",
                                                     i->result, i->arg1, i->arg2);
                             else           snprintf(buf, n, "CALL %s, %s", i->arg1, i->arg2);
                             break;
        case TAC_RETURN:     if (i->arg1) snprintf(buf, n, "RETURN %s", i->arg1);
                             else         snprintf(buf, n, "RETURN");
                             break;
        case TAC_LABEL:      snprintf(buf, n, "%s:", i->result); break;
        case TAC_GOTO:       snprintf(buf, n, "GOTO %s", i->arg1); break;
        case TAC_IF_FALSE:   snprintf(buf, n, "IF_FALSE %s GOTO %s", i->arg1, i->arg2); break;
        case TAC_IF_TRUE:    snprintf(buf, n, "IF_TRUE %s GOTO %s", i->arg1, i->arg2); break;
        case TAC_ARRAY_DECL: snprintf(buf, n, "ARRAY_DECL %s[%s]", i->result, i->arg1); break;
        case TAC_ARRAY_LOAD: snprintf(buf, n, "%s = %s[%s]", i->result, i->arg1, i->arg2); break;
        case TAC_ARRAY_STORE:snprintf(buf, n, "%s[%s] = %s", i->arg1, i->arg2, i->result); break;
        default:             snprintf(buf, n, "UNKNOWN"); break;
    }
}

/* Count instructions in a list — the crudest possible code-size metric, and
 * the one Topic 4 uses to quantify what optimization bought us. */
int countTAC(const TACList* list) {
    int n = 0;
    for (TACInstr* c = list->head; c; c = c->next) n++;
    return n;
}

static void dumpList(const TACList* list, const char* title) {
    char buf[1024];
    trace("%s\n", title);
    trace("─────────────────────────────────────────────\n");
    int line = 1;
    for (TACInstr* c = list->head; c; c = c->next) {
        formatTAC(c, buf, sizeof buf);
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END)
            trace("%3d: %s\n", line++, buf);
        else
            trace("%3d:     %s\n", line++, buf);
    }
    trace("─────────────────────────────────────────────\n");
    trace("     %d instructions\n\n", countTAC(list));
}

void printTAC(void)          { dumpList(&tacList,       "UNOPTIMIZED THREE-ADDRESS CODE"); }
void printOptimizedTAC(void) { dumpList(&optimizedList, "OPTIMIZED THREE-ADDRESS CODE"); }

TACList* getOptimizedTAC(void) { return &optimizedList; }
TACList* getUnoptimizedTAC(void) { return &tacList; }

static void saveList(const TACList* list, const char* filename, const char* banner) {
    FILE* f = fopen(filename, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", filename); return; }
    char buf[1024];
    fprintf(f, "; %s\n", banner);
    fprintf(f, "; Generated by the CST-405 mini compiler\n;\n");
    int line = 1;
    for (TACInstr* c = list->head; c; c = c->next) {
        formatTAC(c, buf, sizeof buf);
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END)
            fprintf(f, "%3d: %s\n", line++, buf);
        else
            fprintf(f, "%3d:     %s\n", line++, buf);
    }
    fprintf(f, ";\n; %d instructions\n", countTAC(list));
    fclose(f);
}

void saveTACToFile(const char* filename)          { saveList(&tacList,       filename, "Unoptimized three-address code"); }
void saveOptimizedTACToFile(const char* filename) { saveList(&optimizedList, filename, "Optimized three-address code"); }

/* ==========================================================================
 * PHASE 5 — OPTIMIZATION
 * --------------------------------------------------------------------------
 * The optimizer rewrites the TAC list into a shorter, cheaper list that
 * computes the same thing.  It runs the same set of transformations REPEATEDLY
 * until a pass changes nothing, because optimizations feed each other:
 *
 *      t0 = 2 * 3        constant folding  ->  t0 = 6
 *      x  = t0                                  x  = 6      (copy propagation)
 *      y  = x + 0                               y  = 6      (algebraic + folding)
 *      t0 = ...                                 (t0 now dead -> removed)
 *
 * No single pass finds all of that; three passes do.  Reaching a fixed point
 * is the standard way real optimizers are structured.
 *
 * Transformations implemented, in the order they are applied within a pass:
 *
 *   1. Algebraic simplification   x+0, x-0, x*1, x*0, x/1
 *   2. Constant folding           evaluate operations on two literals
 *   3. Constant propagation       replace a variable by a constant known to
 *                                 hold it at that point
 *   4. Copy propagation           replace t1 by x after "t1 = x"
 *   5. Dead code elimination      drop assignments to temps never read again
 *   6. Dead store elimination     the same for locals, in straight-line code
 *   --- flow optimization (flowPass, run after the value passes) ---
 *   7. Branch simplification      constant conditions become GOTO or nothing
 *   8. Jump threading             a jump to a jump goes straight to the end
 *   9. Jump-to-next removal       GOTO L immediately followed by L:
 *  10. Unreachable code removal   drop code between GOTO/RETURN and next label
 *  11. Unused label removal       a label nothing jumps to
 *
 * (Peephole optimization runs later, on the generated MIPS: see peephole.c.)
 *
 * SAFETY RULES every pass obeys (and any new pass must obey too):
 *   • A LABEL ends a basic block: forget everything known about values,
 *     because control can arrive here from anywhere.
 *   • A CALL may modify globals and arrays: forget facts about non-temporaries.
 *   • Never eliminate a store to an array or a global as "dead"; we do not
 *     track those precisely enough to prove it.
 * ========================================================================*/

#define MAX_FACTS 256

typedef struct {
    char* name;      /* The variable or temporary the fact is about */
    char* value;     /* The constant, or the name it is a copy of   */
    int   isConst;   /* 1 = value is a literal, 0 = value is a name */
} Fact;

static Fact  facts[MAX_FACTS];
static int   factCount = 0;
static int   changesThisPass = 0;

/* Optimization bookkeeping, reported to the user and used by Topic 4 to
 * quantify the gain from each technique. */
static OptStats optStats;

OptStats getOptStats(void) { return optStats; }

static void clearFacts(void) { factCount = 0; }

static void dropFactsAbout(const char* name) {
    for (int i = 0; i < factCount; i++) {
        if (strcmp(facts[i].name, name) == 0 ||
            (!facts[i].isConst && strcmp(facts[i].value, name) == 0)) {
            facts[i] = facts[--factCount];
            i--;
        }
    }
}

/* Is this name a compiler temporary (t0, t1, ...)? */
static int isTempText(const char* s) {
    if (!s || s[0] != 't' || !s[1]) return 0;
    for (int i = 1; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* A call can change any global, so only facts that involve nothing but
 * temporaries and literals survive it.  "t1 is a copy of g" must go too: it
 * is a fact ABOUT a temporary, but it says something about g. */
static void dropNonTempFacts(void) {
    for (int i = 0; i < factCount; i++) {
        int keep = isTempText(facts[i].name) &&
                   (facts[i].isConst || isTempText(facts[i].value));
        if (!keep) { facts[i] = facts[--factCount]; i--; }
    }
}

static void recordFact(const char* name, const char* value, int isConst) {
    dropFactsAbout(name);
    if (factCount >= MAX_FACTS) return;
    facts[factCount].name    = (char*)name;
    facts[factCount].value   = (char*)value;
    facts[factCount].isConst = isConst;
    factCount++;
}

static const char* lookupFact(const char* name, int wantConst) {
    if (!name) return NULL;
    for (int i = 0; i < factCount; i++)
        if (strcmp(facts[i].name, name) == 0 && (!wantConst || facts[i].isConst))
            return facts[i].value;
    return NULL;
}

/* True for opcodes that only compute a value into `result` and have no other
 * effect.  Only these are candidates for dead-code elimination: a CALL may
 * print or modify globals, a STORE writes memory, a branch changes control.
 * TAC_DIV is left out on purpose: an unused a / b can still trap on a zero
 * divisor, and deleting it would change what the program does. */
static int mnemonicIsPure(TACOp op) {
    switch (op) {
        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_NEG:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR: case TAC_NOT:
        case TAC_ARRAY_LOAD:
            return 1;
        default:
            return 0;
    }
}

/* Is arg1 / arg2 of this instruction a VALUE being read?  Constant and copy
 * propagation may only touch operands that are.  arg1 of a CALL is a function
 * name, arg1 of an ARRAY_LOAD is an array, arg2 of an IF is a label, and a
 * variable can legally share its spelling with any of those. */
static int arg1IsValue(TACOp op) {
    switch (op) {
        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV: case TAC_NEG:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR: case TAC_NOT:
        case TAC_ASSIGN: case TAC_PRINT: case TAC_ARG: case TAC_RETURN:
        case TAC_IF_FALSE: case TAC_IF_TRUE:
            return 1;
        default:
            return 0;
    }
}

static int arg2IsValue(TACOp op) {
    switch (op) {
        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR:
        case TAC_ARRAY_LOAD: case TAC_ARRAY_STORE:
            return 1;
        default:
            return 0;
    }
}

/* Is this operand a literal integer? */
static int isConstantNumber(const char* s) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Evaluate an operation whose operands are both literals. */
static char* foldConstants(TACOp op, const char* a1, const char* a2, int* folded) {
    *folded = 0;
    if (!isConstantNumber(a1) || !isConstantNumber(a2)) return NULL;
    long a = atol(a1), b = atol(a2), r;
    /* Compute in 32 bits, as the MIPS instruction would.  Folding in 64 could
     * produce a constant the program would never have seen at run time. */
    switch (op) {
        case TAC_ADD: r = a + b; break;
        case TAC_SUB: r = a - b; break;
        case TAC_MUL: r = a * b; break;
        case TAC_DIV: if (b == 0) return NULL;   /* leave division by zero
                                                  * alone: folding it would
                                                  * change a run-time trap
                                                  * into a compile-time one */
                      r = a / b; break;
        case TAC_LT:  r = a <  b; break;
        case TAC_GT:  r = a >  b; break;
        case TAC_LE:  r = a <= b; break;
        case TAC_GE:  r = a >= b; break;
        case TAC_EQ:  r = a == b; break;
        case TAC_NE:  r = a != b; break;
        case TAC_AND: r = (a != 0) && (b != 0); break;
        case TAC_OR:  r = (a != 0) || (b != 0); break;
        default: return NULL;
    }
    char* s = malloc(24);
    snprintf(s, 24, "%d", (int)r);
    *folded = 1;
    return s;
}

/* Does `name` get read anywhere at or after instruction `from`, up to the end
 * of the function?  Temporaries and locals are private to one activation, so
 * a later function that reuses the same spelling is a different location.
 * A conservative liveness test: it stops being conservative only for
 * compiler temporaries, which is exactly where dead code piles up. */
static int isReadLater(TACInstr* from, const char* name) {
    for (TACInstr* c = from; c && c->op != TAC_FUNC_END; c = c->next) {
        if (c->arg1 && strcmp(c->arg1, name) == 0) return 1;
        if (c->arg2 && strcmp(c->arg2, name) == 0) return 1;
        /* ARRAY_STORE reads its `result` field (the value being stored) */
        if (c->op == TAC_ARRAY_STORE && c->result && strcmp(c->result, name) == 0) return 1;
        if (c->op == TAC_RETURN && c->arg1 && strcmp(c->arg1, name) == 0) return 1;
    }
    return 0;
}

/* Copy a TAC list so a pass can rewrite it without destroying the original. */
static TACList copyList(const TACList* src) {
    TACList d = { NULL, NULL, src->tempCount, src->labelCount };
    for (TACInstr* c = src->head; c; c = c->next) {
        TACInstr* n = createTAC(c->op, c->arg1, c->arg2, c->result);
        if (!d.head) d.head = d.tail = n;
        else { d.tail->next = n; d.tail = n; }
    }
    return d;
}

/* Is `name` a compiler-generated temporary (t0, t1, ...)?  Only temporaries
 * are safe to erase as dead code: a real variable might still be read by
 * code this teaching compiler does not analyze (a future topic, a print we
 * folded away above it, and so on). */
static int isTempName(const char* s) {
    if (!s || s[0] != 't' || !s[1]) return 0;
    for (int i = 1; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Is `name` declared (DECL) inside the function that starts at `func`?
 * Only such locals are candidates for dead store elimination. */
static int isDeclaredLocal(TACInstr* func, const char* name) {
    for (TACInstr* c = func->next; c && c->op != TAC_FUNC_END; c = c->next)
        if (c->op == TAC_DECL && c->result && strcmp(c->result, name) == 0)
            return 1;
    return 0;
}

/* Does the function starting at `func` contain no labels (and therefore no
 * jump targets)?  In straight-line code "not read later in the listing" is
 * the same as "not read later at run time", which is what DSE relies on. */
static int functionIsStraightLine(TACInstr* func) {
    for (TACInstr* c = func->next; c && c->op != TAC_FUNC_END; c = c->next)
        if (c->op == TAC_LABEL) return 0;
    return 1;
}

/* Append helper for the passes below. */
static void pushInstr(TACList* l, TACInstr* src) {
    TACInstr* n = createTAC(src->op, src->arg1, src->arg2, src->result);
    if (!l->head) l->head = l->tail = n;
    else { l->tail->next = n; l->tail = n; }
}

/* The label a jump instruction targets, or NULL if `c` is not a jump. */
static char* jumpTarget(TACInstr* c) {
    if (c->op == TAC_GOTO) return c->arg1;
    if (c->op == TAC_IF_FALSE || c->op == TAC_IF_TRUE) return c->arg2;
    return NULL;
}

/* Find the LABEL instruction named `label` in list `l`. */
static TACInstr* findLabel(TACList* l, const char* label) {
    for (TACInstr* c = l->head; c; c = c->next)
        if (c->op == TAC_LABEL && strcmp(c->result, label) == 0) return c;
    return NULL;
}

/* Does any jump in list `l` target this label? */
static int labelIsUsed(TACList* l, const char* label) {
    for (TACInstr* c = l->head; c; c = c->next) {
        char* target = jumpTarget(c);
        if (target && strcmp(target, label) == 0) return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * FLOW OPTIMIZATION — one pass over `in`, producing a new list.
 *
 * These transformations work on the CONTROL FLOW of the program rather than
 * on the values it computes:
 *
 *   a. Branch simplification   IF_FALSE 0 GOTO L  ->  GOTO L
 *                              IF_FALSE 5 GOTO L  ->  (removed: never taken)
 *                              (and the mirror image for IF_TRUE).
 *                              The condition becomes a literal after
 *                              constant propagation, which is why this pass
 *                              runs after optimizePass() in every round.
 *   b. Jump threading          GOTO L1 ... L1: GOTO L2   ->   GOTO L2
 *                              (a jump to a jump goes straight to the end)
 *   c. Jump-to-next removal    GOTO L ; L:   ->   L:
 *   d. Unreachable code        everything after an unconditional GOTO or
 *                              RETURN up to the next LABEL can never run.
 *                              Declarations are kept: they produce no code
 *                              but layoutFrame() still needs them.
 *   e. Unused labels           a label no jump targets is removed.
 *
 * `if` produces real jumps for this pass to work on (see tests/t3_13), and
 * tests/unit/test_optimizer.c exercises every case directly, including
 * patterns no current source program can produce.
 * -----------------------------------------------------------------------*/
static TACList flowPass(TACList* in) {
    TACList out = { NULL, NULL, in->tempCount, in->labelCount };
    int unreachable = 0;     /* 1 while skipping code after GOTO/RETURN */

    for (TACInstr* c = in->head; c; c = c->next) {
        /* A label nothing jumps to does not start a new path, so it must not
         * make code reachable.  This is what finishes the job on `if (1) ...
         * else ...`: once the branch is simplified, the else label has no
         * jumps left, and removing it exposes the else code as unreachable. */
        if (c->op == TAC_LABEL && !labelIsUsed(in, c->result)) {
            optStats.deadLabel++;
            changesThisPass++;
            continue;
        }

        /* A label or the end of a function makes code reachable again. */
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_END || c->op == TAC_FUNC_BEGIN)
            unreachable = 0;

        if (unreachable) {
            if (c->op == TAC_DECL || c->op == TAC_ARRAY_DECL || c->op == TAC_PARAM) {
                pushInstr(&out, c);          /* storage, not code: keep it */
            } else {
                optStats.unreachable++;
                changesThisPass++;
            }
            continue;
        }

        TACInstr tmp = *c;                   /* the instruction we will emit */

        /* (a) branch simplification */
        if ((c->op == TAC_IF_FALSE || c->op == TAC_IF_TRUE) && isConstantNumber(c->arg1)) {
            int truth = atol(c->arg1) != 0;
            int taken = (c->op == TAC_IF_TRUE) ? truth : !truth;
            optStats.branch++;
            changesThisPass++;
            if (!taken) continue;            /* never jumps: drop it entirely */
            tmp.op = TAC_GOTO; tmp.arg1 = c->arg2; tmp.arg2 = NULL;   /* always jumps */
        }

        /* (b) jump threading — follow a chain of "L: GOTO L'" to its end.
         * A chain that loops back on itself (L1: GOTO L2 / L2: GOTO L1) is an
         * infinite loop in the source; it is left exactly as written. */
        char* target = jumpTarget(&tmp);
        if (target) {
            char* seen[8];
            int   hops = 0, cycle = 0;
            char* t = target;
            seen[hops++] = t;
            while (hops < 8) {
                TACInstr* lbl = findLabel(in, t);
                TACInstr* after = lbl ? lbl->next : NULL;
                while (after && after->op == TAC_LABEL) after = after->next;
                if (!after || after->op != TAC_GOTO) break;
                t = after->arg1;
                for (int k = 0; k < hops; k++)
                    if (strcmp(seen[k], t) == 0) cycle = 1;
                if (cycle) break;
                seen[hops++] = t;
            }
            if (!cycle && t != target) {
                optStats.jumpThread++;
                changesThisPass++;
                target = t;
                if (tmp.op == TAC_GOTO) tmp.arg1 = target; else tmp.arg2 = target;
            }
        }

        /* (c) jump to the very next instruction: the jump does nothing */
        if (target) {
            int isNext = 0;
            for (TACInstr* n = c->next; n && n->op == TAC_LABEL; n = n->next)
                if (strcmp(n->result, target) == 0) { isNext = 1; break; }
            if (isNext) {
                optStats.jumpToNext++;
                changesThisPass++;
                continue;
            }
        }

        pushInstr(&out, &tmp);

        /* (d) what follows an unconditional transfer is dead until a label */
        if (tmp.op == TAC_GOTO || tmp.op == TAC_RETURN) unreachable = 1;
    }
    return out;
}

/* -------------------------------------------------------------------------
 * One optimization pass over `in`, producing `out`.
 *
 * Two sub-passes, in the order the header comment promises:
 *   1. algebraic simplification, constant folding, and constant/copy
 *      propagation, in a single left-to-right walk (each can feed the next)
 *   2. dead code elimination over the result of (1), which needs the
 *      REWRITTEN list — an operand this pass just propagated away no longer
 *      counts as a use.
 * -----------------------------------------------------------------------*/
static TACList optimizePass(TACList* in) {
    TACList mid = { NULL, NULL, in->tempCount, in->labelCount };
    clearFacts();

    for (TACInstr* c = in->head; c; c = c->next) {
        /* THE RULE YOU MUST NOT BREAK: forget everything at a LABEL, because
         * control can arrive there from anywhere.  A CALL may change any
         * global, so only facts about temporaries survive one. */
        if (c->op == TAC_LABEL) clearFacts();
        if (c->op == TAC_CALL)  dropNonTempFacts();

        /* The same rule at a function boundary.  The next function is entered
         * by a call from anywhere, so nothing learned in the previous one is
         * true there, even about a global.  Without this, `g = v;` at the end
         * of one function makes the next one read a parameter it doesn't have. */
        if (c->op == TAC_FUNC_BEGIN) clearFacts();

        /* --- constant / copy propagation: substitute what is already known --- */
        const char* a1 = c->arg1;
        const char* a2 = c->arg2;
        const char* stored = c->result;    /* ARRAY_STORE keeps its VALUE here */
        if (c->op == TAC_ARRAY_STORE && !isConstantNumber(stored)) {
            const char* k = lookupFact(stored, 1);
            if (k) { stored = k; optStats.constProp++; changesThisPass++; }
        }
        if (a1 && arg1IsValue(c->op) && !isConstantNumber(a1)) {
            const char* k = lookupFact(a1, 1);            /* known constant?   */
            if (k)      { a1 = k; optStats.constProp++; changesThisPass++; }
            else {
                const char* cp = lookupFact(a1, 0);        /* known copy?       */
                if (cp) { a1 = cp; optStats.copyProp++;  changesThisPass++; }
            }
        }
        if (a2 && arg2IsValue(c->op) && !isConstantNumber(a2)) {
            const char* k = lookupFact(a2, 1);
            if (k)      { a2 = k; optStats.constProp++; changesThisPass++; }
            else {
                const char* cp = lookupFact(a2, 0);
                if (cp) { a2 = cp; optStats.copyProp++;  changesThisPass++; }
            }
        }

        TACInstr* n = NULL;

        /* --- algebraic simplification: x+0, 0+x, x-0, x*1, 1*x, x*0, 0*x, x/1 --- */
        if ((c->op == TAC_ADD || c->op == TAC_SUB) && a2 && strcmp(a2, "0") == 0)
            n = createTAC(TAC_ASSIGN, (char*)a1, NULL, c->result);
        else if (c->op == TAC_ADD && a1 && strcmp(a1, "0") == 0)
            n = createTAC(TAC_ASSIGN, (char*)a2, NULL, c->result);
        else if (c->op == TAC_MUL && a2 && strcmp(a2, "1") == 0)
            n = createTAC(TAC_ASSIGN, (char*)a1, NULL, c->result);
        else if (c->op == TAC_MUL && a1 && strcmp(a1, "1") == 0)
            n = createTAC(TAC_ASSIGN, (char*)a2, NULL, c->result);
        else if (c->op == TAC_MUL && ((a2 && strcmp(a2, "0") == 0) || (a1 && strcmp(a1, "0") == 0)))
            n = createTAC(TAC_ASSIGN, "0", NULL, c->result);
        else if (c->op == TAC_DIV && a2 && strcmp(a2, "1") == 0)
            n = createTAC(TAC_ASSIGN, (char*)a1, NULL, c->result);

        if (n) { optStats.algebraic++; changesThisPass++; }

        /* --- constant folding: both operands are now literals --- */
        if (!n) {
            int folded = 0;
            char* val = foldConstants(c->op, a1, a2, &folded);
            if (folded) {
                n = createTAC(TAC_ASSIGN, val, NULL, c->result);
                free(val);
                optStats.constFold++;
                changesThisPass++;
            }
        }

        /* --- constant folding of unary minus: t0 = -5 --- */
        if (!n && c->op == TAC_NEG && isConstantNumber(a1)) {
            char val[24];
            snprintf(val, sizeof val, "%d", (int)-atol(a1));
            n = createTAC(TAC_ASSIGN, val, NULL, c->result);
            optStats.constFold++;
            changesThisPass++;
        }

        if (!n) n = createTAC(c->op, (char*)a1, (char*)a2, (char*)stored);

        if (!mid.head) mid.head = mid.tail = n;
        else { mid.tail->next = n; mid.tail = n; }

        /* Remember what `result` holds now, for instructions still to come.
         * ARRAY_STORE is skipped: its `result` is a value being READ. */
        if (n->result && n->op != TAC_ARRAY_STORE) {
            if (n->op == TAC_ASSIGN) recordFact(n->result, n->arg1, isConstantNumber(n->arg1));
            else                     dropFactsAbout(n->result);   /* value now unknown */
        }
    }

    /* --- dead code elimination -------------------------------------------
     * A pure, value-producing instruction (arithmetic, comparison, array
     * load — or an ASSIGN into a temporary, which is what algebraic
     * simplification and folding rewrite into above) is dead if nothing
     * from here to the end of the function ever reads its result.
     *
     * --- dead store elimination ------------------------------------------
     * The same test is extended to REAL variables, but only where it can be
     * proven safe: the variable must be a local DECLared in this function
     * (globals outlive the function and could be read elsewhere), and the
     * function must be straight-line code with no labels (with a backward
     * jump, a read that appears EARLIER in the listing could still run
     * after the store).  In the starter language every variable is a local
     * of the implicit main() and there are no jumps, so after propagation
     * has replaced every read with its constant, the stores themselves
     * become dead:   r = 6 ; PRINT 6   ->   PRINT 6 */
    TACList out = { NULL, NULL, mid.tempCount, mid.labelCount };
    TACInstr* func = NULL;          /* FUNC_BEGIN of the function we are in */
    for (TACInstr* c = mid.head; c; c = c->next) {
        if (c->op == TAC_FUNC_BEGIN) func = c;

        int eligible = mnemonicIsPure(c->op) ||
                       (c->op == TAC_ASSIGN && isTempName(c->result));
        if (eligible && c->result && !isReadLater(c->next, c->result)) {
            optStats.deadCode++;
            changesThisPass++;
            continue;   /* drop it */
        }
        if (c->op == TAC_ASSIGN && c->result && !isTempName(c->result) &&
            func && functionIsStraightLine(func) &&
            isDeclaredLocal(func, c->result) &&
            !isReadLater(c->next, c->result)) {
            optStats.deadStore++;
            changesThisPass++;
            continue;   /* drop it */
        }
        TACInstr* n = createTAC(c->op, c->arg1, c->arg2, c->result);
        if (!out.head) out.head = out.tail = n;
        else { out.tail->next = n; out.tail = n; }
    }
    return out;
}

void optimizeTAC(void) {
    memset(&optStats, 0, sizeof optStats);
    optStats.instructionsBefore = countTAC(&tacList);

    TACList work = copyList(&tacList);

    int pass = 0;
    trace("Running optimization passes until no further change:\n");
    do {
        changesThisPass = 0;
        TACList next = optimizePass(&work);   /* values: fold, propagate, DCE */
        work = flowPass(&next);               /* control: branches, jumps     */
        pass++;
        trace("  pass %d: %3d change(s), %3d instructions\n",
               pass, changesThisPass, countTAC(&work));
    } while (changesThisPass > 0 && pass < 20);

    optimizedList = work;
    optStats.passes = pass;
    optStats.instructionsAfter = countTAC(&optimizedList);

    int removed = optStats.instructionsBefore - optStats.instructionsAfter;
    double pct = optStats.instructionsBefore
               ? (100.0 * removed / optStats.instructionsBefore) : 0.0;

    trace("\n  Technique                     Applications\n");
    trace("  ─────────────────────────────────────────\n");
    trace("  Algebraic simplification      %6d\n", optStats.algebraic);
    trace("  Constant folding              %6d\n", optStats.constFold);
    trace("  Constant propagation          %6d\n", optStats.constProp);
    trace("  Copy propagation              %6d\n", optStats.copyProp);
    trace("  Dead code elimination         %6d\n", optStats.deadCode);
    trace("  Dead store elimination        %6d\n", optStats.deadStore);
    trace("  Unreachable code removal      %6d\n", optStats.unreachable);
    trace("  Branch simplification         %6d\n", optStats.branch);
    trace("  Jump threading                %6d\n", optStats.jumpThread);
    trace("  Jump-to-next removal          %6d\n", optStats.jumpToNext);
    trace("  Unused label removal          %6d\n", optStats.deadLabel);
    trace("  ─────────────────────────────────────────\n");
    trace("  TAC instructions   %d -> %d  (%d removed, %.1f%% smaller)\n\n",
           optStats.instructionsBefore, optStats.instructionsAfter, removed, pct);
}

/* -O0: hand the back end an unmodified copy of the TAC.  Comparing the .s
 * file this produces with the optimized one shows exactly what Phase 5 buys
 * (and gives the peephole pass in codegen more to do). */
void skipOptimizeTAC(void) {
    memset(&optStats, 0, sizeof optStats);
    optimizedList = copyList(&tacList);
    optStats.instructionsBefore = optStats.instructionsAfter = countTAC(&optimizedList);
    trace("Optimization disabled (-O0): the back end will use the TAC unchanged.\n");
}
