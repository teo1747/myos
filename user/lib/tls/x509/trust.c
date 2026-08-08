/* Bundled trust anchors -- see roots.h, which is GENERATED.
 *
 * Adding a root is adding a line to tools/mkroots.py and re-running it. It used
 * to mean transcribing a subject name and a public key as hex arrays by hand,
 * which is why there were four of them and the browser could not open
 * example.com: a four-root browser reaches the fraction of the web that
 * happens to chain to those four.
 *
 * Still a fixed set compiled in. The packaging story (docs) will later let
 * /system carry a curated, verified-boot-sealed set the user can grow, which
 * is the difference between a browser that trusts what it was built with and
 * one whose owner decides. */
#include "trust.h"
#include "cert.h"
#include "roots.h"     /* GENERATED -- see tools/mkroots.py */
#include <string.h>

static const struct trust_anchor ANCHORS[] = {
    EMBK_TLS_ROOTS_TABLE
};
#define N_ANCHORS (int)(sizeof ANCHORS / sizeof ANCHORS[0])

const struct trust_anchor *trust_find(const uint8_t *issuer, size_t issuer_len) {
    for (int i = 0; i < N_ANCHORS; i++)
        if (ANCHORS[i].subject_len == issuer_len && memcmp(ANCHORS[i].subject, issuer, issuer_len) == 0)
            return &ANCHORS[i];
    return NULL;
}
