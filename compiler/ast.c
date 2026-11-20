/* =========================================================================
 * CST-405  ·  Project 4  ·  Loops and Optimization
 * FILE: ast.c   —   Phase 2 — Syntax analysis (the tree it builds)
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                        ^^^  this file
 *
 * PHASE
 *   Phase 2, syntax analysis (the tree the parser builds).
 *
 * RECEIVES
 *   Calls from the grammar actions in parser.y.
 *
 * PRODUCES
 *   AST nodes for every later phase, and the AST display.
 *
 * CREDITS
 *   AST constructors and printAST: Alissa Head.
 * ========================================================================= */

/* AST IMPLEMENTATION
 * Functions to create and manipulate Abstract Syntax Tree nodes
 * The AST is built during parsing and used for all subsequent phases
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

/* External line number from scanner */
extern int yylineno;

/* Create a number literal node */
ASTNode* createNum(int value) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_NUM;
    node->lineno = yylineno;
    node->data.num = value;  /* Store the integer value */
    return node;
}

/* Create a variable reference node */
ASTNode* createVar(char* name) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_VAR;
    node->lineno = yylineno;
    node->data.name = strdup(name);  /* Copy the variable name */
    return node;
}

/* Create a binary operation node (e.g. x + y) */
ASTNode* createBinOp(char op, ASTNode* left, ASTNode* right) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_BINOP;
    node->lineno = yylineno;
    node->data.binop.op = op;
    node->data.binop.left = left;
    node->data.binop.right = right;
    return node;
}

/* Create a declaration node (e.g. int x) */
ASTNode* createDecl(char* type, char* name) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_DECL;
    node->lineno = yylineno;
    node->data.decl.varType = strdup(type);  /* both come from short-lived   */
    node->data.decl.name    = strdup(name);  /* buffers (a literal / yytext) */
    return node;
}

/* Create an assignment node (e.g. x = 10) */
ASTNode* createAssign(char* var, ASTNode* value) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_ASSIGN;
    node->lineno = yylineno;
    node->data.assign.var   = strdup(var);   /* scanner's buffer is reused   */
    node->data.assign.value = value;
    node->data.assign.arrayLHS = NULL;
    return node;
}

/* Create a print statement node (e.g. print(x)) */
ASTNode* createPrint(ASTNode* expr) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_PRINT;
    node->lineno = yylineno;
    node->data.expr = expr;
    return node;
}

/* Create a statement list node, linking stmt1 (the list so far, since the
 * grammar is left recursive) to stmt2 (the newly parsed statement). */
ASTNode* createStmtList(ASTNode* stmt1, ASTNode* stmt2) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_STMT_LIST;
    node->lineno = yylineno;
    node->data.stmtlist.stmt = stmt1;
    node->data.stmtlist.next = stmt2;
    return node;
}

/* Allocate a node and stamp it with its kind and the scanner's current line.
 * parser.y overwrites the line where a more exact one is known. */
static ASTNode* newNode(NodeType type) {
    ASTNode* node = malloc(sizeof(ASTNode));
    memset(node, 0, sizeof(ASTNode));
    node->type = type;
    node->lineno = yylineno;
    return node;
}

/* Create a function definition; both name and body are kept by the node */
ASTNode* createFuncDef(char* name, int isVoid, ASTNode* params, ASTNode* body) {
    ASTNode* node = newNode(NODE_FUNC_DEF);
    node->data.funcdef.name   = strdup(name);
    node->data.funcdef.isVoid = isVoid;
    node->data.funcdef.params = params;
    node->data.funcdef.body   = body;
    return node;
}

/* Create one parameter.  isArray means the slot will hold an ADDRESS. */
ASTNode* createParam(char* name, int isArray) {
    ASTNode* node = newNode(NODE_PARAM);
    node->data.param.name    = strdup(name);
    node->data.param.isArray = isArray;
    return node;
}

/* Lists are appended at the tail, unlike createStmtList, because the order of
 * parameters and arguments IS the calling convention: the first one must be
 * the first thing every later phase meets when it walks the list. */
static ASTNode* appendToList(NodeType type, ASTNode* list, ASTNode* item) {
    ASTNode* node = newNode(type);
    node->data.list.item = item;
    node->data.list.next = NULL;
    if (!list) return node;
    ASTNode* tail = list;
    while (tail->data.list.next) tail = tail->data.list.next;
    tail->data.list.next = node;
    return list;
}

ASTNode* createParamList(ASTNode* list, ASTNode* param) {
    return appendToList(NODE_PARAM_LIST, list, param);
}

ASTNode* createArgList(ASTNode* list, ASTNode* expr) {
    return appendToList(NODE_ARG_LIST, list, expr);
}

/* Create a function call; args is NULL for f() */
ASTNode* createFuncCall(char* name, ASTNode* args) {
    ASTNode* node = newNode(NODE_FUNC_CALL);
    node->data.call.name = strdup(name);
    node->data.call.args = args;
    return node;
}

/* Create a return; expr is NULL for a bare `return;` */
ASTNode* createReturn(ASTNode* expr) {
    ASTNode* node = newNode(NODE_RETURN);
    node->data.ret = expr;
    return node;
}

/* Create a { ... } block; stmts is NULL for `{ }` */
ASTNode* createBlock(ASTNode* stmts) {
    ASTNode* node = newNode(NODE_BLOCK);
    node->data.block = stmts;
    return node;
}

/* Create an if statement; elseStmt is NULL when there is no else */
ASTNode* createIf(ASTNode* cond, ASTNode* thenStmt, ASTNode* elseStmt) {
    ASTNode* node = newNode(NODE_IF);
    node->data.ifstmt.cond     = cond;
    node->data.ifstmt.thenStmt = thenStmt;
    node->data.ifstmt.elseStmt = elseStmt;
    return node;
}

/* Create a while loop */
ASTNode* createWhile(ASTNode* cond, ASTNode* body) {
    ASTNode* node = newNode(NODE_WHILE);
    node->data.loop.cond = cond;
    node->data.loop.body = body;
    return node;
}

/* Create a for loop; init, cond and update may each be NULL */
ASTNode* createFor(ASTNode* init, ASTNode* cond, ASTNode* update, ASTNode* body) {
    ASTNode* node = newNode(NODE_FOR);
    node->data.loop.init   = init;
    node->data.loop.cond   = cond;
    node->data.loop.update = update;
    node->data.loop.body   = body;
    return node;
}

/* break and continue carry no data: WHICH loop they refer to is decided by
 * context, in semantic.c (is there one?) and tac.c (which label?). */
ASTNode* createBreak(void)    { return newNode(NODE_BREAK); }
ASTNode* createContinue(void) { return newNode(NODE_CONTINUE); }

/* Create an array declaration.  The size is stored exactly as written, even
 * if it is nonsense: rejecting int a[0] is semantic.c's job, with a line
 * number, not the parser's. */
ASTNode* createArrayDecl(char* name, int size) {
    ASTNode* node = newNode(NODE_ARRAY_DECL);
    node->data.arraydecl.name = strdup(name);
    node->data.arraydecl.size = size;
    return node;
}

/* Create an array element reference, on either side of an assignment */
ASTNode* createArrayIndex(char* name, ASTNode* index) {
    ASTNode* node = newNode(NODE_ARRAY_INDEX);
    node->data.arrayindex.name  = strdup(name);
    node->data.arrayindex.index = index;
    return node;
}

/* Create a[i] = value.  The target is a tree rather than a name, so `var`
 * stays NULL and later phases branch on arrayLHS. */
ASTNode* createArrayAssign(ASTNode* target, ASTNode* value) {
    ASTNode* node = newNode(NODE_ASSIGN);
    node->data.assign.var      = NULL;
    node->data.assign.value    = value;
    node->data.assign.arrayLHS = target;
    return node;
}

/* Text form of an operator code.  An AST node stores every operator in one
 * char, so the two-character operators are packed into single letters and
 * this is the one place that has to know how. */
const char* opText(char op) {
    static char buf[2];
    switch (op) {
        case 'u': return "unary -";
        case 'L': return "<=";
        case 'G': return ">=";
        case 'E': return "==";
        case 'N': return "!=";
        default:
            buf[0] = op;
            buf[1] = '\0';
            return buf;
    }
}

/* Print `count` levels of indentation (printAST's only formatting rule) */
static void indent(int level) {
    for (int i = 0; i < level; i++) printf("  ");
}

/* Display the AST structure (for debugging and education) */
void printAST(ASTNode* node, int level) {
    if (!node) return;
    /* A list is structure, not a construct, so it adds no line of its own */
    if (node->type != NODE_STMT_LIST && node->type != NODE_PARAM_LIST &&
        node->type != NODE_ARG_LIST)
        indent(level);

    switch (node->type) {
        case NODE_NUM:
            printf("NUM %d\n", node->data.num);
            break;
        case NODE_VAR:
            printf("VAR %s\n", node->data.name);
            break;
        case NODE_BINOP:
            printf("BINOP '%s'\n", opText(node->data.binop.op));
            printAST(node->data.binop.left, level + 1);
            printAST(node->data.binop.right, level + 1);
            break;
        case NODE_DECL:
            printf("DECL %s %s\n", node->data.decl.varType, node->data.decl.name);
            break;
        case NODE_ASSIGN:
            if (node->data.assign.arrayLHS) {
                printf("ASSIGN to array element\n");
                printAST(node->data.assign.arrayLHS, level + 1);
            } else {
                printf("ASSIGN %s\n", node->data.assign.var);
            }
            printAST(node->data.assign.value, level + 1);
            break;
        case NODE_PRINT:
            printf("PRINT\n");
            printAST(node->data.expr, level + 1);
            break;
        case NODE_STMT_LIST:
            /* A list of statements is a sequence, not a nesting: print both
             * halves at the SAME level instead of indenting deeper. */
            printAST(node->data.stmtlist.stmt, level);
            printAST(node->data.stmtlist.next, level);
            break;
        case NODE_FUNC_DEF:
            printf("FUNC_DEF %s %s\n", node->data.funcdef.isVoid ? "void" : "int",
                   node->data.funcdef.name);
            printAST(node->data.funcdef.params, level + 1);
            printAST(node->data.funcdef.body, level + 1);
            break;
        case NODE_PARAM:
            printf("PARAM int%s %s\n", node->data.param.isArray ? "[]" : "",
                   node->data.param.name);
            break;
        case NODE_PARAM_LIST:
        case NODE_ARG_LIST:
            printAST(node->data.list.item, level);
            printAST(node->data.list.next, level);
            break;
        case NODE_FUNC_CALL:
            printf("CALL %s\n", node->data.call.name);
            printAST(node->data.call.args, level + 1);
            break;
        case NODE_RETURN:
            printf("RETURN\n");
            printAST(node->data.ret, level + 1);
            break;
        case NODE_BLOCK:
            printf("BLOCK\n");
            printAST(node->data.block, level + 1);
            break;
        case NODE_ARRAY_DECL:
            printf("ARRAY_DECL int %s[%d]\n", node->data.arraydecl.name,
                   node->data.arraydecl.size);
            break;
        case NODE_ARRAY_INDEX:
            printf("ARRAY_INDEX %s\n", node->data.arrayindex.name);
            printAST(node->data.arrayindex.index, level + 1);
            break;
        case NODE_IF:
            printf("IF\n");
            printAST(node->data.ifstmt.cond, level + 1);
            printAST(node->data.ifstmt.thenStmt, level + 1);
            if (node->data.ifstmt.elseStmt) {
                indent(level);
                printf("ELSE\n");
                printAST(node->data.ifstmt.elseStmt, level + 1);
            }
            break;
        case NODE_WHILE:
            printf("WHILE\n");
            printAST(node->data.loop.cond, level + 1);
            printAST(node->data.loop.body, level + 1);
            break;
        case NODE_FOR:
            /* label each part: any of them may be missing */
            printf("FOR\n");
            indent(level + 1); printf("init:%s\n",   node->data.loop.init   ? "" : " (none)");
            printAST(node->data.loop.init, level + 2);
            indent(level + 1); printf("cond:%s\n",   node->data.loop.cond   ? "" : " (always true)");
            printAST(node->data.loop.cond, level + 2);
            indent(level + 1); printf("update:%s\n", node->data.loop.update ? "" : " (none)");
            printAST(node->data.loop.update, level + 2);
            indent(level + 1); printf("body:\n");
            printAST(node->data.loop.body, level + 2);
            break;
        case NODE_BREAK:
            printf("BREAK\n");
            break;
        case NODE_CONTINUE:
            printf("CONTINUE\n");
            break;
        default:
            printf("UNKNOWN NODE TYPE %d\n", node->type);
            break;
    }
}
