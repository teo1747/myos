/* user/note/syntax_test.c -- the highlighter, which is a pure function and so
 * is testable without a screen.
 *
 * The cases that matter are the ones an editor meets and a compiler never
 * does: a half-typed string, a block comment with no end, a line that is
 * nothing but whitespace. A compiler is allowed to refuse those. This must
 * colour them and hand back spans that still cover the line exactly, because
 * the editor draws what it is given and a gap or an overlap is a corrupted
 * line on screen. */
#include "syntax.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
static void ok(int c, const char *what) {
    printf("  %s: %s\n", c ? "ok  " : "FAIL", what);
    if (!c) g_fail++;
}

/* Every test asserts this, whatever else it asserts. */
static int covers(const char *line, struct syn_span *sp, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        if (sp[i].len <= 0) return 0;
        total += sp[i].len;
    }
    return n == 0 || total == (int)strlen(line);
}

static int role_at(const char *line, struct syn_span *sp, int n, int col) {
    int off = 0;
    (void)line;
    for (int i = 0; i < n; i++) {
        if (col < off + sp[i].len) return sp[i].role;
        off += sp[i].len;
    }
    return -1;
}

#define RUN(lang, str) \
    n = syn_line((lang), (str), (int)strlen(str), sp, 64, &st); \
    ok(covers((str), sp, n), "spans cover \"" str "\" exactly")

int main(void) {
    struct syn_span sp[64];
    int n, st = SYN_ST_NONE;

    printf("syntax_test\n");

    st = SYN_ST_NONE;
    RUN(SYN_C, "int x = 42;");
    ok(role_at("int x = 42;", sp, n, 0) == SYN_TYPE,    "`int` is a type");
    ok(role_at("int x = 42;", sp, n, 8) == SYN_NUMBER,  "`42` is a number");

    st = SYN_ST_NONE;
    RUN(SYN_C, "return \"hi\"; // done");
    ok(role_at("return \"hi\"; // done", sp, n, 0) == SYN_KEYWORD, "`return` is a keyword");
    ok(role_at("return \"hi\"; // done", sp, n, 8) == SYN_STRING,  "the string is a string");
    ok(role_at("return \"hi\"; // done", sp, n, 14) == SYN_COMMENT, "the trailing // is a comment");

    /* A string with an escaped quote in it. Get this wrong and the rest of the
     * line is coloured as code, which is the commonest highlighter bug there
     * is. */
    st = SYN_ST_NONE;
    RUN(SYN_C, "p = \"a\\\"b\"; x");
    ok(role_at("p = \"a\\\"b\"; x", sp, n, 5) == SYN_STRING, "an escaped quote does not end the string");
    ok(role_at("p = \"a\\\"b\"; x", sp, n, 12) != SYN_STRING, "...and the code after it is code");

    /* The multi-line cases: state in, state out. */
    st = SYN_ST_NONE;
    RUN(SYN_C, "x; /* open");
    ok(st == SYN_ST_BLOCK_COMMENT, "an unterminated /* leaves the block-comment state set");
    RUN(SYN_C, "still inside");
    ok(n == 1 && sp[0].role == SYN_COMMENT, "the next line is entirely comment");
    ok(st == SYN_ST_BLOCK_COMMENT, "...and stays open");
    RUN(SYN_C, "closes */ int y;");
    ok(st == SYN_ST_NONE, "the closing */ clears the state");
    ok(role_at("closes */ int y;", sp, n, 10) == SYN_TYPE, "code after the close is code again");

    /* An editor sees these constantly; a compiler would refuse them. */
    st = SYN_ST_NONE;
    RUN(SYN_C, "char *s = \"unterminated");
    ok(role_at("char *s = \"unterminated", sp, n, 12) == SYN_STRING,
       "an unterminated string is a string to the end of the line");
    st = SYN_ST_NONE;
    RUN(SYN_C, "");
    ok(n == 0, "an empty line asks for no spans");
    st = SYN_ST_NONE;
    RUN(SYN_C, "    ");
    st = SYN_ST_NONE;
    RUN(SYN_C, "#include <stdio.h>  /* io */");
    ok(role_at("#include <stdio.h>  /* io */", sp, n, 2) == SYN_PREPROC, "#include is preprocessor");
    ok(role_at("#include <stdio.h>  /* io */", sp, n, 22) == SYN_COMMENT,
       "...and a comment after it is still a comment");

    /* The compiler's own table is what supplies these. */
    st = SYN_ST_NONE;
    RUN(SYN_C, "_Static_assert(1, \"\");");
    ok(role_at("_Static_assert(1, \"\");", sp, n, 0) == SYN_KEYWORD,
       "_Static_assert is a keyword (from EmbCC's table)");
    st = SYN_ST_NONE;
    RUN(SYN_C, "size_t n; my_own_t v;");
    ok(role_at("size_t n; my_own_t v;", sp, n, 0) == SYN_TYPE, "size_t is a type");
    ok(role_at("size_t n; my_own_t v;", sp, n, 10) == SYN_TYPE, "a name ending _t is a type");

    st = SYN_ST_NONE;
    RUN(SYN_PY, "def f(): return 1  # hi");
    ok(role_at("def f(): return 1  # hi", sp, n, 0) == SYN_KEYWORD, "python def");
    ok(role_at("def f(): return 1  # hi", sp, n, 19) == SYN_COMMENT, "python # comment");

    st = SYN_ST_NONE;
    n = syn_line(SYN_PLAIN, "int x;", 6, sp, 64, &st);
    ok(n == 0, "a plain-text file gets no spans at all");

    /* Language from the name. */
    ok(syn_lang_of("/data/a.c") == SYN_C, "a .c file is C");
    ok(syn_lang_of("/data/a.H") == SYN_C, "a .H file is C (case-insensitive)");
    ok(syn_lang_of("/data/readme") == SYN_PLAIN, "no extension is plain");

    printf("=== syntax_test: %s (%d failure%s) ===\n",
           g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail != 0;
}
