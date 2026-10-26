from pathlib import Path
import re
import unittest


class VersionTests(unittest.TestCase):
    def test_cpp_and_python_versions_match(self):
        root = Path(__file__).resolve().parents[1]
        cmake = re.search(r'project\(hft_lob_engine VERSION ([\d.]+)', (root/'CMakeLists.txt').read_text()).group(1)
        package = re.search(r'^version = "([\d.]+)"', (root/'pyproject.toml').read_text(), re.M).group(1)
        header = (root/'include/hft/Version.hpp').read_text()
        string = re.search(r'version_string = "([\d.]+)"', header).group(1)
        macros = '.'.join(re.search(r'HFT_SDK_VERSION_' + part + r' (\d+)', header).group(1)
                          for part in ('MAJOR', 'MINOR', 'PATCH'))
        self.assertEqual({cmake, package, string, macros}, {cmake})


class StressHarnessTests(unittest.TestCase):
    def test_expected_empty_failure_does_not_hide_crashes(self):
        from tools.run_stress import RunResult, case_passed
        def result(code, timeout=False):
            return RunResult('paper_eval/empty', [], code, 0, 0, timed_out=timeout,
                             stderr='tick file contains no events')
        self.assertTrue(case_passed(result(1)))
        for code, timeout in ((0, False), (-11, False), (1, True), (2, False)):
            self.assertFalse(case_passed(result(code, timeout)))
