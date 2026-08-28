#!/usr/bin/env python3
"""Shrink the captive-portal page on its way into flash.

main/portal.src.html is the readable source; the build runs this over it and
embeds the result, so the page can stay commented and indented without paying
for it in rodata. See main/CMakeLists.txt.

Deliberately conservative. This page ships in firmware that is a pain to
reflash, so anything with a plausible failure mode is left alone:

  - <svg> subtrees are copied byte for byte. Path data is whitespace-sensitive
    in ways that are easy to get subtly wrong, and the wordmark is one long
    <path d> that a naive collapse could corrupt into a slightly wrong glyph --
    a bug you would only catch by looking at it.
  - <pre> and <textarea> likewise, since their whitespace renders.
  - JS keeps its newlines. Comments and indentation go, but joining lines means
    reasoning about automatic semicolon insertion, and one wrong join is a page
    that fails to parse on the one device that serves it.

CSS gets the aggressive treatment, having no such hazards.
"""
import re
import sys

VERBATIM = ("svg", "pre", "textarea")


def minify_css(css):
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    css = re.sub(r"\s+", " ", css)
    css = re.sub(r"\s*([{};:,>])\s*", r"\1", css)
    css = re.sub(r";}", "}", css)
    return css.strip()


def strip_js_comments(js):
    """Drop // and /* */ comments without touching look-alikes in strings.

    Walks the source one character at a time because a regex cannot tell a
    comment from the // inside a URL string literal.
    """
    out = []
    i, n = 0, len(js)
    quote = None
    while i < n:
        c = js[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(js[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in "\"'`":
            quote = c
            out.append(c)
            i += 1
            continue
        if js.startswith("//", i):
            i = js.find("\n", i)
            if i < 0:
                break
            continue
        if js.startswith("/*", i):
            end = js.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def minify_js(js):
    lines = [ln.strip() for ln in strip_js_comments(js).split("\n")]
    return "\n".join(ln for ln in lines if ln)


def minify_tag(tag):
    """Collapse whitespace inside a start tag, leaving quoted values alone."""
    parts = re.split(r"""("[^"]*"|'[^']*')""", tag)
    for i in range(0, len(parts), 2):
        parts[i] = re.sub(r"\s+", " ", parts[i])
        parts[i] = re.sub(r"\s*=\s*", "=", parts[i])
    return "".join(parts).replace(" >", ">")


def minify(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith("<!--", i):
            end = src.find("-->", i)
            i = n if end < 0 else end + 3
            continue

        m = re.match(r"<(%s)\b" % "|".join(VERBATIM), src[i:], re.I)
        if m:
            name = m.group(1)
            # Nested same-name elements are legal in SVG; count depth.
            depth, j = 0, i
            pat = re.compile(r"<(/?)%s\b" % name, re.I)
            while j < n:
                t = pat.search(src, j)
                if not t:
                    j = n
                    break
                depth += -1 if t.group(1) else 1
                j = t.end()
                if depth == 0:
                    j = src.find(">", j) + 1
                    break
            out.append(src[i:j])
            i = j
            continue

        m = re.match(r"<(style|script)\b([^>]*)>", src[i:], re.I)
        if m:
            name = m.group(1).lower()
            body_start = i + m.end()
            close = re.compile(r"</%s\s*>" % name, re.I).search(src, body_start)
            body_end = close.start() if close else n
            body = src[body_start:body_end]
            body = minify_css(body) if name == "style" else minify_js(body)
            out.append(minify_tag(m.group(0)) + body + "</%s>" % name)
            i = close.end() if close else n
            continue

        if src[i] == "<":
            end = src.find(">", i)
            if end < 0:
                out.append(src[i:])
                break
            out.append(minify_tag(src[i:end + 1]))
            i = end + 1
            continue

        end = src.find("<", i)
        end = n if end < 0 else end
        text = src[i:end]
        # A whitespace-only run that spans lines was indentation; one that does
        # not is a real space between inline elements, as in "> Show password".
        if text.strip():
            out.append(re.sub(r"\s+", " ", text))
        elif "\n" not in text:
            out.append(text)
        i = end
    return "".join(out).strip() + "\n"


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        text = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(minify(text))
