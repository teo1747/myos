#!/usr/bin/env python3
"""Generate user/lib/tls/x509/roots.h -- the browser's trust anchors.

A trust anchor is two things: the DER of the root's SUBJECT name, which is what
a chain's topmost issuer field is compared against, and its PUBLIC KEY, which
is what the last signature is checked with. Both were hand-transcribed as hex
arrays, one root at a time, which is why there were four of them -- and four
roots means the browser can reach the fraction of the web that happens to chain
to those four. It could not open example.com.

This reads real certificates and writes that file, so adding a root is adding a
line to ROOTS below. The certificates come from the host's CA bundle: they are
public, they are the same bytes every browser ships, and taking them from a
file the machine already trusts beats pasting hex out of a web page.

    python3 tools/mkroots.py            # regenerate from the host bundle
    python3 tools/mkroots.py --check    # ...and fail if it would change

The ASN.1 walk here is deliberately minimal -- enough to find two fields of a
certificate and nothing more. A real parser lives in user/lib/tls/x509; this is
a build tool, and the certificates it reads are not hostile input.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "user", "lib", "tls", "x509", "roots.h")

# Where the host keeps its certificates. Several layouts, because several
# distributions.
BUNDLES = [
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
]

# The roots to bundle, by the CN in their subject, with the C identifier to
# emit. Chosen for COVERAGE of the web a person actually visits rather than for
# completeness: between them these issue for the large majority of public
# sites. Adding one is adding a line.
ROOTS = [
    ("GTS_R4",         "GTS Root R4"),                       # Google / Cloudflare
    ("GTS_R1",         "GTS Root R1"),
    ("ISRG_X1",        "ISRG Root X1"),                      # Let's Encrypt
    ("ISRG_X2",        "ISRG Root X2"),
    ("GS_R3",          "GlobalSign"),                        # GlobalSign Root R3
    ("USERTRUST_ECC",  "USERTrust ECC Certification Authority"),
    ("USERTRUST_RSA",  "USERTrust RSA Certification Authority"),
    ("DIGICERT_G2",    "DigiCert Global Root G2"),           # example.com, much of the web
    ("DIGICERT_G3",    "DigiCert Global Root G3"),
    ("DIGICERT_CA",    "DigiCert Global Root CA"),
    ("DIGICERT_HA",    "DigiCert High Assurance EV Root CA"),
    ("BALTIMORE",      "Baltimore CyberTrust Root"),         # Microsoft, Azure
    ("AMAZON_1",       "Amazon Root CA 1"),
    ("AMAZON_2",       "Amazon Root CA 2"),
    ("AMAZON_3",       "Amazon Root CA 3"),
    ("AMAZON_4",       "Amazon Root CA 4"),
    ("SECTIGO_E46",    "Sectigo Public Server Authentication Root E46"),
    ("SECTIGO_R46",    "Sectigo Public Server Authentication Root R46"),
    ("CERTUM_EC",      "Certum EC-384 CA"),
    ("GLOBALSIGN_R6",  "GlobalSign Root CA - R6"),
    # example.com chains here, which is worth recording because it was assumed
    # to be DigiCert and is not: asking the server settled it in one command.
    ("SSLCOM_ECC_2022","SSL.com TLS ECC Root CA 2022"),
    ("SSLCOM_RSA_2022","SSL.com TLS RSA Root CA 2022"),
    ("SSLCOM_EV_ECC",  "SSL.com EV Root Certification Authority ECC"),
    ("GTS_R2",         "GTS Root R2"),
    ("GTS_R3",         "GTS Root R3"),
    ("CERTUM_TRUSTED", "Certum Trusted Network CA"),
    ("ENTRUST_G2",     "Entrust Root Certification Authority - G2"),
    ("GODADDY_G2",     "Go Daddy Root Certificate Authority - G2"),
]


# --- a very small DER reader ------------------------------------------------
def tlv(buf, i):
    """(tag, header_len, length, value_start) of the element at `i`."""
    tag = buf[i]
    n = buf[i + 1]
    if n < 0x80:
        return tag, 2, n, i + 2
    k = n & 0x7F
    ln = int.from_bytes(buf[i + 2:i + 2 + k], "big")
    return tag, 2 + k, ln, i + 2 + k


def children(buf, i):
    """Yield (tag, start_of_element, end_of_element) for a constructed value."""
    _, hl, ln, vs = tlv(buf, i)
    end = vs + ln
    p = vs
    while p < end:
        _, h2, l2, v2 = tlv(buf, p)
        yield buf[p], p, v2 + l2
        p = v2 + l2


def cert_fields(der):
    """(subject_der, spki_der) of an X.509 certificate."""
    _, _, _, _ = tlv(der, 0)
    tbs = next(iter(children(der, 0)))          # first child = tbsCertificate
    fields = list(children(der, tbs[1]))
    # tbs: [0] version (context 0, optional), serial, sigAlg, issuer,
    #      validity, subject, spki, ...
    off = 1 if fields[0][0] == 0xA0 else 0
    subject = fields[off + 4]
    spki    = fields[off + 5]
    return der[subject[1]:subject[2]], der[spki[1]:spki[2]]


def spki_key(spki):
    """(kind, ...) -- ('ec', point) or ('rsa', n, e)."""
    algid, bits = list(children(spki, 0))
    oid = None
    for tag, s, e in children(spki, algid[1]):
        if tag == 0x06:
            _, hl, ln, vs = tlv(spki, s)
            oid = spki[vs:vs + ln]
            break
    _, hl, ln, vs = tlv(spki, bits[1])
    raw = spki[vs + 1: vs + ln]                 # skip the unused-bits byte
    if oid == bytes([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01]):     # ecPublicKey
        return ("ec", raw)
    # RSA: the bit string wraps SEQUENCE { n INTEGER, e INTEGER }
    ns, es = list(children(raw, 0))
    def integer(b, ent):
        _, hl2, ln2, vs2 = tlv(b, ent[1])
        v = b[vs2:vs2 + ln2]
        return v.lstrip(b"\x00") or b"\x00"
    return ("rsa", integer(raw, ns), integer(raw, es))


def load_bundle():
    for p in BUNDLES:
        if os.path.exists(p):
            return open(p, "rb").read().decode("ascii", "replace"), p
    sys.exit("no CA bundle found; looked in:\n  " + "\n  ".join(BUNDLES))


def parse_pems(text):
    out = {}
    import base64
    for m in re.finditer(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", text, re.S):
        der = base64.b64decode(re.sub(r"\s", "", m.group(1)))
        try:
            subj, spki = cert_fields(der)
        except Exception:
            continue
        cn = None
        # the CN is the last commonName attribute in the subject
        for m2 in re.finditer(b"\x06\x03\x55\x04\x03", subj):
            i = m2.end()
            _, hl, ln, vs = tlv(subj, i)
            cn = subj[vs:vs + ln].decode("utf-8", "replace")
        if cn:
            out.setdefault(cn, (subj, spki))
    return out


def carr(name, b, indent="  "):
    lines, row = [], []
    for i, byte in enumerate(b):
        row.append("0x%02x" % byte)
        if len(row) == 16:
            lines.append(indent + ",".join(row)); row = []
    if row:
        lines.append(indent + ",".join(row))
    return "static const uint8_t %s[] = {\n%s\n};\n" % (name, ",\n".join(lines))


def main():
    text, path = load_bundle()
    certs = parse_pems(text)
    body, anchors, missing = [], [], []
    for ident, cn in ROOTS:
        if cn not in certs:
            missing.append(cn); continue
        subj, spki = certs[cn]
        kind = spki_key(spki)
        body.append("/* %s */" % cn)
        body.append(carr(ident + "_SUBJECT", subj))
        if kind[0] == "ec":
            point = kind[1]
            half = (len(point) - 1) // 2
            curve = {32: "X509_CURVE_P256", 48: "X509_CURVE_P384",
                     66: "X509_CURVE_P521"}.get(half)
            if not curve:
                missing.append(cn + " (unknown curve)"); body.pop(); body.pop(); continue
            body.append(carr(ident + "_POINT", point))
            anchors.append("    {   /* %s -- EC */\n"
                           "        %s_SUBJECT, sizeof %s_SUBJECT,\n"
                           "        X509_KEY_EC, %s,\n"
                           "        %s_POINT + 1, %s_POINT + 1 + %d, %d,\n"
                           "        NULL, 0, NULL, 0,\n    },"
                           % (cn, ident, ident, curve, ident, ident, half, half))
        else:
            body.append(carr(ident + "_N", kind[1]))
            body.append(carr(ident + "_E", kind[2]))
            anchors.append("    {   /* %s -- RSA-%d */\n"
                           "        %s_SUBJECT, sizeof %s_SUBJECT,\n"
                           "        X509_KEY_RSA, X509_CURVE_NONE,\n"
                           "        NULL, NULL, 0,\n"
                           "        %s_N, sizeof %s_N, %s_E, sizeof %s_E,\n    },"
                           % (cn, len(kind[1]) * 8, ident, ident, ident, ident, ident, ident))

    out = ("/* GENERATED by tools/mkroots.py from %s -- do not edit.\n"
           " *\n"
           " * The browser's trust anchors: each root's SUBJECT name, which a chain's\n"
           " * topmost issuer field is matched against, and its PUBLIC KEY, which the\n"
           " * last signature is verified with. Adding a root is adding a line to\n"
           " * ROOTS in the generator; it used to be transcribing hex by hand, which\n"
           " * is why there were four and the browser could not open example.com.\n"
           " */\n"
           "#ifndef EMBK_TLS_ROOTS_H\n#define EMBK_TLS_ROOTS_H\n#include <stdint.h>\n\n"
           % path) + "\n".join(body) + "\n" + \
          ("/* The table trust.c searches, in the same order as the generator's list. */\n"
           "#define EMBK_TLS_ROOTS_TABLE \\\n") + \
          " \\\n".join(a.replace("\n", "\\\n") for a in anchors) + "\n\n#endif\n"

    if "--check" in sys.argv:
        cur = open(OUT).read() if os.path.exists(OUT) else ""
        if cur != out:
            sys.exit("roots.h is out of date; run: python3 tools/mkroots.py")
        print("roots.h up to date (%d anchors)" % len(anchors))
        return
    open(OUT, "w").write(out)
    print("wrote %s: %d anchors from %s" % (os.path.relpath(OUT, ROOT), len(anchors), path))
    if missing:
        print("  NOT FOUND in the host bundle (skipped): " + ", ".join(missing))


if __name__ == "__main__":
    main()
