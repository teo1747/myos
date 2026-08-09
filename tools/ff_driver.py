#!/usr/bin/env python3
"""ff_driver.py -- Firefox as an oracle, over WebDriver.

The reference browser, driven by the four calls that matter: load a page, ask
it a question in JavaScript, take a picture. No selenium -- the protocol is
plain HTTP and JSON, and a dependency that has to be installed first is a tool
nobody runs.

It is one file because two different verifiers need the same oracle and must
agree exactly about what "the reference" means: web_verify.py asks what text a
reader can SEE, web_shots.py asks WHERE it is. Asking the same Firefox session
the same way is the whole point.
"""
import json
import os
import subprocess
import time
import urllib.request

PORT = 4455


class Firefox:
    def __init__(self, width=1100, height=900, port=PORT):
        self.port = port
        self.w, self.h = width, height
        self.p = subprocess.Popen(["geckodriver", "--port", str(port)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sid = None
        for _ in range(60):
            try:
                self._rq("GET", "/status")
                break
            except Exception:
                time.sleep(0.5)
        s = self._rq("POST", "/session", {"capabilities": {"alwaysMatch": {
            "moz:firefoxOptions": {"args": ["-headless", "--width=%d" % width,
                                            "--height=%d" % height]}}}})
        self.sid = s["value"]["sessionId"]

    def _rq(self, method, path, body=None):
        r = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path), method=method,
                                   data=json.dumps(body).encode() if body is not None else None,
                                   headers={"Content-Type": "application/json"})
        return json.loads(urllib.request.urlopen(r, timeout=180).read())

    def open(self, path):
        self._rq("POST", "/session/%s/url" % self.sid,
                 {"url": "file://" + os.path.abspath(path)})

    def js(self, script):
        return self._rq("POST", "/session/%s/execute/sync" % self.sid,
                        {"script": script, "args": []}).get("value")

    def visible_text(self, path):
        self.open(path)
        return self.js("return document.body ? document.body.innerText : ''") or ""

    # THE PICTURE. WebDriver's own screenshot is the VIEWPORT, which is the
    # honest comparison: our render is a viewport too, and a full-page capture
    # would compare a 12000px scroll against a 900px window.
    def screenshot(self, path):
        import base64
        b = self._rq("GET", "/session/%s/screenshot" % self.sid)["value"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(b))

    # WHERE THE WORDS ARE. Every text node's box, in document order, with the
    # invisible ones dropped by the same rule innerText uses -- so the two
    # verifiers are looking at the same page.
    RECTS_JS = r"""
      var out = [], w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
      var n;
      while ((n = w.nextNode())) {
        var t = n.textContent.trim();
        if (!t) continue;
        var p = n.parentElement;
        if (!p) continue;
        var cs = getComputedStyle(p);
        if (cs.visibility === 'hidden' || cs.display === 'none' || cs.opacity === '0') continue;
        var r = document.createRange(); r.selectNodeContents(n);
        var b = r.getBoundingClientRect();
        if (b.width < 1 || b.height < 1) continue;
        out.push({t: t.slice(0, 60), x: Math.round(b.left + window.scrollX),
                  y: Math.round(b.top + window.scrollY),
                  w: Math.round(b.width), h: Math.round(b.height)});
      }
      return out;
    """

    def text_rects(self, path):
        self.open(path)
        return self.js("return (function(){%s})()" % self.RECTS_JS) or []

    def close(self):
        try:
            if self.sid:
                self._rq("DELETE", "/session/%s" % self.sid)
        except Exception:
            pass
        self.p.terminate()
