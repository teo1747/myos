/* user/note/syntax.c -- see syntax.h. A lexer, not a parser: it answers "what
 * does this look like", which is all a colour needs and all that can be decided
 * from one line. */

#include "syntax.h"
#include <string.h>

/* ---- languages ---------------------------------------------------------- */

static int ends_with(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    if (m > n) return 0;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

enum syn_lang syn_lang_of(const char *path) {
    if (!path || !path[0]) return SYN_PLAIN;
    if (ends_with(path, ".c") || ends_with(path, ".h") ||
        ends_with(path, ".cc") || ends_with(path, ".cpp") ||
        ends_with(path, ".hpp") || ends_with(path, ".cxx")) return SYN_C;
    if (ends_with(path, ".py"))  return SYN_PY;
    if (ends_with(path, ".js") || ends_with(path, ".json")) return SYN_JS;
    if (ends_with(path, ".sh") || ends_with(path, ".bash")) return SYN_SH;
    if (ends_with(path, ".md"))  return SYN_MD;
    return SYN_PLAIN;
}

const char *syn_lang_name(enum syn_lang l) {
    switch (l) {
        case SYN_C:  return "C";
        case SYN_PY: return "Python";
        case SYN_JS: return "JavaScript";
        case SYN_SH: return "Shell";
        case SYN_MD: return "Markdown";
        default:     return "Plain text";
    }
}

/* ---- word tables -------------------------------------------------------- */

/* C's words come from EmbCC's lexer, generated rather than retyped -- see
 * tools/mkkeywords.py for why the WORDS are shared with the compiler and the
 * LEXING deliberately is not. */
#include "ckeywords.h"
#define KW_C C_KEYWORDS
#define TY_C C_TYPES

static const char *const KW_PY[] = {
    "def","class","if","elif","else","for","while","in","not","and","or",
    "return","yield","import","from","as","try","except","finally","raise",
    "with","lambda","pass","break","continue","global","nonlocal","assert",
    "del","is","async","await", 0
};
static const char *const TY_PY[] = { "True","False","None","self","int","str","float","list","dict","set","tuple","bytes", 0 };
static const char *const KW_JS[] = {
    "function","var","let","const","if","else","for","while","do","switch",
    "case","default","break","continue","return","new","delete","typeof",
    "instanceof","in","of","class","extends","this","try","catch","finally",
    "throw","async","await","yield","import","export","from", 0
};
static const char *const TY_JS[] = { "true","false","null","undefined","NaN","Infinity", 0 };
static const char *const KW_SH[] = {
    "if","then","else","elif","fi","for","while","do","done","case","esac",
    "function","return","in","local","export","source","echo","exit", 0
};

static int word_in(const char *const *tab, const char *s, int n) {
    if (!tab) return 0;
    for (int i = 0; tab[i]; i++) {
        int k = 0;
        while (k < n && tab[i][k] && tab[i][k] == s[k]) k++;
        if (k == n && !tab[i][k]) return 1;
    }
    return 0;
}

/* ---- character classes -------------------------------------------------- */

static int is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* ---- the lexer ---------------------------------------------------------- */

/* Append a span, merging with the previous one when the role is the same. The
 * merge is not cosmetic: the editor draws one text node per span, and a line of
 * code emitted character by character would be a hundred nodes to lay out. */
static void push(struct syn_span *out, int max, int *n, int len, int role) {
    if (len <= 0) return;
    if (*n > 0 && out[*n - 1].role == role) { out[*n - 1].len += len; return; }
    if (*n >= max) { out[max - 1].len += len; return; }   /* full: widen the last */
    out[*n].len = len;
    out[*n].role = (unsigned char)role;
    (*n)++;
}

int syn_line(enum syn_lang lang, const char *s, int n,
             struct syn_span *out, int max, int *state) {
    int ns = 0;
    int st = state ? *state : SYN_ST_NONE;
    if (lang == SYN_PLAIN || max < 2 || n <= 0) {
        if (state) *state = SYN_ST_NONE;
        return 0;                       /* 0 spans == draw it plainly */
    }

    const char *const *kw = 0, *const *ty = 0;
    int hash_comment = 0, slash_comment = 0, block_comment = 0;
    switch (lang) {
        case SYN_C:  kw = KW_C;  ty = TY_C;  slash_comment = 1; block_comment = 1; break;
        case SYN_JS: kw = KW_JS; ty = TY_JS; slash_comment = 1; block_comment = 1; break;
        case SYN_PY: kw = KW_PY; ty = TY_PY; hash_comment = 1; break;
        case SYN_SH: kw = KW_SH; hash_comment = 1; break;
        case SYN_MD: default: break;
    }

    int i = 0;

    /* A block comment opened on an earlier line runs until it closes. */
    if (st == SYN_ST_BLOCK_COMMENT) {
        int j = 0;
        while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
        if (j + 1 < n) { push(out, max, &ns, j + 2, SYN_COMMENT); i = j + 2; st = SYN_ST_NONE; }
        else           { push(out, max, &ns, n, SYN_COMMENT); if (state) *state = st; return ns; }
    }

    /* Markdown is a different shape of thing: whole-line roles, no tokens. */
    if (lang == SYN_MD) {
        if (n && s[0] == '#')      { push(out, max, &ns, n, SYN_KEYWORD); }
        else if (n && s[0] == '>') { push(out, max, &ns, n, SYN_COMMENT); }
        else if (n >= 3 && (!memcmp(s, "```", 3))) { push(out, max, &ns, n, SYN_STRING); }
        else                       { push(out, max, &ns, n, SYN_TEXT); }
        if (state) *state = SYN_ST_NONE;
        return ns;
    }

    /* A preprocessor line is one role from the hash to the end -- except that
     * its trailing comment is still a comment, which is common enough in this
     * codebase to be worth getting right. */
    if (lang == SYN_C) {
        int k = i;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == '#') {
            int c = k;
            while (c + 1 < n && !(s[c] == '/' && (s[c + 1] == '/' || s[c + 1] == '*'))) c++;
            if (c + 1 < n) {
                push(out, max, &ns, c, SYN_PREPROC);
                i = c;                       /* fall through to the comment scan */
            } else {
                push(out, max, &ns, n, SYN_PREPROC);
                if (state) *state = SYN_ST_NONE;
                return ns;
            }
        }
    }

    while (i < n) {
        char c = s[i];

        if (slash_comment && c == '/' && i + 1 < n && s[i + 1] == '/') {
            push(out, max, &ns, n - i, SYN_COMMENT);
            i = n;
            break;
        }
        if (block_comment && c == '/' && i + 1 < n && s[i + 1] == '*') {
            int j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 < n) { push(out, max, &ns, (j + 2) - i, SYN_COMMENT); i = j + 2; }
            else           { push(out, max, &ns, n - i, SYN_COMMENT); i = n;
                             st = SYN_ST_BLOCK_COMMENT; }
            continue;
        }
        if (hash_comment && c == '#') {
            push(out, max, &ns, n - i, SYN_COMMENT);
            i = n;
            break;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            int j = i + 1;
            /* A backslash escapes the next byte, including the quote. Without
             * this "\"" ends the string at its own escaped quote and the rest
             * of the line is coloured as code. */
            while (j < n && s[j] != q) { if (s[j] == '\\' && j + 1 < n) j++; j++; }
            int len = (j < n) ? (j - i + 1) : (n - i);
            push(out, max, &ns, len, SYN_STRING);
            i += len;
            continue;
        }
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
            int j = i;
            while (j < n && (is_word(s[j]) || s[j] == '.')) j++;   /* 0x2A, 3.14f, 1e9 */
            push(out, max, &ns, j - i, SYN_NUMBER);
            i = j;
            continue;
        }
        if (is_word(c) && !is_digit(c)) {
            int j = i;
            while (j < n && is_word(s[j])) j++;
            int len = j - i;
            int role = SYN_TEXT;
            if (word_in(kw, s + i, len))      role = SYN_KEYWORD;
            else if (word_in(ty, s + i, len)) role = SYN_TYPE;
            /* A name ending in _t is a type by convention, and this codebase
             * follows it everywhere. Cheap, and it colours far more of a real
             * file than any table of built-ins would. */
            else if (len > 2 && s[j - 2] == '_' && s[j - 1] == 't') role = SYN_TYPE;
            push(out, max, &ns, len, role);
            i = j;
            continue;
        }
        if (c == ' ' || c == '\t') {
            int j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            push(out, max, &ns, j - i, SYN_TEXT);
            i = j;
            continue;
        }
        push(out, max, &ns, 1, SYN_PUNCT);
        i++;
    }

    if (state) *state = st;
    return ns;
}
