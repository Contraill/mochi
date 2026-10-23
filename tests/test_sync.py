"""Keeps the English and Turkish builds in sync.

The two sketches and the two web panels are separate files on purpose (one folder per
language). These tests fail when they drift apart in anything other than translated text,
e.g. a bug fix applied to only one language.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import build_web  # noqa: E402

C_STRING = re.compile(r'"(?:\\.|[^"\\\n])*"')
JS_STRING = re.compile(r"'(?:\\.|[^'\\\n])*'|\"(?:\\.|[^\"\\\n])*\"|`(?:\\.|[^`\\])*`")


def test_web_headers_match_sources():
    assert build_web.main(["--check"]) == 0, "run: python tools/build_web.py"


def test_sketches_differ_only_in_strings():
    en = (ROOT / "Mochi_EN" / "Mochi_EN.ino").read_text(encoding="utf-8").splitlines()
    tr = (ROOT / "Mochi_TR" / "Mochi_TR.ino").read_text(encoding="utf-8").splitlines()
    assert len(en) == len(tr), "Mochi_EN.ino and Mochi_TR.ino have a different number of lines"
    drift = [i + 1 for i, (a, b) in enumerate(zip(en, tr))
             if a != b and C_STRING.sub('""', a) != C_STRING.sub('""', b)]
    assert drift == [], f"code differs (not just strings) on lines {drift}"


def _skeleton(html):
    """HTML with text, translatable attributes and JS string contents removed."""
    scripts = [JS_STRING.sub("''", js) for js in re.findall(r"<script>(.*?)</script>", html, re.S)]
    markup = re.sub(r"<script>.*?</script>", "<script></script>", html, flags=re.S)
    markup = re.sub(r'\s(placeholder|title)="[^"]*"', "", markup)
    markup = re.sub(r"<html lang=\"\w+\">", "<html>", markup)
    markup = re.sub(r">[^<]*<", "><", markup)
    return markup, scripts


def test_web_panels_have_same_structure():
    en_markup, en_js = _skeleton((ROOT / "web" / "index_en.html").read_text(encoding="utf-8"))
    tr_markup, tr_js = _skeleton((ROOT / "web" / "index_tr.html").read_text(encoding="utf-8"))
    assert re.findall(r'id="([^"]+)"', en_markup) == re.findall(r'id="([^"]+)"', tr_markup)
    assert en_markup == tr_markup
    assert len(en_js) == len(tr_js)
    for i, (a, b) in enumerate(zip(en_js, tr_js)):
        a_lines, b_lines = a.splitlines(), b.splitlines()
        drift = [n + 1 for n, (x, y) in enumerate(zip(a_lines, b_lines)) if x != y]
        assert len(a_lines) == len(b_lines) and drift == [], f"script {i} differs on lines {drift[:10]}"
