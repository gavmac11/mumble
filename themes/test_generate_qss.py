import importlib.util
import tempfile
import unittest
from pathlib import Path


THEMES_DIRECTORY = Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("generate_qss", THEMES_DIRECTORY / "generate-qss.py")
GENERATE_QSS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATE_QSS)


class GeneratorTest(unittest.TestCase):
    def test_hyphens_and_underscores_are_bidirectionally_equivalent(self):
        self.assertEqual(
            GENERATE_QSS.resolve({"foo_bar": "1px", "from-hyphen": "$foo-bar"}),
            {"foo-bar": "1px", "from-hyphen": "1px"},
        )
        self.assertEqual(
            GENERATE_QSS.resolve({"foo-bar": "1px", "from_underscore": "$foo_bar"}),
            {"foo-bar": "1px", "from-underscore": "1px"},
        )

    def test_equivalent_local_declaration_overrides_import(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            (directory / "Imported.scss").write_text("$base_padding: 2px;\n", encoding="utf-8")
            source = directory / "Theme.scss"
            source.write_text("$base-padding: 4px;\n@import 'Imported';\n", encoding="utf-8")

            variables, _ = GENERATE_QSS.parse(source)

        self.assertEqual(variables, {"base-padding": "4px"})

    def test_compact_subtraction_and_negative_literals(self):
        self.assertEqual(GENERATE_QSS.evaluate_arithmetic("4px-3"), "1px")
        self.assertEqual(GENERATE_QSS.evaluate_arithmetic("4px--3"), "7px")
        self.assertEqual(GENERATE_QSS.evaluate_arithmetic("-3"), "-3")
        self.assertEqual(GENERATE_QSS.evaluate_arithmetic("4px * -3"), "-12px")

    def test_documented_directory_invocations_find_the_expected_source(self):
        expected = (THEMES_DIRECTORY / "Nord/source/Dark.scss").resolve()
        self.assertEqual(list(GENERATE_QSS.theme_sources(expected.parent)), [expected])
        self.assertEqual(list(GENERATE_QSS.theme_sources(expected.parent.parent)), [expected])

    def test_default_invocation_excludes_legacy_default_assets(self):
        sources = list(GENERATE_QSS.theme_sources(THEMES_DIRECTORY))
        self.assertEqual(len(sources), 10)
        self.assertNotIn("Default", {source.parent.parent.name for source in sources})

    def test_additional_themes_regenerate_byte_for_byte(self):
        for source in GENERATE_QSS.theme_sources(THEMES_DIRECTORY):
            with self.subTest(source=source):
                output = source.parent.parent / f"{source.stem}.qss"
                self.assertEqual(GENERATE_QSS.generate(source), output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
