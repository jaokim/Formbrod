#!/usr/bin/env python3
"""
catgen.py - locale catalog tool for Formbröd.

A small stand-in for FlexCat/CatComp, so catalogs can be built and checked
on any host with only Python 3.

  catgen.py header  formbrod.cd -o src/locale/strings.h
  catgen.py check   formbrod.cd svenska.ct [more.ct ...]
  catgen.py catalog formbrod.cd svenska.ct -o Catalogs/svenska/formbrod.catalog
  catgen.py dump    Catalogs/svenska/formbrod.catalog
  catgen.py compare a.catalog b.catalog

The release catalogs are built with FlexCat ('make catalogs'); 'compare'
checks that FlexCat and catgen agree.

.cd and .ct files are read as UTF-8 and written out in ISO-8859-1, which is
what AmigaOS expects. Text that has no ISO-8859-1 form is an error.

The catalog is the IFF FORM CTLG format read by locale.library, laid out
exactly as FlexCat writes it: FVER (version string), LANG (language name),
CSET (code set, 32 bytes) and STRS. In STRS each string is its ID, its
length without the NUL, then the text followed by one to four NULs so the
next entry starts on a four-byte boundary. Translations identical to the
built-in string are left out; locale.library then uses the built-in one.
"""
import argparse
import re
import struct
import sys

CATALOG_NAME = "formbrod.catalog"
CATALOG_VERSION = 1          # must match OC_Version in src/locale/catalog.c
TARGET_ENCODING = "iso-8859-1"

DEF_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\((\d*)/(\d*)/(\d*)\)\s*$")
NAME_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*$")
FORMAT_RE = re.compile(
    r"%(?:%|[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
    r"(?:hh|h|ll|l|L|q|z|j|t)?[diouxXeEfFgGcspaA])")
VERSION_RE = re.compile(r"^\$VER: (\S+) (\d+)\.(\d+)")

ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b",
           "f": "\f", "v": "\v", "e": "\x1b", "c": "\x9b", "\\": "\\",
           '"': '"'}


class CatError(Exception):
    pass


def unescape(text, where):
    out = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        i += 1
        if i >= len(text):
            raise CatError("%s: lone backslash at end of string" % where)
        ch = text[i]
        if ch in ESCAPES:
            out.append(ESCAPES[ch])
            i += 1
        elif ch == "x":
            m = re.match(r"[0-9A-Fa-f]{2}", text[i + 1:])
            if not m:
                raise CatError("%s: \\x needs two hex digits" % where)
            out.append(chr(int(m.group(0), 16)))
            i += 3
        elif ch in "01234567":
            m = re.match(r"[0-7]{1,3}", text[i:])
            out.append(chr(int(m.group(0), 8)))
            i += len(m.group(0))
        else:
            raise CatError("%s: unknown escape \\%s" % (where, ch))
    return "".join(out)


def read_lines(path):
    """Yields (line number, text) with backslash-continued lines joined."""
    with open(path, encoding="utf-8") as f:
        raw = f.read().split("\n")
    i = 0
    while i < len(raw):
        start = i + 1
        line = raw[i].rstrip("\r")
        # A trailing single backslash continues the line (not "\\").
        while line.endswith("\\") and not line.endswith("\\\\") \
                and i + 1 < len(raw):
            i += 1
            line = line[:-1] + raw[i].rstrip("\r")
        yield start, line
        i += 1


def parse_cd(path):
    """Returns a list of dicts: name, id, minlen, maxlen, text."""
    entries = []
    names, ids = set(), set()
    pending = None
    for lineno, line in read_lines(path):
        where = "%s:%d" % (path, lineno)
        if pending is not None:
            pending["text"] = unescape(line, where)
            entries.append(pending)
            pending = None
            continue
        if line == "" or line.startswith(";") or line.startswith("#"):
            continue
        m = DEF_RE.match(line)
        if not m:
            raise CatError("%s: expected 'NAME (id/min/max)'" % where)
        name, sid, smin, smax = m.groups()
        if not sid:
            raise CatError("%s: %s needs an explicit ID" % (where, name))
        sid = int(sid)
        if name in names:
            raise CatError("%s: %s defined twice" % (where, name))
        if sid in ids:
            raise CatError("%s: ID %d used twice" % (where, sid))
        names.add(name)
        ids.add(sid)
        pending = {"name": name, "id": sid,
                   "minlen": int(smin) if smin else None,
                   "maxlen": int(smax) if smax else None,
                   "where": where}
    if pending is not None:
        raise CatError("%s: %s has no string" % (path, pending["name"]))
    for e in entries:
        encode(e["text"], e["where"])
    return entries


def parse_ct(path):
    """Returns (header dict, {name: (text, where)})."""
    header = {}
    strings = {}
    pending = None
    for lineno, line in read_lines(path):
        where = "%s:%d" % (path, lineno)
        if pending is not None:
            if pending in strings:
                raise CatError("%s: %s translated twice" % (where, pending))
            strings[pending] = (unescape(line, where), where)
            pending = None
            continue
        if line.startswith("##"):
            parts = line[2:].strip().split(None, 1)
            if parts:
                header[parts[0].lower()] = parts[1] if len(parts) > 1 else ""
            continue
        if line == "" or line.startswith(";") or line.startswith("#"):
            continue
        m = NAME_RE.match(line)
        if not m:
            raise CatError("%s: expected a string name" % where)
        pending = m.group(1)
    if pending is not None:
        raise CatError("%s: %s has no string" % (path, pending))
    return header, strings


def encode(text, where):
    try:
        return text.encode(TARGET_ENCODING)
    except UnicodeEncodeError as exc:
        raise CatError("%s: %r has no %s form" %
                       (where, text[exc.start:exc.end], TARGET_ENCODING))


def formats(text):
    return [f for f in FORMAT_RE.findall(text) if f != "%%"]


def check(cd, ct_path):
    """Returns (errors, warnings, header, translated entries)."""
    errors, warnings = [], []
    header, strings = parse_ct(ct_path)
    by_name = {e["name"]: e for e in cd}

    ver = header.get("version", "")
    m = VERSION_RE.match(ver)
    if not m:
        errors.append("%s: missing or bad '## version $VER: %s %d.x (dd.mm.yyyy)'"
                      % (ct_path, CATALOG_NAME, CATALOG_VERSION))
    elif m.group(1) != CATALOG_NAME:
        errors.append("%s: version names %s, expected %s"
                      % (ct_path, m.group(1), CATALOG_NAME))
    elif int(m.group(2)) != CATALOG_VERSION:
        errors.append("%s: catalog version %s, the program asks for %d"
                      % (ct_path, m.group(2), CATALOG_VERSION))
    if not header.get("language"):
        errors.append("%s: missing '## language <name>'" % ct_path)

    out = []
    for name, (text, where) in strings.items():
        e = by_name.get(name)
        if e is None:
            errors.append("%s: %s is not in the .cd file" % (where, name))
            continue
        if text == "":
            warnings.append("%s: %s is empty; English is used" % (where, name))
            continue
        if formats(text) != formats(e["text"]):
            errors.append("%s: %s has conversions %s, the original has %s"
                          % (where, name, formats(text), formats(e["text"])))
        if e["text"].count("_") == 1 and text.count("_") != 1:
            errors.append("%s: %s needs exactly one '_' shortcut"
                          % (where, name))
        if e["text"].endswith("\n") != text.endswith("\n"):
            errors.append("%s: %s must %s in \\n" % (
                where, name, "end" if e["text"].endswith("\n") else "not end"))
        if e["maxlen"] is not None and len(text) > e["maxlen"]:
            errors.append("%s: %s is longer than %d characters"
                          % (where, name, e["maxlen"]))
        if e["minlen"] is not None and len(text) < e["minlen"]:
            errors.append("%s: %s is shorter than %d characters"
                          % (where, name, e["minlen"]))
        try:
            encode(text, where)
        except CatError as exc:
            errors.append(str(exc))
            continue
        if text == e["text"]:
            continue        # same as built in; FlexCat leaves these out too
        out.append((e["id"], text))
    for e in cd:
        if e["name"] not in strings:
            warnings.append("%s: %s is not translated; English is used"
                            % (ct_path, e["name"]))
    out.sort()
    return errors, warnings, header, out


def c_string(text):
    out = []
    prev = ""
    for b in encode(text, "header"):
        ch = chr(b)
        if ch == "\\" or ch == '"':
            out.append("\\" + ch)
        elif ch == "?" and prev == "?":
            out.append("\\077")         # no trigraphs
        elif 0x20 <= b < 0x7f:
            out.append(ch)
        else:
            out.append("\\%03o" % b)
        prev = ch
    return '"' + "".join(out) + '"'


def write_header(cd, cd_path, out_path):
    lines = [
        "/*",
        " * strings.h - GENERATED by tools/catgen.py from %s." % cd_path,
        " * Do not edit; change the .cd file and run 'make strings'.",
        " *",
        " * For each string: MSG_x is its catalog ID and MSG_x_STR the built-in",
        " * English text (ISO-8859-1). Define FB_STRINGS_ARRAY before including",
        " * to also get the FB_BuiltinStrings table.",
        " */",
        "#ifndef FORMBROD_STRINGS_H",
        "#define FORMBROD_STRINGS_H",
        "",
        '#define FB_CATALOG_NAME    "%s"' % CATALOG_NAME,
        "#define FB_CATALOG_VERSION %d" % CATALOG_VERSION,
        "",
    ]
    width = max(len(e["name"]) for e in cd) + 4
    for e in cd:
        lines.append("#define %-*s %d" % (width, e["name"], e["id"]))
        lines.append("#define %-*s %s" % (width, e["name"] + "_STR",
                                         c_string(e["text"])))
    lines += [
        "",
        "#define FB_NUM_STRINGS %d" % len(cd),
        "",
        "#ifdef FB_STRINGS_ARRAY",
        "static const struct FBBuiltinString {",
        "    long        id;",
        "    const char *str;",
        "} FB_BuiltinStrings[FB_NUM_STRINGS] = {",
    ]
    for e in cd:
        lines.append("    { %s, %s_STR }," % (e["name"], e["name"]))
    lines += ["};", "#endif", "", "#endif", ""]
    with open(out_path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))


def iff_chunk(tag, data):
    size = len(data)
    pad = b"\0" if size & 1 else b""
    return tag + struct.pack(">I", size) + data + pad


def build_catalog(header, strings):
    fver = encode(header["version"], "version") + b"\0"
    lang = encode(header["language"], "language") + b"\0"
    try:
        codeset = int(header.get("codeset", "0") or 0)
    except ValueError:
        raise CatError("## codeset must be a number")
    cset = struct.pack(">I", codeset) + b"\0" * 28
    strs = bytearray()
    for sid, text in strings:
        data = encode(text, "string %d" % sid)
        strs += struct.pack(">II", sid, len(data))
        strs += data + b"\0" * (4 - len(data) % 4)     # 1..4 NULs
    body = (b"CTLG" + iff_chunk(b"FVER", fver) + iff_chunk(b"LANG", lang) +
            iff_chunk(b"CSET", cset) + iff_chunk(b"STRS", bytes(strs)))
    return b"FORM" + struct.pack(">I", len(body)) + body


def read_catalog(data):
    """Parses a catalog. Returns (chunks dict, {id: text}). For tests."""
    if data[:4] != b"FORM" or data[8:12] != b"CTLG":
        raise CatError("not an IFF CTLG file")
    size = struct.unpack(">I", data[4:8])[0]
    if size + 8 != len(data):
        raise CatError("FORM size %d does not match file size %d"
                       % (size + 8, len(data)))
    pos, chunks = 12, {}
    while pos < len(data):
        tag = data[pos:pos + 4]
        clen = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        chunks[tag.decode("ascii")] = data[pos + 8:pos + 8 + clen]
        pos += 8 + clen + (clen & 1)
    strings = {}
    strs = chunks.get("STRS", b"")
    pos = 0
    while pos < len(strs):
        sid, slen = struct.unpack(">II", strs[pos:pos + 8])
        if strs[pos + 8 + slen:pos + 9 + slen] != b"\0":
            raise CatError("string %d is not NUL-terminated" % sid)
        strings[sid] = strs[pos + 8:pos + 8 + slen].decode(TARGET_ENCODING)
        pos += 8 + slen + (4 - slen % 4)
    return chunks, strings


def compare_catalogs(a_path, b_path):
    """Returns a list of differences that locale.library would notice."""
    with open(a_path, "rb") as f:
        a_data = f.read()
    with open(b_path, "rb") as f:
        b_data = f.read()
    a_chunks, a_strs = read_catalog(a_data)
    b_chunks, b_strs = read_catalog(b_data)
    diffs = []
    for tag in ("FVER", "LANG"):
        a_val = a_chunks.get(tag, b"").rstrip(b"\0")
        b_val = b_chunks.get(tag, b"").rstrip(b"\0")
        if a_val != b_val:
            diffs.append("%s: %r vs %r" % (tag, a_val, b_val))
    a_cs = a_chunks.get("CSET", b"\0\0\0\0")[:4]
    b_cs = b_chunks.get("CSET", b"\0\0\0\0")[:4]
    if a_cs != b_cs:
        diffs.append("CSET codeset: %r vs %r" % (a_cs, b_cs))
    for sid in sorted(set(a_strs) | set(b_strs)):
        if a_strs.get(sid) != b_strs.get(sid):
            diffs.append("string %d: %r vs %r"
                         % (sid, a_strs.get(sid), b_strs.get(sid)))
    if not diffs and a_data != b_data:
        diffs.append("same strings, but the bytes differ (chunks %s vs %s)"
                     % (sorted(a_chunks), sorted(b_chunks)))
    return diffs


def report(errors, warnings, quiet):
    if not quiet:
        for w in warnings:
            print("warning: " + w, file=sys.stderr)
    for e in errors:
        print("error: " + e, file=sys.stderr)
    return not errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("header", help="write the C header from a .cd")
    p.add_argument("cd")
    p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("check", help="check .ct files against a .cd")
    p.add_argument("cd")
    p.add_argument("ct", nargs="+")
    p.add_argument("-q", "--quiet", action="store_true",
                   help="don't print warnings")
    p = sub.add_parser("catalog", help="compile a .ct into a .catalog")
    p.add_argument("cd")
    p.add_argument("ct")
    p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("dump", help="print the strings in a .catalog")
    p.add_argument("catalog")
    p = sub.add_parser("compare",
                       help="compare two .catalog files, e.g. FlexCat's and ours")
    p.add_argument("a")
    p.add_argument("b")
    args = ap.parse_args(argv)

    try:
        if args.cmd == "compare":
            diffs = compare_catalogs(args.a, args.b)
            for d in diffs:
                print("differs: " + d, file=sys.stderr)
            if not diffs:
                print("identical: %s, %s" % (args.a, args.b))
            return 1 if diffs else 0
        if args.cmd == "dump":
            with open(args.catalog, "rb") as f:
                chunks, strings = read_catalog(f.read())
            for tag in ("FVER", "LANG"):
                print("%s: %s" % (tag, chunks.get(tag, b"").rstrip(b"\0")
                                  .decode(TARGET_ENCODING)))
            for sid in sorted(strings):
                print("%5d  %r" % (sid, strings[sid]))
            return 0

        cd = parse_cd(args.cd)
        if args.cmd == "header":
            write_header(cd, args.cd, args.output)
            return 0
        if args.cmd == "check":
            ok = True
            for ct in args.ct:
                errors, warnings, _, _ = check(cd, ct)
                ok = report(errors, warnings, args.quiet) and ok
            return 0 if ok else 1
        if args.cmd == "catalog":
            errors, warnings, header, strings = check(cd, args.ct)
            if not report(errors, warnings, True):
                return 1
            with open(args.output, "wb") as f:
                f.write(build_catalog(header, strings))
            return 0
    except (CatError, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
