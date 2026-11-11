"""
Tests for tools/catgen.py. Run with:  make -f Makefile.host test
"""
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import catgen  # noqa: E402

CD = os.path.join(ROOT, "locale", "formbrod.cd")
HEADER = os.path.join(ROOT, "src", "locale", "strings.h")

MINI_CD = """\
; test
MSG_HELLO (1//)
Hello
;
MSG_ADDED (2//)
Added %s
;
MSG_BUTTON (3//)
_Save
;
MSG_LINE (4//)
Oops\\n
"""

CT_HEAD = """\
## version $VER: formbrod.catalog 1.0 (10.11.2026)
## language svenska
## codeset 0
"""


class CatgenTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text):
        path = os.path.join(self.tmp.name, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path

    def check(self, ct_body):
        cd = catgen.parse_cd(self.write("t.cd", MINI_CD))
        return catgen.check(cd, self.write("t.ct", CT_HEAD + ct_body))

    def test_header_is_up_to_date(self):
        out = os.path.join(self.tmp.name, "strings.h")
        catgen.write_header(catgen.parse_cd(CD), "locale/formbrod.cd", out)
        with open(out) as a, open(HEADER) as b:
            self.assertEqual(a.read(), b.read(),
                             "strings.h is stale: run 'make strings'")

    def test_round_trip(self):
        errors, _, header, strings = self.check(
            "MSG_HELLO\nHej på dig\nMSG_ADDED\nLade till %s\n"
            "MSG_LINE\nHoppsan\\n\n")
        self.assertEqual(errors, [])
        data = catgen.build_catalog(header, strings)
        self.assertEqual(len(data) % 2, 0)
        chunks, got = catgen.read_catalog(data)
        self.assertEqual(got, {1: "Hej på dig", 2: "Lade till %s",
                               4: "Hoppsan\n"})
        self.assertEqual(chunks["LANG"], b"svenska\0")
        self.assertEqual(len(chunks["CSET"]), 32)
        self.assertIn("\xe5".encode("iso-8859-1"), data)   # å in Latin-1

    def test_flexcat_layout(self):
        # Layout taken from a FlexCat-built catalog: the length excludes the
        # NUL unless the text is a multiple of 4 long, padding to 4 bytes,
        # and translations equal to the original are left out.
        errors, _, header, strings = self.check(
            "MSG_HELLO\nHello\nMSG_BUTTON\n_Spar\nMSG_ADDED\nLade %s\n"
            "MSG_LINE\nHoppsan\\n\n")
        self.assertEqual(errors, [])
        self.assertEqual([sid for sid, _ in strings], [2, 3, 4])
        data = catgen.build_catalog(header, strings)
        strs = data[data.index(b"STRS") + 8:]
        self.assertEqual(strs[:16], b"\0\0\0\2\0\0\0\7Lade %s\0")
        self.assertEqual(strs[16:32], b"\0\0\0\3\0\0\0\5_Spar\0\0\0")
        self.assertEqual(strs[32:52],
                         b"\0\0\0\4\0\0\0\x09Hoppsan\n\0\0\0\0")
        self.assertEqual(catgen.read_catalog(data)[1][4], "Hoppsan\n")

    def test_compare(self):
        _, _, header, strings = self.check("MSG_HELLO\nHej\nMSG_ADDED\nLade till %s\n")
        a = self.write("a.ct", "")
        b = self.write("b.ct", "")
        with open(a, "wb") as f:
            f.write(catgen.build_catalog(header, strings))
        with open(b, "wb") as f:
            f.write(catgen.build_catalog(header, strings))
        self.assertEqual(catgen.compare_catalogs(a, b), [])
        with open(b, "wb") as f:
            f.write(catgen.build_catalog(header, [(1, "Hej!"), strings[1]]))
        diffs = catgen.compare_catalogs(a, b)
        self.assertEqual(len(diffs), 1)
        self.assertIn("string 1", diffs[0])

    def test_format_mismatch(self):
        errors, _, _, _ = self.check("MSG_ADDED\nLade till %d\n")
        self.assertTrue(any("conversions" in e for e in errors))
        errors, _, _, _ = self.check("MSG_ADDED\nLade till\n")
        self.assertTrue(any("conversions" in e for e in errors))

    def test_shortcut_and_newline(self):
        errors, _, _, _ = self.check("MSG_BUTTON\nSpara\n")
        self.assertTrue(any("shortcut" in e for e in errors))
        errors, _, _, _ = self.check("MSG_LINE\nHoppsan\n")
        self.assertTrue(any("\\n" in e for e in errors))

    def test_unknown_name_and_missing_version(self):
        cd = catgen.parse_cd(self.write("t.cd", MINI_CD))
        errors, _, _, _ = catgen.check(
            cd, self.write("t.ct", "## language svenska\nMSG_NOPE\nx\n"))
        self.assertTrue(any("not in the .cd" in e for e in errors))
        self.assertTrue(any("version" in e for e in errors))

    def test_not_latin1(self):
        errors, _, _, _ = self.check("MSG_HELLO\nПривет\n")
        self.assertTrue(any("iso-8859-1" in e for e in errors))

    def test_missing_translation_falls_back(self):
        errors, warnings, _, strings = self.check("MSG_HELLO\nHej\n")
        self.assertEqual(errors, [])
        self.assertEqual(strings, [(1, "Hej")])
        self.assertTrue(any("MSG_ADDED" in w for w in warnings))

    def test_duplicate_id(self):
        path = self.write("dup.cd", "A (1//)\na\nB (1//)\nb\n")
        with self.assertRaises(catgen.CatError):
            catgen.parse_cd(path)

    def test_not_latin1(self):
        path = self.write("arrow.cd", "; Settings \u2192 Log\nA (1//)\na\n")
        with self.assertRaises(catgen.CatError):
            catgen.parse_cd(path)

    def test_ids_out_of_order(self):
        path = self.write("order.cd", "A (2//)\na\nB (1//)\nb\n")
        with self.assertRaises(catgen.CatError):
            catgen.parse_cd(path)

    def test_c_string(self):
        self.assertEqual(catgen.c_string('Formbröd "x"\\?\n'),
                         '"Formbr\\366d \\"x\\"\\\\?\\012"')
        self.assertEqual(catgen.c_string("??="), '"?\\077="')


if __name__ == "__main__":
    unittest.main()
