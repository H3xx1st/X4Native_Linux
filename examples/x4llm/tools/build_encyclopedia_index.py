#!/usr/bin/env python3
"""
build_encyclopedia_index.py — offline X4 encyclopedia indexer for x4llm.

The X4 encyclopedia has no single data file. Entry DEFINITIONS are scattered
throughout the readable game archives as element opening tags carrying text
references:  <ware id="..." name="{page,line}" description="{page,line}"/>.
The actual TEXT lives in the English language file (t/0001-l044.xml, language
id 44) as <page id><t id>text</t></page>, and supports inline {page,line}
cross-references plus backslash escapes.

This tool:
  1. loads the English language file -> (page,line) -> text, plus page titles;
  2. scans the definition archives for every element with a name reference;
  3. resolves name/description references (and inline cross-refs) to plain text;
  4. categorizes each entry by its encyclopedia page title;
  5. writes a compact flat index for the x4llm C++ extension to retrieve from.

Output format: one record per line, fields separated by 0x1F (unit separator):
    <category> \x1f <id> \x1f <name> \x1f <description>

Usage:
    python3 build_encyclopedia_index.py \
        --game-dir "/path/to/X4 Foundations" \
        --out "/path/to/x4llm/data/encyclopedia.index"

Requires the `x4cat` tool on PATH (to extract the language + faction XMLs).
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

LANG_XML = "t/0001-l044.xml"          # English text (language id 44)
DEF_ARCHIVE = "08.dat"                # readable archive holding most definitions
EXTRA_MEMBERS = ["libraries/factions.xml"]  # full faction list lives elsewhere

# Encyclopedia records are <identification> elements spread across these LARGE
# archives (01.dat is ~10 GB). They carry name/description refs but NO id, so
# they are grepped out (fast even for multi-GB files) instead of slurped.
IDENT_ARCHIVES = ["01.dat", "02.dat", "05.dat"]

REF_RE = re.compile(r"\{(\d+)\s*,\s*(\d+)\}")
ELEMENT_RE = re.compile(r'<([A-Za-z0-9_]+)\s[^>]*name="\{\d+\s*,\s*\d+\}"[^>]*>')
US = "\x1f"
BS = chr(92)   # backslash, used to avoid escape headaches
NL = "\x00"    # temporary sentinel for protected backslashes


def extract_member(game_dir, work_dir, member):
    r = subprocess.run(
        ["x4cat", "extract", game_dir, "-o", work_dir, "-g", member],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"warning: x4cat extract failed for {member}: {r.stderr.strip()}", file=sys.stderr)


def load_language(lang_path):
    """Return (text[(page,line)] -> str, page_title[page] -> str)."""
    text, titles = {}, {}
    for page in ET.parse(lang_path).getroot().iter("page"):
        pid = page.get("id")
        if not pid:
            continue
        title = page.get("title") or ""
        if title:
            titles[pid] = title
        for t in page.iter("t"):
            tid = t.get("id")
            if tid is not None:
                text[(pid, tid)] = "".join(t.itertext())
    return text, titles


def clean(s):
    if not s:
        return ""
    # De-escape X4 text: backslash-n -> space (paragraph); backslash-X -> X.
    s = s.replace(BS + BS, NL)                    # protect real backslashes
    s = s.replace(BS + "n", " ")                  # paragraph break
    for ch in "()[]'\"<>,;:":
        s = s.replace(BS + ch, ch)
    s = s.replace(NL, BS)
    s = s.replace("\r", " ").replace("\n", " ").replace("\t", " ")
    return re.sub(r"\s+", " ", s).strip()


def resolve(text_map, s, depth=3, _seen=None):
    """Replace inline {page,line} cross-references with their resolved text."""
    if s is None:
        return ""
    if _seen is None:
        _seen = set()
    if depth <= 0:
        return clean(REF_RE.sub(" ", s))

    def repl(m):
        key = (m.group(1), m.group(2))
        if key in _seen:
            return ""
        val = text_map.get(key)
        if val is None:
            return ""
        _seen.add(key)
        return resolve(text_map, val, depth - 1, _seen)

    return clean(REF_RE.sub(repl, s))


def lookup(text_map, ref):
    if not ref:
        return None
    m = REF_RE.fullmatch(ref.strip())
    return text_map.get((m.group(1), m.group(2))) if m else None


def scan_elements(data, seen, elems):
    """Scan archive text for element tags with a name ref, deduping on
    (tag, id-or-name-ref). Encyclopedia records (<identification>) carry NO id,
    so id is optional here and the name ref is used as the identity instead."""
    for m in ELEMENT_RE.finditer(data):
        line = m.group(0)
        mname = re.search(r'\bname="(\{\d+\s*,\s*\d+\})"', line)
        if not mname:
            continue
        mid = re.search(r'\bid="([^"]+)"', line)
        mdesc = re.search(r'\bdescription="(\{\d+\s*,\s*\d+\})"', line)
        # Key on (tag, id) when present, else (tag, name-ref): the same id can
        # appear on several element types (e.g. "xenon" is a <faction>, a
        # <race> and a <compatibilitytype>), so id alone would shadow the real
        # lore. Name-refs are unique per encyclopedia entry.
        key = (m.group(1), mid.group(1) if mid else mname.group(1))
        if key in seen:
            continue
        seen.add(key)
        elems.append({
            "id": key[1],
            "tag": m.group(1),
            "name": mname.group(1),
            "desc": mdesc.group(1) if mdesc else None,
        })


def grep_identification(path):
    """Extract <identification ...> lines from a large archive via grep."""
    try:
        r = subprocess.run(["grep", "-aoE", r"<identification [^>]*>", path],
                           capture_output=True, text=True, timeout=600)
        return r.stdout
    except Exception as e:
        print(f"warning: grep identification failed for {path}: {e}", file=sys.stderr)
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", required=True, help="Path to the X4 install")
    ap.add_argument("--def-archive", default=DEF_ARCHIVE,
                    help="Readable archive holding item definitions (default 08.dat)")
    ap.add_argument("--out", required=True, help="Output index file path")
    args = ap.parse_args()

    game = os.path.abspath(args.game_dir)
    work = tempfile.mkdtemp(prefix="x4enc_")
    print("extracting language file ...", file=sys.stderr)
    extract_member(game, work, LANG_XML)
    lang_path = os.path.join(work, *LANG_XML.split("/"))
    if not os.path.exists(lang_path):
        sys.exit(f"error: language file not found at {lang_path}")

    print("loading language text ...", file=sys.stderr)
    text_map, page_titles = load_language(lang_path)
    print(f"  {len(text_map)} text entries, {len(page_titles)} page titles", file=sys.stderr)

    seen, elems = set(), []
    archive = os.path.join(game, args.def_archive)
    if not os.path.exists(archive):
        sys.exit(f"error: definition archive not found at {archive}")
    print(f"scanning {args.def_archive} for item definitions ...", file=sys.stderr)
    with open(archive, "r", encoding="utf-8", errors="replace") as f:
        scan_elements(f.read(), seen, elems)

    # Encyclopedia records (<identification>) are spread across the large data
    # archives; grep them out and scan. (08.dat's own are already captured above.)
    for arc in IDENT_ARCHIVES:
        p = os.path.join(game, arc)
        if not os.path.exists(p):
            continue
        print(f"scanning {arc} for <identification> encyclopedia records ...", file=sys.stderr)
        scan_elements(grep_identification(p), seen, elems)

    # Some definition files (notably the full factions list) live in other
    # archives than the default one; extract the known ones and scan them too.
    for member in EXTRA_MEMBERS:
        extract_member(game, work, member)
        p = os.path.join(work, *member.split("/"))
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                scan_elements(f.read(), seen, elems)

    print(f"  {len(elems)} elements with a name reference", file=sys.stderr)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    n = 0
    with open(args.out, "w", encoding="utf-8") as f:
        for e in elems:
            name = resolve(text_map, lookup(text_map, e["name"]) or e["name"])
            if not name:
                continue
            desc = resolve(text_map, lookup(text_map, e["desc"]) or e["desc"]) if e["desc"] else ""
            # Skip pure section markers like "(Generic appended description texts)".
            if name.startswith("(") and name.endswith(")") and len(name) < 40 and not desc:
                continue
            page = REF_RE.fullmatch(e["name"].strip()).group(1)
            category = page_titles.get(page, e["tag"])
            f.write(US.join([category, e["id"], name, desc]).replace("\n", " ") + "\n")
            n += 1

    print(f"wrote {n} encyclopedia entries -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
