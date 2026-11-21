"""Tests for tools/check_content.py: the test-name guard (a ';' in a doctest name)."""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import check_content  # noqa: E402


class TestNames(unittest.TestCase):
    def test_semicolon_names_are_found(self):
        bad = check_content.bad_test_names
        self.assertEqual(bad('TEST_CASE("dbno body: a remade puppet gets the key again; a gone one") {'),
                         ["dbno body: a remade puppet gets the key again; a gone one"])
        self.assertEqual(bad('    SUBCASE("up; down") {'), ["up; down"])
        self.assertEqual(bad('SCENARIO("a;b")'), ["a;b"])
        self.assertEqual(bad('TEST_CASE_TEMPLATE("t;", T, int, long)'), ["t;"])
        # The fixture's name is the second argument (tests/CMakeLists.txt's regex missed it before).
        self.assertEqual(bad('TEST_CASE_FIXTURE(Rig, "with a rig; twice")'), ["with a rig; twice"])
        self.assertEqual(bad('TEST_CASE_FIXTURE(ns::Rig<int>, "x;y")'), ["x;y"])

    def test_good_names_pass(self):
        bad = check_content.bad_test_names
        self.assertEqual(bad('TEST_CASE("a remade puppet gets the key again, a gone one: never called") {'), [])
        self.assertEqual(bad('TEST_CASE("x") { int a = 1; int b = 2; }'), [])  # ';' after the name
        self.assertEqual(bad('SUBCASE("quote \\" inside, no semicolon") {'), [])
        self.assertEqual(bad('CHECK(s == "a;b");'), [])  # not a test name
        self.assertEqual(bad('MY_TEST_CASE("a;b")'), [])  # another macro

    def test_escaped_quote_does_not_end_the_name(self):
        self.assertEqual(check_content.bad_test_names('TEST_CASE("say \\"hi\\"; then go")'), ['say \\"hi\\"; then go'])


class TestNameProblems(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.root)

    def write(self, path, text):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as f:
            f.write(text)

    def test_only_cpp_sources_under_tests(self):
        line = 'TEST_CASE("one; two") {}\n'
        self.write("tests/lichii/a_test.cpp", "// x\n" + line)
        self.write("tests/lichii/b.h", line)
        self.write("tools/x.cpp", line)          # not under tests/
        self.write("tests/notes.md", line)       # not a source
        p = check_content.test_name_problems
        self.assertEqual(p(self.root, "tests/lichii/a_test.cpp"), ['line 2: "one; two"'])
        self.assertEqual(p(self.root, "tests/lichii/b.h"), ['line 1: "one; two"'])
        self.assertEqual(p(self.root, "tools/x.cpp"), [])
        self.assertEqual(p(self.root, "tests/notes.md"), [])

    def test_the_repository_has_none(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        found = [f"{path}: {w}" for path in check_content.tracked_files(root)
                 for w in check_content.test_name_problems(root, path)]
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
