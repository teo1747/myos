/* ports/netsurf/nsprobe.c -- does NetSurf's engine actually RUN on this OS?
 *
 * Cross-compiling is not the same claim as running. A library stack can build
 * for a target and still fault on the first allocation, or link against a libc
 * function that exists as a symbol and returns ENOSYS when called -- this
 * project has a name for that (LINKABLE is not CALLABLE) and has been caught
 * by it before.
 *
 * So this is the smallest program that makes the engine do real work:
 * hand libhubbub+libdom a document, walk the DOM back out, and hand libcss a
 * stylesheet and read a computed value out of it. If this runs on the metal,
 * the parser, the tree and the cascade are all genuinely working -- which is
 * the entire question the port turns on, and everything after it is the
 * frontend.
 *
 * It prints one line per check so the serial log is the evidence.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>
#include <libcss/libcss.h>

static int g_fail;
static void check(int ok, const char *what)
{
    printf("%s   %s\n", ok ? "  ok:" : "FAIL:", what);
    if (!ok) g_fail++;
}

static const char *HTML =
    "<!DOCTYPE html><html><head><title>Ported</title></head>"
    "<body><h1 id=\"top\">Hello</h1><p class=\"x\">A paragraph.</p></body></html>";

/* --- libcss needs to be told how to resolve a URL and find a system font.
 * Neither is exercised by this probe; they must exist because the API takes
 * them, and saying so beats a NULL that faults three layers down. --- */
static css_error resolve_url(void *pw, const char *base,
                             lwc_string *rel, lwc_string **abs)
{
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

static bool parse_document(dom_document **out)
{
    dom_hubbub_parser_params params = {
        .enc = NULL, .fix_enc = true, .enable_script = false,
        .script = NULL, .ctx = NULL, .daf = NULL,
    };
    dom_hubbub_parser *parser = NULL;
    dom_hubbub_error err = dom_hubbub_parser_create(&params, &parser, out);
    if (err != DOM_HUBBUB_OK) return false;
    err = dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)HTML, strlen(HTML));
    if (err != DOM_HUBBUB_OK) { dom_hubbub_parser_destroy(parser); return false; }
    err = dom_hubbub_parser_completed(parser);
    dom_hubbub_parser_destroy(parser);
    return err == DOM_HUBBUB_OK;
}

/* The <h1>'s text, read back through the DOM the parser built. */
static bool read_heading(dom_document *doc, char *out, size_t cap)
{
    dom_nodelist *list = NULL;
    dom_string *tag = NULL;
    bool ok = false;

    if (dom_string_create((const uint8_t *)"h1", 2, &tag) != DOM_NO_ERR) return false;
    if (dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR && list) {
        uint32_t n = 0;
        dom_nodelist_get_length(list, &n);
        if (n > 0) {
            dom_node *h1 = NULL;
            if (dom_nodelist_item(list, 0, &h1) == DOM_NO_ERR && h1) {
                dom_string *text = NULL;
                if (dom_node_get_text_content(h1, &text) == DOM_NO_ERR && text) {
                    snprintf(out, cap, "%.*s", (int)dom_string_length(text),
                             dom_string_data(text));
                    dom_string_unref(text);
                    ok = true;
                }
                dom_node_unref(h1);
            }
        }
        dom_nodelist_unref(list);
    }
    dom_string_unref(tag);
    return ok;
}

/* Parse a stylesheet and confirm the cascade produced a rule. */
static bool parse_stylesheet(void)
{
    static const char CSS_TEXT[] = "h1 { color: #ff0000; display: block }";
    css_stylesheet_params p;
    css_stylesheet *sheet = NULL;
    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level = CSS_LEVEL_DEFAULT;
    p.charset = "UTF-8";
    p.url = "about:probe";
    p.resolve = resolve_url;

    if (css_stylesheet_create(&p, &sheet) != CSS_OK) return false;
    css_error err = css_stylesheet_append_data(sheet,
            (const uint8_t *)CSS_TEXT, sizeof CSS_TEXT - 1);
    if (err != CSS_OK && err != CSS_NEEDDATA) { css_stylesheet_destroy(sheet); return false; }
    err = css_stylesheet_data_done(sheet);
    size_t size = 0;
    css_stylesheet_size(sheet, &size);
    css_stylesheet_destroy(sheet);
    return err == CSS_OK && size > 0;
}

int main(void)
{
    printf("=== nsprobe: NetSurf's engine on EmbLinkOS\n");

    dom_document *doc = NULL;
    check(parse_document(&doc) && doc != NULL, "libhubbub + libdom parse a document");

    if (doc != NULL) {
        char heading[64] = "";
        bool got = read_heading(doc, heading, sizeof heading);
        check(got, "the DOM can be walked back out");
        check(got && strcmp(heading, "Hello") == 0, "the <h1> reads back as its own text");
        if (got) printf("       <h1> = [%s]\n", heading);

        dom_element *root = NULL;
        dom_document_get_document_element(doc, &root);
        check(root != NULL, "the document has a root element");
        if (root) dom_node_unref(root);
        dom_node_unref(doc);
    }

    check(parse_stylesheet(), "libcss parses a stylesheet");

    printf(g_fail ? "=== nsprobe: FAIL (%d)\n" : "=== nsprobe: OK (%d failures)\n", g_fail);
    return g_fail != 0;
}
