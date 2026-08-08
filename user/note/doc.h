/* user/note/doc.h -- the files Note++ has open.
 *
 * One document per tab, and a tab is exactly what has to survive while you are
 * looking at a different one: the text, where the cursor was, whether it has
 * unsaved changes, and which language it is. Unlike the browser's tabs (which
 * keep source bytes and re-parse on switch, because one document arena is
 * shared -- see user/web/tabs.h) an editor's documents are ALL live at once:
 * you can be halfway through editing four files, and re-reading one from disk
 * on switch would throw away the edits that are the whole point.
 *
 * So the buffers are per-document and reserved up front. That is the memory
 * this app costs: DOC_MAX * DOC_CAP bytes, chosen rather than grown, for the
 * same reason every other arena here is.
 *
 * The DIRTY flag is a comparison against what was last read or written, not a
 * flag set on every keystroke -- so typing a character and deleting it again
 * leaves a document clean, which is what a person expects and what stops a
 * "save changes?" prompt appearing for no reason.
 *
 * No UI and no syscalls in this file beyond the read/write of a whole file, so
 * the model is testable on a host.
 */
#ifndef _EMBLINK_NOTE_DOC_H_
#define _EMBLINK_NOTE_DOC_H_

#include <stddef.h>
#include "syntax.h"

#define DOC_MAX      8               /* documents open at once     */
#define DOC_CAP      (256 * 1024)    /* bytes per document         */
#define DOC_PATH_MAX 256

struct doc {
    char          path[DOC_PATH_MAX];  /* "" = never saved anywhere   */
    char         *text;                /* NUL-terminated, DOC_CAP cap */
    int           cursor;              /* byte offset                 */
    int           open;
    enum syn_lang lang;
    unsigned long saved_hash;          /* of the text as last read/written */
    int           truncated;           /* the file was larger than DOC_CAP */
};

void        doc_init(void);
int         doc_count(void);
int         doc_current(void);
struct doc *doc_at(int i);            /* NULL if that slot is not open */
struct doc *doc_cur(void);

/* Open `path`, or return an already-open document for it -- opening the same
 * file twice in two tabs is two buffers diverging over one file, and whichever
 * you saved last would silently win. Returns the index, or -1 if there is no
 * room. `read` does the actual I/O and returns bytes read, or -1. */
int  doc_open(const char *path, long (*read)(const char *path, char *buf, size_t cap));
int  doc_new(void);                   /* an empty, unnamed document */
int  doc_close(int i);                /* returns the index current afterwards */
int  doc_select(int i);               /* 0 if it changed */

/* Has this document changed since it was last read or written? */
int  doc_dirty(int i);
/* Record the text as saved (call after a successful write). */
void doc_mark_saved(int i);

/* What the tab shows: the file name, or "Untitled". Never empty -- a nameless
 * tab is unclickable. A trailing dot marks unsaved changes, which is the one
 * piece of state a person must be able to see without opening a menu. */
const char *doc_label(int i);

/* Line and column (both 1-based) of the cursor, for the status bar. */
void doc_line_col(const struct doc *d, int *line, int *col);

/* The hash the dirty check uses. Exposed for the test. */
unsigned long doc_hash(const char *s);

#endif /* _EMBLINK_NOTE_DOC_H_ */
