/* =========================================================================
 * CST-405  ·  Project 4  ·  Loops and Optimization
 * FILE: parser.y   —   Phase 2 — Syntax analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *              ^^^^^^  this file
 *
 * PHASE
 *   Phase 2, syntax analysis.
 *
 * RECEIVES
 *   Tokens from the scanner.
 *
 * PRODUCES
 *   The abstract syntax tree (`root`), or syntax errors with line and column.
 *
 * CREDITS
 *   Prologue (%union, %token, precedence table, yyerror) from the CST-405 course starter code.
 *   Grammar rules, semantic actions and error productions, including the
 *   loop rules: Alissa Head.
 * ========================================================================= */

%{
/* PHASE 2 — SYNTAX ANALYSIS
 *
 * The parser answers one question: do these tokens form a legal program?
 * Bison builds an LALR(1) bottom-up parser from the grammar below.  It reads
 * tokens left to right, shifts them onto a stack, and REDUCES whenever the
 * top of the stack matches the right-hand side of a rule.
 *
 * Answering "yes" is not enough, though — the rest of the compiler needs the
 * program's STRUCTURE.  So each rule carries a semantic action in { } that
 * builds one node of the abstract syntax tree.  $1, $2, ... are the values of
 * the symbols on the right-hand side; $$ is the value this rule hands back.
 *
 *      expr '+' expr   { $$ = createBinOp('+', $1, $3); }
 *       │        │                              │   └── right operand
 *       │        └── $3                         └────── left operand
 *       └── $1
 *
 * By the time yyparse() returns, the tree has been built bottom-up beneath us
 * and `root` points at the whole program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

/* External declarations for lexer interface */
extern int yylex();
extern int yyparse();
extern FILE* yyin;
extern int yylineno;  /* Line number from scanner */

void yyerror(const char* s);
ASTNode* root = NULL;
static int syntaxErrors = 0;   /* error productions recover, so count errors ourselves */

/* ---- SYNTAX ERROR REPORTING (helpers are defined after the grammar) ------
 * Bison detects a mistake and calls yyerror() with its own wording.  We do
 * not print that immediately: we HOLD it, because an error production may be
 * able to say something far more useful.  Whichever way it goes, exactly one
 * message is printed per mistake, with the line, the column, the source line
 * and a caret under the spot:
 *
 *   diagnose()          an error production explains it: print OUR message at
 *                       the spot the missing piece belonged
 *   flushSyntaxError()  nobody could explain it: print Bison's message at the
 *                       token it complained about
 *
 * Positions come from scanner.l (see trackPosition there).                   */
#include <stdarg.h>
void flushSyntaxError(void);
static void diagnose(const char* fmt, ...);
static int  endsStatement(int tok);

/* Use inside an error production for a missing ';'.  Only claim the error if
 * the token we stopped at plausibly STARTS the next statement — otherwise the
 * real mistake is something else and Bison's own message is the honest one. */
#define MISSING_SEMI(...) \
    do { if (endsStatement(yychar)) { diagnose(__VA_ARGS__); yyerrok; } \
         else flushSyntaxError(); } while (0)
%}

/* SEMANTIC VALUES UNION */
%union {
    int num;
    char* str;
    struct ASTNode* node;
}

/* Ask Bison to list what it EXPECTED, e.g.
 *     syntax error, unexpected ')', expecting number or identifier
 * instead of a bare "syntax error".  The quoted names below are what it prints. */
%define parse.error verbose

/* TOKEN DECLARATIONS */
%token <num> NUM "number"
%token <str> ID "identifier"
%token INT "int" VOID "void" PRINT "print" RETURN "return"
%token IF "if" ELSE "else"
%token LE "<=" GE ">=" EQ "==" NE "!="

/* Loop keywords (Project 4) */
%token WHILE "while" FOR "for" BREAK "break" CONTINUE "continue"

/* Reserved for a later milestone: the scanner returns them, but no rule uses
 * them yet, so `int switch;` is a syntax error that names the keyword. */
%token SWITCH "switch" CASE "case" DEFAULT "default"

/* Track each token's source line (set in scanner.l) so that a node can record
 * the line its construct STARTED on, available here as @1, @2, ... */
%locations

/* NON-TERMINAL TYPES: every non-terminal that yields an AST node */
%type <node> program top_list top_item func_def params param_list param
%type <node> stmt_list stmt block decl assign expr print_stmt return_stmt
%type <node> if_stmt func_call args arg_list
%type <node> while_stmt for_stmt for_assign for_cond jump_stmt

/* OPERATOR PRECEDENCE AND ASSOCIATIVITY
 * Listed from lowest to highest precedence.
 *
 * DANGLING-ELSE RESOLUTION:
 *   The grammar has an ambiguity: in  if (c) if (c) s  else s
 *   the "else" can bind to either "if".  The standard resolution
 *   is that else binds to the nearest (innermost) if.
 *
 *   We express this by giving the if-without-else rule a lower
 *   precedence (%prec LOWER_THAN_ELSE) than the ELSE token, so
 *   when bison sees ELSE it shifts (attaches it to the inner if)
 *   rather than reducing (closing the outer if first).
 */
%precedence LOWER_THAN_ELSE
%precedence ELSE
%left EQ NE                       /* binds looser than the orderings, as in C */
%left '<' '>' LE GE
%left '+' '-'
%left '*' '/'
%precedence UMINUS                   /* -2 * 3 is (-2) * 3; see the rule's comment */

%%

/* --------------------------------------------------------------------
 * THE GRAMMAR (as implemented below)
 *
 *     program     ->  top_list
 *     top_list    ->  top_item  |  top_list top_item
 *     top_item    ->  func_def  |  stmt
 *     func_def    ->  ('int' | 'void') ID '(' params ')' block
 *     params      ->  (empty)  |  'void'  |  param_list
 *     param_list  ->  param  |  param_list ',' param
 *     param       ->  'int' ID  |  'int' ID '[' ']'
 *     block       ->  '{' '}'  |  '{' stmt_list '}'
 *     stmt_list   ->  stmt  |  stmt_list stmt
 *     stmt        ->  decl  |  assign  |  print_stmt  |  return_stmt
 *                  |  if_stmt  |  while_stmt  |  for_stmt  |  jump_stmt
 *                  |  block  |  func_call ';'  |  error ';'
 *     decl        ->  'int' ID ';'  |  'int' ID '=' expr ';'
 *                  |  'int' ID '[' NUM ']' ';'
 *     assign      ->  ID '=' expr ';'  |  ID '[' expr ']' '=' expr ';'
 *     return_stmt ->  'return' expr ';'  |  'return' ';'
 *     if_stmt     ->  'if' '(' expr ')' stmt  [ 'else' stmt ]
 *     while_stmt  ->  'while' '(' expr ')' stmt
 *     for_stmt    ->  'for' '(' for_assign ';' for_cond ';' for_assign ')' stmt
 *     for_assign  ->  (empty)  |  ID '=' expr  |  ID '[' expr ']' '=' expr
 *     for_cond    ->  (empty)  |  expr
 *     jump_stmt   ->  'break' ';'  |  'continue' ';'
 *     expr        ->  NUM  |  ID  |  ID '[' expr ']'  |  func_call
 *                  |  expr ('+' | '-' | '*' | '/') expr
 *                  |  expr ('<' | '>' | '<=' | '>=' | '==' | '!=') expr
 *                  |  '-' expr  |  '(' expr ')'
 *     func_call   ->  ID '(' args ')'
 *     args        ->  (empty)  |  arg_list
 *     arg_list    ->  expr  |  arg_list ',' expr
 *     print_stmt  ->  'print' '(' expr ')' ';'
 *
 * The expression grammar is ambiguous on purpose; the precedence table above
 * resolves it: '*' and '/' bind tighter than '+' and '-', and all four are
 * left associative, so  a - b - c  parses as (a - b) - c.  Writing the grammar
 * this way keeps it short and keeps the AST free of the extra
 * term/factor nodes an unambiguous grammar would need.
 *
 * DESIGN DECISIONS
 *
 *   One AST node per action.  Every rule's action builds at most one node
 *   with the constructors in ast.c and hands it up as $$.  Parentheses
 *   build NOTHING ( '(' expr ')' just passes $2 up): the tree's shape
 *   already records the grouping, which is exactly why it is an ABSTRACT
 *   syntax tree.
 *
 *   'int x = e;' is sugar.  It becomes a DECL followed by an ASSIGN, so no
 *   later phase needs a special case for initialized declarations.
 *
 *   top_item accepts a statement as well as a function.  That is what keeps
 *   the Project 2 programs compiling: a file with no functions is still one
 *   implicit main().  In a file that DOES define functions, semantic.c
 *   rejects executable statements outside them.  The rule lives there, not
 *   here, because it can then be reported with a useful message.
 *
 *   if/else is not on the Project 3 feature list, but factorial(5) is, and
 *   recursion cannot stop without a conditional.  So `if` and the comparison
 *   operators are here, as the smallest addition that meets that requirement.
 *
 *   Memory.  The scanner strdup's every identifier; the AST constructors
 *   copy it again, so each rule that receives an ID frees its copy.
 *
 *   `root` is assigned in exactly one place, the `program` rule, and only
 *   when no syntax error was seen (even a recovered one).
 *
 *   Error productions.  Instead of Bison's generic "syntax error", the
 *   rules ending in `error` recognise the common mistakes — a missing ';',
 *   a missing ')', an operator with no right operand — and report them with
 *   the line and column where the missing token belonged.  `error ';'` is
 *   panic-mode recovery: skip to the next ';' and keep parsing, so ONE run
 *   reports every syntax error in the file (see tests/t2_06_three_errors.cm).
 * -------------------------------------------------------------------- */

/* -- PROGRAM ------------------------------------------------------------
 * The ONLY rule that assigns the global `root`. */
program:
    top_list            { if (syntaxErrors > 0) YYABORT;   /* recovered, but still a failed parse */
                          root = $1; $$ = $1; }
    ;

/* -- TOP LEVEL ----------------------------------------------------------
 * The file as a list.  Same shape as stmt_list, so a file of plain statements
 * builds exactly the tree it built in Project 2. */
top_list:
    top_item            { $$ = $1; }
    | top_list top_item { $$ = ($2 == NULL) ? $1 : ($1 == NULL) ? $2
                                            : createStmtList($1, $2); }
    ;

top_item:
    func_def            { $$ = $1; }
    | stmt              { $$ = $1; }
    ;

/* -- FUNCTION DEFINITION -> NODE_FUNC_DEF --------------------------------
 *   int f(int a, int b[]) { ... }     void g() { ... }
 * `int ID` also begins a variable declaration.  An LALR(1) parser sees no
 * conflict: it shifts both rules' common prefix and the next token, '(' here
 * or ';', '=', '[' in decl, picks the rule.
 *
 * The block is unwrapped ($6->data.block): the body shares one scope with the
 * parameters, so `int f(int a) { int a; }` is a duplicate declaration. */
func_def:
    INT ID '(' params ')' block
        { $$ = createFuncDef($2, 0, $4, $6->data.block);
          $$->lineno = @2.first_line; free($2); }
    | VOID ID '(' params ')' block
        { $$ = createFuncDef($2, 1, $4, $6->data.block);
          $$->lineno = @2.first_line; free($2); }
    ;

/* -- PARAMETERS -> NODE_PARAM_LIST of NODE_PARAM -------------------------
 * NULL means none: `f()` and `f(void)` mean the same thing. */
params:
    %empty              { $$ = NULL; }
    | VOID              { $$ = NULL; }
    | param_list        { $$ = $1; }
    ;

param_list:
    param                       { $$ = createParamList(NULL, $1); }
    | param_list ',' param      { $$ = createParamList($1, $3); }
    ;

/* `int a[]` has no size, because arrays are passed BY REFERENCE: the callee
 * gets the base address of the caller's array, so nothing here depends on
 * how long that array is. */
param:
    INT ID              { $$ = createParam($2, 0); $$->lineno = @2.first_line; free($2); }
    | INT ID '[' ']'    { $$ = createParam($2, 1); $$->lineno = @2.first_line; free($2); }
    ;

/* -- BLOCK -> NODE_BLOCK -------------------------------------------------
 * A node with no job at run time: it exists because it opens a SCOPE. */
block:
    '{' '}'             { $$ = createBlock(NULL); }
    | '{' stmt_list '}' { $$ = createBlock($2); }
    ;

/* -- STATEMENT LIST -----------------------------------------------------
 * Left recursive: $1 is the list built so far, $2 is the new statement.
 * A statement that failed to parse (error production below) comes back as
 * NULL, so we skip it instead of putting a hole in the tree. */
stmt_list:
    stmt                { $$ = $1; }
    | stmt_list stmt    { $$ = ($2 == NULL) ? $1 : ($1 == NULL) ? $2
                                            : createStmtList($1, $2); }
    ;

stmt:
    decl                { $$ = $1; }
    | assign            { $$ = $1; }
    | print_stmt        { $$ = $1; }
    | return_stmt       { $$ = $1; }
    | if_stmt           { $$ = $1; }
    | while_stmt        { $$ = $1; }
    | for_stmt          { $$ = $1; }
    | jump_stmt         { $$ = $1; }
    | block             { $$ = $1; }
    | func_call ';'     { $$ = $1; }            /* called for its effect */
    /* PANIC-MODE RECOVERY: when no more specific rule fits, skip ahead to the
     * next ';' and carry on.  Without this the parser would give up at the
     * first mistake and every later error would go unreported. */
    | error ';'         { flushSyntaxError(); $$ = NULL; yyerrok; }
    ;

/* -- DECLARATIONS -------------------------------------------------------
 *   int x;         BEGINNER: one node, a DECL
 *   int x = 5;     a DECL followed by an ASSIGN, glued into a small list
 *                  (the same tree you would get from  int x;  x = 5;) */
decl:
    INT ID ';'
        { $$ = createDecl("int", $2); free($2); }
    | INT ID '=' expr ';'
        { $$ = createStmtList(createDecl("int", $2), createAssign($2, $4));
          free($2); }
    /* int a[10];  -> NODE_ARRAY_DECL.  The size is a literal: the compiler
     * must know at compile time how many bytes to reserve.  The negative form
     * is grammatical only so that semantic.c can reject `int a[-3];` with a
     * message about array sizes rather than a generic syntax error. */
    | INT ID '[' NUM ']' ';'
        { $$ = createArrayDecl($2, $4); $$->lineno = @2.first_line; free($2); }
    | INT ID '[' '-' NUM ']' ';'
        { $$ = createArrayDecl($2, -$5); $$->lineno = @2.first_line; free($2); }
    | INT ID '[' NUM ']' error
        { MISSING_SEMI("missing ';' after the declaration of array '%s'", $2);
          free($2); $$ = NULL; }
    /* error productions: name the mistake instead of "syntax error" */
    | INT ID error
        { MISSING_SEMI("missing ';' after the declaration of '%s'", $2);
          free($2); $$ = NULL; }
    | INT ID '=' expr error
        { MISSING_SEMI("missing ';' after the initial value of '%s'", $2);
          free($2); $$ = NULL; }
    ;

/* -- ASSIGNMENT ---------------------------------------------------------
 *   x = 5;   x = y + 2;   x = (a + b) - 1; */
assign:
    ID '=' expr ';'
        { $$ = createAssign($1, $3); free($1); }
    | ID '=' expr error
        { MISSING_SEMI("missing ';' after the assignment to '%s'", $1);
          free($1); $$ = NULL; }
    /* a[i] = expr;  -> NODE_ASSIGN whose target is a NODE_ARRAY_INDEX tree */
    | ID '[' expr ']' '=' expr ';'
        { ASTNode* target = createArrayIndex($1, $3);
          target->lineno = @1.first_line;
          $$ = createArrayAssign(target, $6);
          $$->lineno = @1.first_line; free($1); }
    | ID '[' expr ']' '=' expr error
        { MISSING_SEMI("missing ';' after the assignment to an element of '%s'", $1);
          free($1); $$ = NULL; }
    ;

/* -- RETURN -> NODE_RETURN -----------------------------------------------
 * Both forms parse anywhere; whether one is LEGAL here (inside a function,
 * and matching its return type) is a question of meaning, so semantic.c
 * decides it. */
return_stmt:
    RETURN expr ';'     { $$ = createReturn($2);   $$->lineno = @1.first_line; }
    | RETURN ';'        { $$ = createReturn(NULL); $$->lineno = @1.first_line; }
    | RETURN expr error
        { MISSING_SEMI("missing ';' after the return statement"); $$ = NULL; }
    ;

/* -- IF -> NODE_IF -------------------------------------------------------
 * %prec LOWER_THAN_ELSE resolves the dangling else as described above. */
if_stmt:
    IF '(' expr ')' stmt %prec LOWER_THAN_ELSE
        { $$ = createIf($3, $5, NULL); $$->lineno = @1.first_line; }
    | IF '(' expr ')' stmt ELSE stmt
        { $$ = createIf($3, $5, $7);   $$->lineno = @1.first_line; }
    ;

/* -- WHILE -> NODE_WHILE -------------------------------------------------
 *   while (cond) body
 * The body is any statement, braced or not.  No dangling-else problem arises:
 * a while has no optional tail, so an `else` can only belong to an `if`. */
while_stmt:
    WHILE '(' expr ')' stmt
        { $$ = createWhile($3, $5); $$->lineno = @1.first_line; }
    /* A broken condition: report it, then resume at the ')' so the body is
     * still parsed.  Resuming at the next ';' instead (the general recovery
     * rule) would swallow the body and hide the errors inside it. */
    | WHILE '(' error ')' stmt
        { flushSyntaxError(); yyerrok; $$ = NULL; }
    ;

/* -- FOR -> NODE_FOR -----------------------------------------------------
 *   for (init; cond; update) body
 * Each of the three parts may be empty, and an empty condition means "always
 * true".  init and update are ASSIGNMENTS, never declarations or arbitrary
 * expressions: assignment is a statement in this language, not an
 * expression, so there is no i++ and no comma operator to parse.
 *
 * DESIGN DECISION: for gets its own node instead of being rewritten into a
 * while by the parser.  The desugared form  init; while (cond) { body; update; }
 * puts the update at the END OF THE BODY, so a `continue` (which jumps over
 * the rest of the body) would also skip the update and never advance the
 * loop.  Keeping the update separate gives `continue` a place to land. */
for_stmt:
    FOR '(' for_assign ';' for_cond ';' for_assign ')' stmt
        { $$ = createFor($3, $5, $7, $9); $$->lineno = @1.first_line; }
    /* A broken header, such as a missing ';' or a declaration in the init:
     * one message, then resume at ')' as for while above. */
    | FOR '(' error ')' stmt
        { flushSyntaxError(); yyerrok; $$ = NULL; }
    ;

for_assign:
    %empty                      { $$ = NULL; }
    | ID '=' expr
        { $$ = createAssign($1, $3); $$->lineno = @1.first_line; free($1); }
    | ID '[' expr ']' '=' expr
        { ASTNode* target = createArrayIndex($1, $3);
          target->lineno = @1.first_line;
          $$ = createArrayAssign(target, $6);
          $$->lineno = @1.first_line; free($1); }
    ;

for_cond:
    %empty                      { $$ = NULL; }      /* no test: loop until a break */
    | expr                      { $$ = $1; }
    ;

/* -- BREAK, CONTINUE -> NODE_BREAK, NODE_CONTINUE ------------------------
 * Both parse anywhere a statement can appear.  Whether one is LEGAL (inside a
 * loop) depends on context the grammar cannot see, so semantic.c decides. */
jump_stmt:
    BREAK ';'           { $$ = createBreak();    $$->lineno = @1.first_line; }
    | CONTINUE ';'      { $$ = createContinue(); $$->lineno = @1.first_line; }
    | BREAK error
        { MISSING_SEMI("missing ';' after break"); $$ = NULL; }
    | CONTINUE error
        { MISSING_SEMI("missing ';' after continue"); $$ = NULL; }
    ;

/* -- EXPRESSIONS --------------------------------------------------------
 *   NUM, ID              leaves of the tree
 *   expr '+' expr        the starter operator; left associative
 *   expr '-' expr        same shape, different operator char
 *   expr '*' expr        bind tighter than + and - (see the precedence table)
 *   expr '/' expr
 *   comparisons          yield 1 or 0.  Two-character operators are packed
 *                        into one char: 'L' <=, 'G' >=, 'E' ==, 'N' != (ast.c)
 *   '-' expr             unary minus: NODE_BINOP with op 'u' and no right
 *                        operand.  %prec UMINUS is what makes -2 * 3 parse as
 *                        (-2) * 3: without it the rule would inherit binary
 *                        '-' precedence, which is lower than '*'.
 *   '(' expr ')'         no node of its own: parentheses only steer the
 *                        parse, so we pass the inner tree straight up
 *   ID '[' expr ']'      an array element -> NODE_ARRAY_INDEX
 *   func_call            a call used for its value -> NODE_FUNC_CALL */
expr:
    NUM                 { $$ = createNum($1); }
    | ID                { $$ = createVar($1); $$->lineno = @1.first_line; free($1); }
    | ID '[' expr ']'   { $$ = createArrayIndex($1, $3); $$->lineno = @1.first_line; free($1); }
    | func_call         { $$ = $1; }
    | expr '+' expr     { $$ = createBinOp('+', $1, $3); }
    | expr '-' expr     { $$ = createBinOp('-', $1, $3); }
    | expr '*' expr     { $$ = createBinOp('*', $1, $3); }
    | expr '/' expr     { $$ = createBinOp('/', $1, $3); }
    | expr '<' expr     { $$ = createBinOp('<', $1, $3); }
    | expr '>' expr     { $$ = createBinOp('>', $1, $3); }
    | expr LE expr      { $$ = createBinOp('L', $1, $3); }
    | expr GE expr      { $$ = createBinOp('G', $1, $3); }
    | expr EQ expr      { $$ = createBinOp('E', $1, $3); }
    | expr NE expr      { $$ = createBinOp('N', $1, $3); }
    | '-' expr %prec UMINUS { $$ = createBinOp('u', $2, NULL); }
    | '(' expr ')'      { $$ = $2; }
    /* an operator with nothing after it:  y = 5 + ;  */
    | expr '+' error    { diagnose("expected a value after '+'");  yyerrok; $$ = $1; }
    | expr '-' error    { diagnose("expected a value after '-'");  yyerrok; $$ = $1; }
    | expr '*' error    { diagnose("expected a value after '*'");  yyerrok; $$ = $1; }
    | expr '/' error    { diagnose("expected a value after '/'");  yyerrok; $$ = $1; }
    ;

/* -- FUNCTION CALL -> NODE_FUNC_CALL -------------------------------------
 * Arguments stay in source order: which value lands in which $a register
 * is part of the calling convention. */
func_call:
    ID '(' args ')'
        { $$ = createFuncCall($1, $3); $$->lineno = @1.first_line; free($1); }
    ;

args:
    %empty              { $$ = NULL; }
    | arg_list          { $$ = $1; }
    ;

arg_list:
    expr                    { $$ = createArgList(NULL, $1); }
    | arg_list ',' expr     { $$ = createArgList($1, $3); }
    ;

/* -- PRINT --------------------------------------------------------------
 *   print(x);   print(a + b); */
print_stmt:
    PRINT '(' expr ')' ';'
        { $$ = createPrint($3); }
    | PRINT '(' expr ')' error
        { MISSING_SEMI("missing ';' after the print statement");
          $$ = NULL; }
    | PRINT '(' expr error ';'
        { diagnose("missing ')' to close the '(' after print");
          yyerrok; $$ = NULL; }
    ;

%%

/* ==========================================================================
 * ERROR REPORTING — the helpers declared in the prologue
 * ========================================================================*/

/* The message Bison produced, held until we know whether an error production
 * can explain it better.  `pending*` is where Bison noticed the problem (the
 * token it stopped at); `gap*` is the spot just past the token BEFORE that,
 * which is where a missing ';' or ')' belonged. */
static int  pendingActive = 0;
static char pendingMsg[256];
static int  pendingLine, pendingCol;
static int  gapLine, gapCol;

extern int tokLine, tokCol, prevEndLine, prevEndCol;   /* scanner.l */

/* Echo the offending source line with a caret under the column.  The scanner
 * has already buffered the file, so it is safe to borrow yyin for a moment as
 * long as the read position is put back afterwards. */
static void showSource(int line, int col) {
    if (!yyin) return;
    long resume = ftell(yyin);
    if (resume < 0) return;                       /* not a seekable file */
    fseek(yyin, 0, SEEK_SET);

    char text[512];
    for (int n = 1; fgets(text, sizeof text, yyin); n++) {
        if (n != line) continue;
        text[strcspn(text, "\r\n")] = '\0';
        fprintf(stderr, "  %4d | %s\n", line, text);
        fprintf(stderr, "       | ");
        for (int i = 0; i < col - 1; i++)
            fputc(i < (int)strlen(text) && text[i] == '\t' ? '\t' : ' ', stderr);
        fprintf(stderr, "^\n");
        break;
    }
    fseek(yyin, resume, SEEK_SET);
}

static void printError(int line, int col, const char* msg) {
    fprintf(stderr, "Syntax Error at line %d, column %d: %s\n", line, col, msg);
    showSource(line, col);
}

/* Print Bison's own message, if nobody has claimed it. */
void flushSyntaxError(void) {
    if (!pendingActive) return;
    pendingActive = 0;
    printError(pendingLine, pendingCol, pendingMsg);
}

/* An error production explains the pending error: print OUR message, at the
 * spot where the missing piece belonged, and drop Bison's. */
static void diagnose(const char* fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pendingActive = 0;
    printError(gapLine, gapCol, msg);
}

/* Could this token be the FIRST token of a new statement? */
static int endsStatement(int tok) {
    return tok == INT || tok == VOID || tok == PRINT || tok == RETURN ||
           tok == IF  || tok == ID   || tok == '{'   || tok == '}'    ||
           tok == WHILE || tok == FOR || tok == BREAK || tok == CONTINUE || tok == YYEOF;
}

/* Called by Bison the moment it detects a mistake. */
void yyerror(const char* s) {
    flushSyntaxError();                 /* an earlier one nobody explained */
    syntaxErrors++;
    pendingActive = 1;
    snprintf(pendingMsg, sizeof pendingMsg, "%s", s);
    pendingLine = tokLine;   pendingCol = tokCol;
    gapLine = prevEndLine;   gapCol = prevEndCol;
}
