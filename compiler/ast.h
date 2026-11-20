/* =========================================================================
 * CST-405  ·  Project 4  ·  Loops and Optimization
 * FILE: ast.h   —   Phase 2 — Syntax analysis (the tree it builds)
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                        ^^^  this file
 *
 * PHASE
 *   Phase 2, syntax analysis (the tree the parser builds).
 *
 * RECEIVES
 *   Nothing: this is the shared definition of the tree.
 *
 * PRODUCES
 *   The node kinds and fields that parser.y fills in and later phases read.
 *
 * CREDITS
 *   Node layout from the CST-405 course starter code; Project 3 nodes: Alissa Head.
 * ========================================================================= */

#ifndef AST_H
#define AST_H

/* ABSTRACT SYNTAX TREE (AST)
 * The AST is an intermediate representation of the program structure
 * It represents the hierarchical syntax of the source code
 * Each node represents a construct in the language
 */

/* NODE TYPES - Different kinds of AST nodes in our language */
typedef enum {
    NODE_NUM,         /* Numeric literal (e.g., 42) */
    NODE_VAR,         /* Variable reference (e.g., x) */
    NODE_BINOP,       /* Binary operation (e.g., x + y) */
    NODE_DECL,        /* Variable declaration (e.g., int x) */
    NODE_ASSIGN,      /* Assignment statement (e.g., x = 10) */
    NODE_PRINT,       /* Print statement (e.g., print(x)) */
    NODE_STMT_LIST,   /* List of statements (program structure) */
    NODE_FUNC_DEF,    /* Function definition (e.g., int f(int a) { ... }) */
    NODE_PARAM,       /* One parameter: int a  or  int a[] */
    NODE_PARAM_LIST,  /* Parameters, in source order */
    NODE_FUNC_CALL,   /* Function call (e.g., f(1, 2)) */
    NODE_ARG_LIST,    /* Call arguments, in source order */
    NODE_RETURN,      /* return e;  or  return; */
    NODE_BLOCK,       /* { ... }: exists because it opens a SCOPE */
    NODE_ARRAY_DECL,  /* Array declaration (e.g., int a[10]) */
    NODE_ARRAY_INDEX, /* Array element (e.g., a[i]) */
    NODE_IF,          /* if (c) s  or  if (c) s else s */
    NODE_WHILE,       /* while (c) s */
    NODE_FOR,         /* for (init; c; update) s */
    NODE_BREAK,       /* break;    leaves the innermost loop */
    NODE_CONTINUE,    /* continue; starts the innermost loop's next iteration */
} NodeType;

/* AST NODE STRUCTURE
 * Uses a union to efficiently store different node data
 * Only the relevant fields for each node type are used
 */
typedef struct ASTNode {
    NodeType type;  /* Identifies what kind of node this is */
    int lineno;     /* Line number in source code for error reporting */

    /* Union allows same memory to store different data types */
    union {
        /* Literal number value (NODE_NUM) */
        int num;
        
        /* Variable reference name (NODE_VAR) */
        char* name;

        /* Declaration structure (NODE_DECL) */
        struct {
            char* name;
            char* varType;
        } decl;


        /* Binary operation structure (NODE_BINOP) */
        struct {
            char op;                    /* Operator code; see opText() */
            struct ASTNode* left;       /* Left operand */
            struct ASTNode* right;      /* Right operand (NULL for unary minus) */
        } binop;

        /* Assignment structure (NODE_ASSIGN) */
        struct {
            char* var;                  /* Variable being assigned to (NULL for array) */
            struct ASTNode* value;      /* Expression being assigned */
            struct ASTNode* arrayLHS;   /* The a[i] target; NULL for a plain variable */
        } assign;
        
        /* Print expression (NODE_PRINT) */
        struct ASTNode* expr;
        
        /* Statement list structure (NODE_STMT_LIST) */
        struct {
            struct ASTNode* stmt;       /* Current statement */
            struct ASTNode* next;       /* Rest of the list */
        } stmtlist;

        /* Function definition (NODE_FUNC_DEF) */
        struct {
            char* name;
            int   isVoid;               /* 1 = void, 0 = int */
            struct ASTNode* params;     /* NODE_PARAM_LIST, or NULL */
            struct ASTNode* body;       /* Statements; NULL for an empty body */
        } funcdef;

        /* One parameter (NODE_PARAM) */
        struct {
            char* name;
            int   isArray;              /* 1 for `int a[]`: receives an ADDRESS */
        } param;

        /* Parameter or argument list (NODE_PARAM_LIST, NODE_ARG_LIST) */
        struct {
            struct ASTNode* item;       /* NODE_PARAM, or an argument expression */
            struct ASTNode* next;       /* Rest of the list, in source order */
        } list;

        /* Function call (NODE_FUNC_CALL) */
        struct {
            char* name;
            struct ASTNode* args;       /* NODE_ARG_LIST, or NULL */
        } call;

        /* return (NODE_RETURN) and { ... } (NODE_BLOCK) */
        struct ASTNode* ret;            /* Value; NULL for a bare `return;` */
        struct ASTNode* block;          /* Statements; NULL for `{ }` */

        /* Array declaration (NODE_ARRAY_DECL) */
        struct {
            char* name;
            int   size;                 /* As written; semantic.c rejects <= 0 */
        } arraydecl;

        /* Array element (NODE_ARRAY_INDEX) */
        struct {
            char* name;
            struct ASTNode* index;      /* Any integer expression */
        } arrayindex;

        /* if statement (NODE_IF) */
        struct {
            struct ASTNode* cond;
            struct ASTNode* thenStmt;
            struct ASTNode* elseStmt;   /* NULL when there is no else */
        } ifstmt;

        /* while and for loops (NODE_WHILE, NODE_FOR).  A while uses only
         * cond and body.  Any of init, cond and update may be NULL in a for;
         * a NULL cond means "always true". */
        struct {
            struct ASTNode* init;       /* an ASSIGN, run once before the loop */
            struct ASTNode* cond;
            struct ASTNode* update;     /* an ASSIGN, run after each iteration */
            struct ASTNode* body;
        } loop;
    } data;
} ASTNode;

/* AST CONSTRUCTION FUNCTIONS
 * These functions are called by the parser to build the tree
 */
/* Basic nodes */
ASTNode* createNum(int value);                                   /* Create number node */
ASTNode* createVar(char* name);                                  /* Create variable node */
ASTNode* createBinOp(char op, ASTNode* left, ASTNode* right);   /* Create binary op node */
ASTNode* createDecl(char* type, char* name);                     /* Create declaration node */
ASTNode* createAssign(char* var, ASTNode* value);               /* Create assignment node */
ASTNode* createPrint(ASTNode* expr);                            /* Create print node */
ASTNode* createStmtList(ASTNode* stmt1, ASTNode* stmt2);        /* Create statement list */

/* Function-related nodes */
ASTNode* createFuncDef(char* name, int isVoid, ASTNode* params, ASTNode* body);
ASTNode* createParam(char* name, int isArray);
ASTNode* createParamList(ASTNode* list, ASTNode* param);        /* Append to list (may be NULL) */
ASTNode* createFuncCall(char* name, ASTNode* args);
ASTNode* createArgList(ASTNode* list, ASTNode* expr);           /* Append to list (may be NULL) */
ASTNode* createReturn(ASTNode* expr);

/* Control flow nodes */
ASTNode* createBlock(ASTNode* stmts);
ASTNode* createIf(ASTNode* cond, ASTNode* thenStmt, ASTNode* elseStmt);
ASTNode* createWhile(ASTNode* cond, ASTNode* body);
ASTNode* createFor(ASTNode* init, ASTNode* cond, ASTNode* update, ASTNode* body);
ASTNode* createBreak(void);
ASTNode* createContinue(void);

/* Array nodes.  There is no createArrayParam: an array parameter has no
 * size and reserves no elements, so it is a NODE_PARAM with isArray set. */
ASTNode* createArrayDecl(char* name, int size);
ASTNode* createArrayIndex(char* name, ASTNode* index);
ASTNode* createArrayAssign(ASTNode* target, ASTNode* value);    /* a[i] = value */

/* Text form of a packed operator code (see createBinOp) */
const char* opText(char op);

/* AST DISPLAY FUNCTION */
void printAST(ASTNode* node, int level);                        /* Pretty-print the AST */

#endif