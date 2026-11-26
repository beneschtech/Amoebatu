#!/usr/bin/env python3
"""Build doc/reference.html from doc/REFERENCE.md.

    python3 doc/build_reference_html.py            # write doc/reference.html
    python3 doc/build_reference_html.py --check    # exit 1 if reference.html is out of date

REFERENCE.md is the source of truth (CLAUDE.md); the HTML page is generated,
committed, and published as is: one self-contained file, no external fonts,
scripts or styles, so it works on any web server or straight from disk.
The output is deterministic (no timestamps), which is what makes --check work;
tests/run_tests.sh runs it.

Only the Markdown that REFERENCE.md uses is supported: headings, paragraphs,
fenced code (also inside list items), bullet and numbered lists with indented
continuation, GitHub-style tables (with \\| escapes), horizontal rules,
`code`, **bold**, *italic*, [links](url). "§9.7" becomes a link to that
section. Standard library only.
"""
import html
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "REFERENCE.md"
OUT = HERE / "reference.html"

# ----------------------------------------------------------------- Dolphin syntax colouring

KEYWORDS = set("""def return if elif else while for in break continue pass try except as raise
and or not lambda import from struct layout extern needs mut""".split())
CONSTANTS = {"True", "False", "None"}
BUILTINS = set("""print read_file Stream JSON Listener Path Archive compress decompress compressors
array len range str repr int float bool abs min max sum ord chr type sorted bits be_u8 be_u16
be_u32 be_u64 be_i16sm be_i32sm be_f32 be_f64""".split())
EXCEPTIONS = set("""Error ValueError TypeError IndexError KeyError NameError UnboundLocalError
AttributeError ZeroDivisionError OverflowError IOError DecodeError RecursionError RuntimeError
SyntaxError JSONError""".split())

TOKEN = re.compile(r"""
    (?P<comment>\#[^\n]*)
  | (?P<string>[bBrRfF]{0,2}(?:'''[\s\S]*?'''|\"\"\"[\s\S]*?\"\"\"|'(?:\\.|[^'\\\n])*'|"(?:\\.|[^"\\\n])*"))
  | (?P<number>\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|\d[\d_]*\.?\d*(?:[eE][+-]?\d+)?)\b)
  | (?P<name>[A-Za-z_]\w*)
""", re.VERBOSE)


def highlight(code):
    out, pos = [], 0
    for m in TOKEN.finditer(code):
        out.append(html.escape(code[pos:m.start()]))
        text = m.group(0)
        kind = m.lastgroup
        if kind == "name":
            kind = ("kw" if text in KEYWORDS else "const" if text in CONSTANTS else
                    "bi" if text in BUILTINS else "exc" if text in EXCEPTIONS else None)
        else:
            kind = {"comment": "com", "string": "str", "number": "num"}[kind]
        out.append(f'<span class="{kind}">{html.escape(text)}</span>' if kind else html.escape(text))
        pos = m.end()
    out.append(html.escape(code[pos:]))
    return "".join(out)


# ----------------------------------------------------------------- inline Markdown

def slug(text):
    s = re.sub(r"<[^>]+>", "", text).lower()
    s = re.sub(r"[`*]", "", s)
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    return s or "section"


class Inline:
    def __init__(self, sections):
        self.sections = sections   # "9.7" -> anchor id

    def __call__(self, text):
        codes = []

        def keep(m):
            codes.append("<code>" + html.escape(m.group(1).replace("\\|", "|")) + "</code>")
            return f"\x00{len(codes) - 1}\x00"

        text = re.sub(r"`([^`]+)`", keep, text)
        text = text.replace("\\|", "|")
        text = html.escape(text, quote=False)
        text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", lambda m: f'<a href="{html.escape(m.group(2))}">{m.group(1)}</a>', text)
        text = re.sub(r"\*\*(?=\S)(.+?)(?<=\S)\*\*", r"<strong>\1</strong>", text)
        text = re.sub(r"(?<![\w*])\*(?=\S)([^*\n]+?)(?<=\S)\*(?![\w*])", r"<em>\1</em>", text)

        def section(m):
            ref = m.group(1)
            anchor = self.sections.get(ref)
            return f'<a href="#{anchor}">§{ref}</a>' if anchor else m.group(0)

        text = re.sub(r"§(\d+(?:\.\d+)?)", section, text)
        return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], text)


# ----------------------------------------------------------------- block Markdown

LIST_ITEM = re.compile(r"^(?:([-*])|(\d+)\.) +")
FENCE = re.compile(r"^```(\w*)\s*$")


def is_table(lines, i):
    return (lines[i].lstrip().startswith("|") and i + 1 < len(lines)
            and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]) is not None)


def split_row(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    return [c.strip() for c in re.split(r"(?<!\\)\|", line)]


class Blocks:
    def __init__(self, inline):
        self.inline = inline
        self.headings = []   # (level, id, html)
        self.ids = set()

    def heading_id(self, text):
        base = slug(text)
        m = re.match(r"(\d+(?:\.\d+)?)\.?\s", text)
        if m:
            base = "s" + m.group(1).replace(".", "-")
        hid, n = base, 2
        while hid in self.ids:
            hid, n = f"{base}-{n}", n + 1
        self.ids.add(hid)
        return hid

    def parse(self, lines, tight=False):
        out, i = [], 0
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                i += 1
                continue
            m = FENCE.match(line)
            if m:
                lang, body, i = m.group(1), [], i + 1
                while i < len(lines) and not FENCE.match(lines[i]):
                    body.append(lines[i])
                    i += 1
                i += 1
                code = "\n".join(body)
                inner = highlight(code) if lang in ("python", "dolphin") else html.escape(code)
                cls = f' class="lang-{lang}"' if lang else ""
                out.append(f"<pre{cls}><code>{inner}</code></pre>")
                continue
            m = re.match(r"^(#{1,6})\s+(.*)$", line)
            if m:
                level, text = len(m.group(1)), m.group(2).strip()
                hid = self.heading_id(text)
                body = self.inline(text)
                if level > 1:
                    self.headings.append((level, hid, body))
                anchor = f' <a class="anchor" href="#{hid}" aria-label="Link to this section">#</a>' if level > 1 else ""
                out.append(f'<h{level} id="{hid}">{body}{anchor}</h{level}>')
                i += 1
                continue
            if re.match(r"^-{3,}\s*$", line):
                out.append("<hr>")
                i += 1
                continue
            if is_table(lines, i):
                head = split_row(lines[i])
                i += 2
                rows = []
                while i < len(lines) and lines[i].lstrip().startswith("|"):
                    rows.append(split_row(lines[i]))
                    i += 1
                t = ["<div class=\"table\"><table><thead><tr>"]
                t += [f"<th>{self.inline(c)}</th>" for c in head]
                t.append("</tr></thead><tbody>")
                for r in rows:
                    r = (r + [""] * len(head))[:len(head)]
                    t.append("<tr>" + "".join(f"<td>{self.inline(c)}</td>" for c in r) + "</tr>")
                t.append("</tbody></table></div>")
                out.append("".join(t))
                continue
            m = LIST_ITEM.match(line)
            if m:
                ordered = m.group(2) is not None
                items = []
                while i < len(lines):
                    m = LIST_ITEM.match(lines[i])
                    if not m or (m.group(2) is not None) != ordered:
                        break
                    width = m.end()
                    item = [lines[i][width:]]
                    i += 1
                    while i < len(lines):
                        nxt = lines[i]
                        if not nxt.strip():
                            # A blank line continues the item only if indented content follows.
                            j = i
                            while j < len(lines) and not lines[j].strip():
                                j += 1
                            if j < len(lines) and lines[j].startswith(" " * width):
                                item.extend([""] * (j - i))
                                i = j
                                continue
                            break
                        if nxt.startswith(" " * width):
                            item.append(nxt[width:])
                            i += 1
                        elif not LIST_ITEM.match(nxt) and not nxt.startswith(("#", "|", "```")) and item[-1].strip():
                            item.append(nxt.strip())   # lazy continuation
                            i += 1
                        else:
                            break
                    items.append(item)
                    if i < len(lines) and not lines[i].strip():
                        j = i
                        while j < len(lines) and not lines[j].strip():
                            j += 1
                        if j < len(lines) and LIST_ITEM.match(lines[j]) and (LIST_ITEM.match(lines[j]).group(2) is not None) == ordered:
                            i = j
                tag = "ol" if ordered else "ul"
                start = ""
                if ordered:
                    first = LIST_ITEM.match(line).group(2)
                    start = f' start="{first}"' if first != "1" else ""
                parts = [f"<{tag}{start}>"]
                for item in items:
                    parts.append("<li>" + self.parse(item, tight=True) + "</li>")
                parts.append(f"</{tag}>")
                out.append("".join(parts))
                continue
            para = [line.strip()]
            i += 1
            while (i < len(lines) and lines[i].strip() and not FENCE.match(lines[i])
                   and not re.match(r"^(#{1,6})\s", lines[i]) and not is_table(lines, i)
                   and not LIST_ITEM.match(lines[i]) and not re.match(r"^-{3,}\s*$", lines[i])):
                para.append(lines[i].strip())
                i += 1
            text = self.inline(" ".join(para))
            out.append(text if tight and not out and i >= len(lines) else f"<p>{text}</p>")
        return "\n".join(out)


# ----------------------------------------------------------------- page

CSS = """
:root{--bg:#fbfaf7;--fg:#1d1f21;--muted:#5f6670;--line:#e3e0d8;--panel:#f2efe8;--accent:#0b6e80;
--code-bg:#f4f1ea;--kw:#8e3a9a;--str:#2f7d32;--num:#b4530b;--com:#7c8288;--bi:#0b6e80;--exc:#b3261e;--const:#b4530b;
--mono:ui-monospace,SFMono-Regular,"JetBrains Mono",Menlo,Consolas,monospace;
--sans:system-ui,-apple-system,"Segoe UI",Roboto,Ubuntu,"Helvetica Neue",Arial,sans-serif}
@media (prefers-color-scheme:dark){:root{--bg:#15171a;--fg:#e4e2dd;--muted:#9aa1a9;--line:#2c3036;--panel:#1c1f23;
--accent:#5cc3d4;--code-bg:#1e2126;--kw:#d49ade;--str:#8fcf8f;--num:#f0a35e;--com:#7f8790;--bi:#5cc3d4;--exc:#ff8a80;--const:#f0a35e}}
*{box-sizing:border-box}
html{scroll-padding-top:1rem}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.6 var(--sans);-webkit-text-size-adjust:100%}
a{color:var(--accent);text-decoration-thickness:1px;text-underline-offset:2px}
.layout{display:grid;grid-template-columns:17rem minmax(0,1fr);max-width:84rem;margin:0 auto}
nav{position:sticky;top:0;height:100vh;overflow-y:auto;padding:1.5rem 1rem 3rem 1.25rem;border-right:1px solid var(--line);font-size:.9rem}
nav .brand{font-weight:700;font-size:1.05rem;margin:0 0 .75rem;color:var(--fg);text-decoration:none;display:block}
nav input{width:100%;padding:.4rem .6rem;margin-bottom:.75rem;border:1px solid var(--line);border-radius:6px;background:var(--panel);color:var(--fg);font:inherit}
nav ul{list-style:none;margin:0;padding:0}
nav li{margin:.1rem 0}
nav ul ul{padding-left:.9rem;margin:.15rem 0 .4rem;border-left:1px solid var(--line)}
nav a{color:var(--muted);text-decoration:none;display:block;padding:.1rem .3rem;border-radius:4px}
nav a:hover,nav a.active{color:var(--fg);background:var(--panel)}
nav > ul > li > a{color:var(--fg);font-weight:600}
main{padding:2rem 2.5rem 6rem;max-width:56rem;min-width:0}
h1{font-size:2.1rem;line-height:1.2;margin:0 0 .5rem}
h2{font-size:1.5rem;margin:3rem 0 .75rem;padding-top:.5rem;border-top:1px solid var(--line)}
h3{font-size:1.15rem;margin:2rem 0 .5rem}
h2 .anchor,h3 .anchor{opacity:0;margin-left:.35rem;text-decoration:none;color:var(--muted);font-weight:400}
h2:hover .anchor,h3:hover .anchor{opacity:1}
.subtitle{color:var(--muted);margin:0 0 1.5rem}
hr{display:none}
p,li{max-width:48rem}
code{font-family:var(--mono);font-size:.88em;background:var(--code-bg);padding:.1em .3em;border-radius:4px}
pre{position:relative;background:var(--code-bg);border:1px solid var(--line);border-radius:8px;padding:.85rem 1rem;overflow-x:auto;line-height:1.45;margin:1rem 0}
pre code{background:none;padding:0;font-size:.86rem}
pre .copy{position:absolute;top:.4rem;right:.4rem;font:12px var(--sans);padding:.15rem .5rem;border:1px solid var(--line);border-radius:5px;background:var(--panel);color:var(--muted);cursor:pointer;opacity:0}
pre:hover .copy,pre .copy:focus{opacity:1}
.kw{color:var(--kw);font-weight:600}.str{color:var(--str)}.num{color:var(--num)}.com{color:var(--com);font-style:italic}
.bi{color:var(--bi)}.exc{color:var(--exc)}.const{color:var(--const)}
.table{overflow-x:auto;margin:1rem 0}
table{border-collapse:collapse;font-size:.92rem;min-width:100%}
th,td{border:1px solid var(--line);padding:.4rem .6rem;text-align:left;vertical-align:top}
th{background:var(--panel)}
td code,th code{white-space:nowrap}
footer{margin-top:4rem;color:var(--muted);font-size:.85rem;border-top:1px solid var(--line);padding-top:1rem}
.menu{display:none}
@media (max-width:900px){
 .layout{display:block}
 nav{position:static;height:auto;border-right:0;border-bottom:1px solid var(--line);padding:1rem 16px}
 nav .brand{display:inline-block;margin:0}
 nav .toc{display:none}
 nav.open .toc{display:block}
 .menu{display:inline-block;margin-left:.5rem;font:inherit;font-size:.85rem;padding:.2rem .6rem;border:1px solid var(--line);border-radius:6px;background:var(--panel);color:var(--fg)}
 nav input{display:none}
 nav.open input{display:block}
 main{padding:1.25rem 16px 4rem}
 h1{font-size:1.7rem}
}
@media print{nav{display:none}.layout{display:block}main{max-width:none}pre .copy{display:none}}
"""

JS = """
(function(){
  var nav=document.querySelector('nav'),filter=document.getElementById('filter');
  document.querySelector('.menu').addEventListener('click',function(){nav.classList.toggle('open')});
  filter.addEventListener('input',function(){
    var q=filter.value.toLowerCase();
    nav.querySelectorAll('.toc > li').forEach(function(top){
      var any=false;
      top.querySelectorAll('li').forEach(function(li){var hit=li.textContent.toLowerCase().indexOf(q)>=0;li.style.display=hit?'':'none';any=any||hit});
      var self=top.firstElementChild.textContent.toLowerCase().indexOf(q)>=0;
      top.style.display=(self||any)?'':'none';
      if(self&&!any)top.querySelectorAll('li').forEach(function(li){li.style.display=''});
    });
  });
  document.querySelectorAll('pre').forEach(function(pre){
    if(!navigator.clipboard)return;
    var b=document.createElement('button');b.className='copy';b.type='button';b.textContent='Copy';
    b.addEventListener('click',function(){navigator.clipboard.writeText(pre.innerText.replace(/Copy$/,'')).then(function(){b.textContent='Copied';setTimeout(function(){b.textContent='Copy'},1200)})});
    pre.appendChild(b);
  });
  var links={};nav.querySelectorAll('.toc a').forEach(function(a){links[a.getAttribute('href').slice(1)]=a});
  if('IntersectionObserver' in window){
    var current=null;
    var io=new IntersectionObserver(function(es){es.forEach(function(e){if(e.isIntersecting&&links[e.target.id]){if(current)current.classList.remove('active');current=links[e.target.id];current.classList.add('active')}})},{rootMargin:'0px 0px -75% 0px'});
    document.querySelectorAll('main h2[id],main h3[id]').forEach(function(h){io.observe(h)});
  }
})();
"""


def build():
    md = SRC.read_text(encoding="utf-8")
    lines = md.split("\n")
    # Section numbers -> anchors, so "§9.7" can link before the headings are rendered.
    sections = {}
    for line in lines:
        m = re.match(r"^#{2,3}\s+(\d+(?:\.\d+)?)\.?\s+(.*)$", line)
        if m:
            sections[m.group(1)] = "s" + m.group(1).replace(".", "-")
    blocks = Blocks(Inline(sections))

    # Title and the italic "last updated" line become the page header.
    title = "Dolphin Language Reference"
    if lines and lines[0].startswith("# "):
        title = lines[0][2:].strip()
        lines = lines[1:]
    body = blocks.parse(lines)
    subtitle = ""
    m = re.match(r"\s*<p><em>(.*?)</em></p>\n?", body, re.S)
    if m:
        subtitle, body = m.group(1), body[m.end():]

    toc, open_sub = ["<ul class=\"toc\">"], False
    for level, hid, text in blocks.headings:
        plain = re.sub(r'\s*<a class="anchor".*', "", text)
        if level == 2:
            if open_sub:
                toc.append("</ul></li>")
                open_sub = False
            elif len(toc) > 1:
                toc.append("</li>")
            toc.append(f'<li><a href="#{hid}">{plain}</a>')
        elif level == 3:
            if not open_sub:
                toc.append("<ul>")
                open_sub = True
            toc.append(f'<li><a href="#{hid}">{plain}</a></li>')
    toc.append("</ul></li></ul>" if open_sub else "</li></ul>")

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<meta name="description" content="Reference for Dolphin, the Amoebatu language: Python-shaped, statically analyzed, implicitly parallel.">
<meta name="color-scheme" content="light dark">
<!-- Generated from doc/REFERENCE.md by doc/build_reference_html.py. Edit the Markdown, not this file. -->
<style>{CSS}</style>
</head>
<body>
<div class="layout">
<nav aria-label="Contents">
<a class="brand" href="#top">Dolphin</a><button class="menu" type="button" aria-label="Show contents">Contents</button>
<input id="filter" type="search" placeholder="Filter sections" aria-label="Filter sections">
{"".join(toc)}
</nav>
<main id="top">
<h1>{html.escape(title)}</h1>
<p class="subtitle">{subtitle}</p>
{body}
<footer>Generated from <code>doc/REFERENCE.md</code>. Dolphin is part of the Amoebatu project.</footer>
</main>
</div>
<script>{JS}</script>
</body>
</html>
"""


def main():
    page = build()
    if "--check" in sys.argv[1:]:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != page:
            print(f"{OUT.relative_to(HERE.parent)} is out of date: run python3 doc/build_reference_html.py", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(page, encoding="utf-8")
    print(f"wrote {OUT.relative_to(HERE.parent)} ({len(page) // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
