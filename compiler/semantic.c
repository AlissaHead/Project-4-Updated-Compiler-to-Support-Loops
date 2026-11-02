/* =========================================================================
 * CST-405  ·  Project 3  ·  Complex Variables and Functions
 * FILE: semantic.c   —   Phase 3 — Semantic analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                               ^^^^^^^^  this file
 *
 * PHASE
 *   Phase 3, semantic analysis (the VISIBILITY table).
 *
 * RECEIVES
 *   The AST from the parser.
 *
 * PRODUCES
 *   The same AST with every name resolved to a unique spelling, plus a verdict.
 *   Errors are reported with line numbers.
 *
 * CREDITS
 *   Scope stack, two-pass analysis and all semantic checks: Alissa Head.
 * ========================================================================= */

/* SEMANTIC ANALYSIS IMPLEMENTATION - WITH FUNCTION SUPPORT
 * Performs semantic checks on the Abstract Syntax Tree
 * Now supports functions, scopes, parameters, control flow
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "semantic.h"
#include "symtab.h"
#include "trace.h"

#define MAX_FUNCTIONS 100
#define MAX_PARAMS 20
#define MAX_SCOPE_DEPTH 64
#define MAX_VAR_TEXT 320       /* Longest name, including any .N suffix  */
#define MAX_ARGS_ALLOWED 4     /* $a0-$a3; codegen.h has the same limit */

/* THE VISIBILITY TABLE
 * This scope stack answers "is this name visible here, and what does it mean?"
 * It exists only during this phase.  The STORAGE table in symtab.c answers a
 * different question ("what address is this name?") during Phase 6, so the
 * two are kept apart even though both hold names.
 *
 * The one thing that crosses over is `unique`.  The storage table is flat, one
 * table per function, but a function can contain several different variables
 * spelled the same way:
 *        int x;  { int x; }  { int x; }
 * So when this phase resolves a name, it rewrites the AST to carry a spelling
 * that is unique within the function, such as `x.2`.  A '.' cannot appear in a
 * source identifier, so the new name can never collide with a real one.
 */
typedef struct {
    char* name;      /* As written in the source                    */
    char* unique;    /* The name later phases will see              */
    int   isArray;   /* 1 = array or array parameter                */
} ScopeEntry;

typedef struct {
    ScopeEntry vars[MAX_VARS];
    int count;
} Scope;

/* THE FUNCTION TABLE
 * Variables and functions are separate namespaces, which is why a program may
 * declare both `int total;` and `int total(...)`.  An entry holds exactly what
 * it takes to check a call. */
typedef struct {
    char* name;
    int   paramCount;
    int   isArrayParam[MAX_PARAMS];
    int   isVoid;
    int   lineno;
} FuncEntry;

/* Global semantic information */
static SemanticInfo semInfo;
static Scope scopes[MAX_SCOPE_DEPTH];
static int scopeDepth = 0;
static FuncEntry functions[MAX_FUNCTIONS];
static int functionCount = 0;
static FuncEntry* currentFunc = NULL;   /* NULL outside any function body  */
static int programHasFunctions = 0;     /* 0 = Project 2 style flat program */
static int uniqueCounter = 0;           /* Numbers the `x.N` spellings      */

/* Initialize semantic analyzer */
void initSemantic() {
    semInfo.errorCount = 0;
    semInfo.warningCount = 0;
    scopeDepth = 0;
    functionCount = 0;
    currentFunc = NULL;
    programHasFunctions = 0;
    uniqueCounter = 0;

    trace("SEMANTIC ANALYZER: initialized\n\n");
}

/* Scope management */
static void enterScope() {
    if (scopeDepth >= MAX_SCOPE_DEPTH) {
        /* exit, not return: the matching exitScope would pop the wrong scope */
        fprintf(stderr, "SEMANTIC ERROR: blocks are nested more than %d levels deep\n",
                MAX_SCOPE_DEPTH);
        exit(1);
    }
    scopes[scopeDepth].count = 0;
    scopeDepth++;
}

static void exitScope() {
    if (scopeDepth > 0) {
        /* Free variable names in this scope */
        for (int i = 0; i < scopes[scopeDepth - 1].count; i++) {
            free(scopes[scopeDepth - 1].vars[i].name);
            free(scopes[scopeDepth - 1].vars[i].unique);
        }
        scopeDepth--;
    }
}

/* Print current semantic scopes for debugging */
static void printSemanticScopes() {
    trace("\n┌─────────────────────────────────────────────────────────┐\n");
    trace("│ SEMANTIC SCOPE STACK (Depth: %d)                        \n", scopeDepth);
    trace("├─────────────────────────────────────────────────────────┤\n");

    if (scopeDepth == 0) {
        trace("│ (no active scopes)                                      │\n");
    } else {
        for (int depth = 0; depth < scopeDepth; depth++) {
            if (depth == 0) {
                trace("│ Scope[%d] GLOBAL (%d variables)                        \n", depth, scopes[depth].count);
            } else if (depth == 1 && currentFunc) {
                trace("│ Scope[%d] FUNCTION %s (%d variables)                   \n", depth, currentFunc->name, scopes[depth].count);
            } else {
                trace("│ Scope[%d] BLOCK (%d variables)                         \n", depth, scopes[depth].count);
            }

            if (scopes[depth].count > 0) {
                trace("│   Variables: ");
                for (int i = 0; i < scopes[depth].count; i++) {
                    ScopeEntry* e = &scopes[depth].vars[i];
                    trace("%s%s", e->name, e->isArray ? "[]" : "");
                    if (strcmp(e->name, e->unique) != 0) trace(" (stored as %s)", e->unique);
                    if (i < scopes[depth].count - 1) trace(", ");
                }
                trace("\n");
            } else {
                trace("│   (empty)\n");
            }
        }
    }
    trace("└─────────────────────────────────────────────────────────┘\n\n");
}

/* Add variable to current scope */
/* RESERVED IDENTIFIERS
 * The intermediate-code generator invents names of its own: temporaries
 * t0, t1, ... and labels L0, L1, ...  If a user variable had one of those
 * shapes the back end could not tell them apart, so the language reserves
 * that namespace.  Reporting it here — in the semantic analyzer, with a line
 * number — is far kinder than a mysterious wrong answer at run time. */
static int isReservedName(const char* name) {
    if (!name || !name[0]) return 0;
    if (strncmp(name, "__sw", 4) == 0) return 1;          /* switch temporaries */
    if (name[0] != 't' && name[0] != 'L') return 0;
    if (!name[1]) return 0;
    for (int i = 1; name[i]; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    return 1;
}

static void reportReserved(const char* name, int lineno) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ SEMANTIC ERROR - Reserved Identifier                      ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    fprintf(stderr, "  ❌ Error: '%s' is reserved for the compiler's own use\n", name);
    fprintf(stderr, "  📖 Note: names of the form t0, t1, ... are three-address-code\n");
    fprintf(stderr, "           temporaries and L0, L1, ... are generated labels.\n");
    fprintf(stderr, "  💡 Suggestion: rename the variable, for example '%s_' or 'total'\n\n", name);
}

/* Find `name` in one scope.  Returns NULL when that scope does not declare it. */
static ScopeEntry* findInScope(Scope* scope, const char* name) {
    for (int i = 0; i < scope->count; i++) {
        if (strcmp(scope->vars[i].name, name) == 0) return &scope->vars[i];
    }
    return NULL;
}

/* Resolve a use of `name`: search innermost to outermost and stop at the first
 * hit.  Shadowing needs no code of its own; it is simply this search order.
 * Returns NULL when the name is not visible from here. */
static ScopeEntry* lookupVar(const char* name) {
    for (int depth = scopeDepth - 1; depth >= 0; depth--) {
        ScopeEntry* e = findInScope(&scopes[depth], name);
        if (e) return e;
    }
    return NULL;
}

/* Should a declaration at the current depth get a unique `x.N` spelling?
 *   - Anything in a nested block: yes.  Two blocks may reuse a name, and the
 *     function's storage table holds only one entry per spelling.
 *   - A function-level local with the same name as a global: yes.  Storage
 *     lookup searches locals first, for the WHOLE function, so a line that
 *     used the global BEFORE this declaration would otherwise be misread.
 *   - Everything else keeps its name, so the listings stay readable. */
static int needsUniqueName(const char* name) {
    int functionLevel = programHasFunctions ? 2 : 1;
    if (scopeDepth > functionLevel) return 1;
    if (programHasFunctions && scopeDepth == 2 && findInScope(&scopes[0], name)) return 1;
    return 0;
}

/* Declare `name` in the current scope.  Returns the entry, or NULL if the
 * current scope already declares it.  Only the CURRENT scope is searched: the
 * same name in an outer scope is legal shadowing, not a duplicate. */
static ScopeEntry* addVarToScope(const char* name, int isArray, int keepName) {
    if (scopeDepth == 0) {
        fprintf(stderr, "SEMANTIC ERROR: No scope to add variable to\n");
        return NULL;
    }

    Scope* currentScope = &scopes[scopeDepth - 1];
    if (findInScope(currentScope, name)) return NULL;

    if (currentScope->count >= MAX_VARS) {
        fprintf(stderr, "SEMANTIC ERROR: Too many variables in scope\n");
        return NULL;
    }

    char unique[MAX_VAR_TEXT];
    if (!keepName && needsUniqueName(name))
        snprintf(unique, sizeof unique, "%s.%d", name, ++uniqueCounter);
    else
        snprintf(unique, sizeof unique, "%s", name);

    ScopeEntry* e = &currentScope->vars[currentScope->count++];
    e->name    = strdup(name);
    e->unique  = strdup(unique);
    e->isArray = isArray;
    return e;
}

/* Replace the name stored in an AST node with its resolved spelling. */
static void setName(char** slot, const char* unique) {
    if (strcmp(*slot, unique) == 0) return;
    free(*slot);
    *slot = strdup(unique);
}

/* Find a function by name.  Returns NULL if none has that name. */
static FuncEntry* findFunction(const char* name) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functions[i].name, name) == 0) return &functions[i];
    }
    return NULL;
}

/* Number of arguments in a call (0 for f()). */
static int countArgs(ASTNode* args) {
    int n = 0;
    for (ASTNode* a = args; a; a = a->data.list.next) n++;
    return n;
}

/* Forward declaration: checkStmt and checkStmtList are mutually recursive,
 * which is exactly what you want when the thing being checked is a tree. */
static void checkStmtList(ASTNode* node);
static void checkExpr(ASTNode* node);

/* Print one error in the standard format and count it. */
static void semanticError(const char* title, int lineno, const char* suggestion,
                          const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ SEMANTIC ERROR - %-41s ║\n", title);
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    if (lineno > 0) fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    fprintf(stderr, "  ❌ Error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (suggestion) fprintf(stderr, "  💡 Suggestion: %s\n", suggestion);
    fprintf(stderr, "\n");
    semInfo.errorCount++;
}

/* Report an undeclared variable, with a line number and a fix. */
static void reportUndeclared(const char* name, int lineno) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ SEMANTIC ERROR - Undeclared Variable                      ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    fprintf(stderr, "  ❌ Error: Variable '%s' is used before being declared\n", name);
    fprintf(stderr, "  💡 Suggestion: Add a declaration before using this variable:\n");
    fprintf(stderr, "     → int %s;  (add this before line %d)\n\n", name, lineno);
    semInfo.errorCount++;
}

/* Report a variable declared twice in the same scope. */
static void reportRedeclared(const char* name, int lineno) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ SEMANTIC ERROR - Duplicate Declaration                    ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    fprintf(stderr, "  ❌ Error: Variable '%s' is already declared in this scope\n", name);
    fprintf(stderr, "  💡 Suggestion: remove the duplicate, or rename one of the two\n\n");
    semInfo.errorCount++;
}

/* Check a declared name and enter it in scope, reporting reserved names and
 * duplicates.  Returns the entry, or NULL if it was a duplicate. */
static ScopeEntry* declareName(const char* name, int isArray, int keepName, int lineno) {
    /* Names shaped like t0 or L3 are reserved for the compiler's own
     * temporaries and labels (isReservedName, above): catch that here, with a
     * line number, instead of letting it corrupt TAC. */
    if (isReservedName(name)) {
        reportReserved(name, lineno);
        semInfo.errorCount++;
        /* Still enter the name in scope.  The program will not be compiled
         * either way, but without this every later use of the name would ALSO
         * be reported as "undeclared": a cascade with one single cause. */
        return addVarToScope(name, isArray, keepName);
    }

    ScopeEntry* e = addVarToScope(name, isArray, keepName);
    if (!e) reportRedeclared(name, lineno);
    else    trace("  ✓ '%s' declared (line %d)\n", name, lineno);
    return e;
}

/* Check a call: the function must exist, the argument COUNT must match, and
 * each argument must be the right KIND (an array where the parameter is an
 * array, a value where it is not).  Argument expressions are checked too.
 * `needValue` is 1 where the result is used, which a void function cannot
 * provide.  Functions come from the table built in pass 1, so a call may
 * appear above the definition it refers to. */
static void checkCall(ASTNode* node, int needValue) {
    char* name = node->data.call.name;
    FuncEntry* f = findFunction(name);
    int count = countArgs(node->data.call.args);

    if (!f) {
        semanticError("Undeclared Function", node->lineno,
                      "define the function, or check the spelling of the call",
                      "Function '%s' is called but never defined", name);
    } else if (count != f->paramCount) {
        semanticError("Wrong Argument Count", node->lineno, NULL,
                      "Function '%s' takes %d argument(s), but this call passes %d",
                      name, f->paramCount, count);
    } else {
        trace("  ✓ Call to '%s' with %d argument(s) (line %d)\n", name, count, node->lineno);
    }
    /* main ends in the exit syscall rather than `jr $ra`, so a call to it could
     * never return.  Rejecting the call is better than a program that stops
     * silently.  Limitation: the language has no recursive main. */
    if (f && strcmp(name, "main") == 0) {
        semanticError("Call To Main", node->lineno, NULL,
                      "'main' is the program's entry point and cannot be called");
    }
    if (f && needValue && f->isVoid) {
        semanticError("Void Value Used", node->lineno, NULL,
                      "Function '%s' is void, so its call has no value to use", name);
    }

    int i = 0;
    for (ASTNode* a = node->data.call.args; a; a = a->data.list.next, i++) {
        ASTNode* arg = a->data.list.item;
        int wantArray = (f && count == f->paramCount) ? f->isArrayParam[i] : -1;

        /* A bare array name is valid ONLY as an argument, so it is handled
         * here rather than in checkExpr, which rejects it everywhere else. */
        ScopeEntry* e = (arg->type == NODE_VAR) ? lookupVar(arg->data.name) : NULL;
        if (e && e->isArray) {
            if (wantArray == 0) {
                semanticError("Wrong Argument Kind", node->lineno, NULL,
                              "Argument %d of '%s' must be a value, but '%s' is an array",
                              i + 1, name, arg->data.name);
            }
            setName(&arg->data.name, e->unique);
            continue;
        }

        if (wantArray == 1) {
            semanticError("Wrong Argument Kind", node->lineno, NULL,
                          "Argument %d of '%s' must be an array", i + 1, name);
        }
        checkExpr(arg);
    }
}

/* Check expression for semantic correctness */
static void checkExpr(ASTNode* node) {
    if (!node) return;

    switch (node->type) {
        case NODE_NUM:
            /* A literal always means what it says — nothing to check. */
            break;

        case NODE_VAR: {
            /* The name must be declared and VISIBLE from here: the scope
             * stack is searched innermost-first, so an outer name still
             * resolves and a shadowed one correctly does not. */
            ScopeEntry* e = lookupVar(node->data.name);
            if (!e) {
                reportUndeclared(node->data.name, node->lineno);
            } else if (e->isArray) {
                semanticError("Array Used As Value", node->lineno,
                              "use an element, such as a[0]",
                              "'%s' is an array and has no single value", node->data.name);
            } else {
                trace("  ✓ Use of '%s' resolves (line %d)\n", node->data.name, node->lineno);
                setName(&node->data.name, e->unique);
            }
            break;
        }

        case NODE_BINOP:
            /* Nothing to check about the operator itself; recurse into both
             * operands so an error deep in a+b+c is still reported.  (The
             * right operand is NULL for unary minus; checkExpr accepts that.) */
            checkExpr(node->data.binop.left);
            checkExpr(node->data.binop.right);
            break;

        case NODE_FUNC_CALL:
            checkCall(node, 1);
            break;

        case NODE_ARRAY_INDEX: {
            /* The name must be an array, and the index must check out.  What
             * is NOT checked: whether the index is in range.  That cannot be
             * known at compile time in general and this language has no
             * run-time check either, so a bad index silently reads or writes
             * the wrong memory.  This is a documented limitation. */
            ScopeEntry* e = lookupVar(node->data.arrayindex.name);
            if (!e) {
                semanticError("Undeclared Array", node->lineno,
                              "declare it first, for example: int name[10];",
                              "Array '%s' is used before being declared",
                              node->data.arrayindex.name);
            } else if (!e->isArray) {
                semanticError("Not An Array", node->lineno, NULL,
                              "'%s' is not an array, so it cannot be indexed",
                              node->data.arrayindex.name);
            } else {
                trace("  ✓ Use of array '%s' resolves (line %d)\n",
                      node->data.arrayindex.name, node->lineno);
                setName(&node->data.arrayindex.name, e->unique);
            }
            checkExpr(node->data.arrayindex.index);
            break;
        }

        default:
            break;
    }
}

/* Check statement */
static void checkStmt(ASTNode* node) {
    if (!node) return;

    switch (node->type) {
        case NODE_DECL: {
            ScopeEntry* e = declareName(node->data.decl.name, 0, 0, node->lineno);
            if (e) setName(&node->data.decl.name, e->unique);
            break;
        }

        case NODE_ARRAY_DECL: {
            /* The size must be positive; nothing downstream would notice. */
            if (node->data.arraydecl.size <= 0) {
                semanticError("Invalid Array Size", node->lineno,
                              "the size must be a positive integer",
                              "Array '%s' is declared with size %d",
                              node->data.arraydecl.name, node->data.arraydecl.size);
            }
            ScopeEntry* e = declareName(node->data.arraydecl.name, 1, 0, node->lineno);
            if (e) setName(&node->data.arraydecl.name, e->unique);
            break;
        }

        case NODE_ASSIGN:
            /* The target must already be declared.  Order matters here: for
             *     int x;  x = x + 1;
             * checking the right-hand side must happen with x already in
             * scope, which it is — the DECL above was a separate, earlier
             * statement, so this check only has to confirm the target. */
            if (node->data.assign.arrayLHS) {
                checkExpr(node->data.assign.arrayLHS);
            } else {
                ScopeEntry* e = lookupVar(node->data.assign.var);
                if (!e) {
                    reportUndeclared(node->data.assign.var, node->lineno);
                } else if (e->isArray) {
                    semanticError("Array Used As Value", node->lineno,
                                  "assign to an element, such as a[0] = 1;",
                                  "'%s' is an array and cannot be assigned as a whole",
                                  node->data.assign.var);
                } else {
                    trace("  ✓ Assignment to '%s' is valid (line %d)\n",
                          node->data.assign.var, node->lineno);
                    setName(&node->data.assign.var, e->unique);
                }
            }
            checkExpr(node->data.assign.value);
            break;

        case NODE_PRINT:
            checkExpr(node->data.expr);
            break;

        case NODE_FUNC_CALL:
            checkCall(node, 0);     /* a statement discards the value */
            break;

        case NODE_RETURN:
            /* `return` outside a function parses, but it means nothing: that
             * gap between "parses" and "means something" is this phase's job. */
            if (!currentFunc) {
                semanticError("Return Outside Function", node->lineno, NULL,
                              "'return' can only appear inside a function");
            } else if (currentFunc->isVoid && node->data.ret) {
                semanticError("Return Type Mismatch", node->lineno, NULL,
                              "Function '%s' is void and cannot return a value",
                              currentFunc->name);
            } else if (!currentFunc->isVoid && !node->data.ret) {
                semanticError("Return Type Mismatch", node->lineno, NULL,
                              "Function '%s' must return an int value", currentFunc->name);
            }
            checkExpr(node->data.ret);
            break;

        case NODE_BLOCK:
            /* Opening and closing a scope is what makes this legal:
             *     int x;  { int x; }      inner x shadows outer x
             * and this an error:
             *     { int x; int x; }       same name twice in one scope */
            enterScope();
            checkStmtList(node->data.block);
            exitScope();
            break;

        case NODE_IF:
            checkExpr(node->data.ifstmt.cond);
            checkStmt(node->data.ifstmt.thenStmt);
            checkStmt(node->data.ifstmt.elseStmt);
            break;

        case NODE_STMT_LIST:
            checkStmtList(node);
            break;

        default:
            break;
    }
}

/* Check statement list */
static void checkStmtList(ASTNode* node) {
    if (!node) return;

    if (node->type == NODE_STMT_LIST) {
        checkStmt(node->data.stmtlist.stmt);
        checkStmtList(node->data.stmtlist.next);
    } else {
        checkStmt(node);
    }
}

/* Does the program define at least one function?  This decides between the
 * two program shapes: see performSemanticAnalysis. */
static int definesFunction(ASTNode* node) {
    if (!node) return 0;
    if (node->type == NODE_FUNC_DEF) return 1;
    if (node->type == NODE_STMT_LIST)
        return definesFunction(node->data.stmtlist.stmt) ||
               definesFunction(node->data.stmtlist.next);
    return 0;
}

/* Does this statement always return?  Deliberately simple: a list ends in
 * one, or an if/else returns on both paths.  Used only for a warning, so an
 * occasional false alarm is harmless. */
static int alwaysReturns(ASTNode* node) {
    if (!node) return 0;
    switch (node->type) {
        case NODE_RETURN:    return 1;
        case NODE_BLOCK:     return alwaysReturns(node->data.block);
        case NODE_STMT_LIST: return alwaysReturns(node->data.stmtlist.next) ||
                                    alwaysReturns(node->data.stmtlist.stmt);
        case NODE_IF:        return alwaysReturns(node->data.ifstmt.thenStmt) &&
                                    alwaysReturns(node->data.ifstmt.elseStmt);
        default:             return 0;
    }
}

/* PASS 1 helper: record one function's signature. */
static void registerFunction(ASTNode* node) {
    char* name = node->data.funcdef.name;

    if (isReservedName(name)) {
        reportReserved(name, node->lineno);
        semInfo.errorCount++;
    }
    FuncEntry* previous = findFunction(name);
    if (previous) {
        semanticError("Duplicate Function", node->lineno, NULL,
                      "Function '%s' is already defined on line %d",
                      name, previous->lineno);
        return;
    }
    if (functionCount >= MAX_FUNCTIONS) {
        fprintf(stderr, "SEMANTIC ERROR: Too many functions\n");
        semInfo.errorCount++;
        return;
    }

    FuncEntry* f = &functions[functionCount++];
    f->name       = name;
    f->paramCount = 0;
    f->isVoid     = node->data.funcdef.isVoid;
    f->lineno     = node->lineno;
    for (ASTNode* p = node->data.funcdef.params; p; p = p->data.list.next) {
        if (f->paramCount < MAX_PARAMS)
            f->isArrayParam[f->paramCount] = p->data.list.item->data.param.isArray;
        f->paramCount++;
    }

    /* Arguments travel in $a0-$a3 and nothing else.  Refusing a fifth here
     * gives a clear error instead of silent corruption at run time. */
    if (f->paramCount > MAX_ARGS_ALLOWED) {
        semanticError("Too Many Parameters", node->lineno, NULL,
                      "Function '%s' has %d parameters; the limit is %d ($a0-$a3)",
                      name, f->paramCount, MAX_ARGS_ALLOWED);
    }
    trace("  ✓ Function '%s' registered: %s, %d parameter(s) (line %d)\n",
          name, f->isVoid ? "void" : "int", f->paramCount, node->lineno);
}

/* PASS 1: walk the top level and record every function's signature. */
static void registerFunctions(ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_STMT_LIST) {
        registerFunctions(node->data.stmtlist.stmt);
        registerFunctions(node->data.stmtlist.next);
    } else if (node->type == NODE_FUNC_DEF) {
        registerFunction(node);
    }
}

/* PASS 2 helper: check one function body inside its own scope.  Parameters
 * and the body's top-level declarations share that scope, so redeclaring a
 * parameter in the body is a duplicate rather than a shadow. */
static void checkFuncDef(ASTNode* node) {
    currentFunc = findFunction(node->data.funcdef.name);
    trace("\nEntering function '%s'\n", node->data.funcdef.name);

    enterScope();
    for (ASTNode* p = node->data.funcdef.params; p; p = p->data.list.next) {
        ASTNode* param = p->data.list.item;
        /* keepName: a parameter is in scope for the whole body, so it can
         * never be confused with a global of the same name. */
        declareName(param->data.param.name, param->data.param.isArray, 1, param->lineno);
    }
    checkStmtList(node->data.funcdef.body);

    if (!node->data.funcdef.isVoid && strcmp(node->data.funcdef.name, "main") != 0 &&
        !alwaysReturns(node->data.funcdef.body)) {
        fprintf(stderr, "Warning at line %d: function '%s' can reach its end "
                        "without a return; the value will be 0\n",
                node->lineno, node->data.funcdef.name);
        semInfo.warningCount++;
    }

    trace("Scope stack at the end of '%s', just before its scope is popped:\n",
          node->data.funcdef.name);
    printSemanticScopes();
    exitScope();
    currentFunc = NULL;
}

/* PASS 2: walk the top level in source order, declaring globals and checking
 * function bodies.  A global is only visible below its declaration, as in C;
 * the two-pass rule applies to FUNCTIONS. */
static void checkFunctions(ASTNode* node) {
    if (!node) return;

    switch (node->type) {
        case NODE_STMT_LIST:
            checkFunctions(node->data.stmtlist.stmt);
            checkFunctions(node->data.stmtlist.next);
            break;
        case NODE_FUNC_DEF:
            checkFuncDef(node);
            break;
        case NODE_DECL:
        case NODE_ARRAY_DECL:
            checkStmt(node);
            break;
        default:
            semanticError("Statement Outside Function", node->lineno,
                          "move it into main(); a global cannot have an initial value",
                          "Executable statements must be inside a function");
            break;
    }
}

/* Entry point of Phase 3.
 *
 * There are two program shapes.  A file with NO functions is a Project 2
 * program: one flat list that becomes an implicit main(), checked with a
 * single walk.  A file WITH functions gets two passes, so that a function may
 * be called above its definition:
 *     pass 1  registerFunctions   record every signature
 *     pass 2  checkFunctions      check every body against those signatures
 * Collapsing them into one pass would make every forward call an error. */
int performSemanticAnalysis(ASTNode* root) {
    if (!root) {
        fprintf(stderr, "SEMANTIC ERROR: No AST to analyze\n");
        return -1;
    }

    trace("Running semantic analysis...\n\n");
    programHasFunctions = definesFunction(root);

    /* Enter global scope */
    enterScope();
    trace("Entered global scope\n");
    printSemanticScopes();

    if (programHasFunctions) {
        trace("Pass 1: recording function signatures\n");
        registerFunctions(root);

        FuncEntry* entry = findFunction("main");
        if (!entry) {
            semanticError("Missing Main", 0, "add: int main() { ... }",
                          "The program defines functions but no 'main'");
        } else if (entry->paramCount != 0) {
            semanticError("Invalid Main", entry->lineno, NULL,
                          "'main' must not take parameters");
        }

        trace("\nPass 2: checking declarations and function bodies\n");
        checkFunctions(root);
    } else {
        checkStmtList(root);
    }

    trace("\nFinished walking the AST — final state of the scope stack:\n");
    printSemanticScopes();

    /* Exit global scope */
    exitScope();

    return semInfo.errorCount > 0 ? -1 : 0;
}

/* Print semantic analysis summary */
void printSemanticSummary() {
    trace("═══════════════════════════════════════════\n");
    trace("SEMANTIC ANALYSIS SUMMARY\n");
    trace("═══════════════════════════════════════════\n");
    trace("Errors found:       %d\n", semInfo.errorCount);
    trace("Warnings found:     %d\n", semInfo.warningCount);
    trace("\n");

    if (semInfo.errorCount == 0) {
        trace("✓ Semantic analysis passed - program is semantically correct!\n");
    } else {
        trace("✗ Semantic analysis failed - fix errors before proceeding\n");
    }
    trace("═══════════════════════════════════════════\n\n");
}
