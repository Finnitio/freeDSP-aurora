"""Tests for run.sh, the one command that runs all host tests.

Each test runs the script on a scratch test folder, so the real suites are not
run again from here.

Run: SOURCES/WEBAPP/ESP32/test/run.sh
"""
import os
import site
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
RUN_SH = os.path.join(HERE, 'run.sh')

PASSING_CPP = '#include <cassert>\nint main() { assert(1 + 1 == 2); return 0; }\n'
FAILING_CPP = '#include <cstdio>\nint main() { std::puts("boom"); return 1; }\n'
PASSING_JS = 'require("assert")(true);\n'
FAILING_JS = 'require("assert").strictEqual(1, 2);\n'
PASSING_PY = 'def test_ok():\n    assert True\n'
FAILING_PY = 'def test_bad():\n    assert False\n'


@pytest.fixture
def tree(tmp_path):
    """A scratch layout: host tests in esp32/test, pytest suite in s2a/tests."""
    host = tmp_path / 'esp32' / 'test'
    py = tmp_path / 's2a' / 'tests'
    home = tmp_path / 'home'
    for d in (host, py, home, tmp_path / 'esp32' / 'aurora'):
        d.mkdir(parents=True)
    return tmp_path


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def run(tree, **env_extra):
    env = dict(os.environ)
    env.pop('ARDUINOJSON_DIR', None)
    env['HOME'] = str(tree / 'home')
    env['AURORA_HOST_TEST_DIR'] = str(tree / 'esp32' / 'test')
    env['AURORA_PYTEST_DIR'] = str(tree / 's2a' / 'tests')
    env['PYTHON'] = sys.executable
    env['PYTHONUSERBASE'] = site.getuserbase()  # pytest may live there; HOME is faked
    env.update(env_extra)
    proc = subprocess.run(['bash', RUN_SH], env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, universal_newlines=True)
    return proc.returncode, proc.stdout


def test_all_passing_suites_exit_zero(tree):
    write(tree / 'esp32' / 'test' / 'test_a.cpp', PASSING_CPP)
    write(tree / 'esp32' / 'test' / 'test_b.js', PASSING_JS)
    write(tree / 's2a' / 'tests' / 'test_c.py', PASSING_PY)
    code, out = run(tree)
    assert code == 0, out
    for name in ('test_a.cpp', 'test_b.js', 'tests'):
        assert name in out


def assert_failed(code, out, name):
    assert code != 0, out
    summary = out.split('Host tests failed')[1]
    assert name in summary, out


def test_failing_cpp_test_is_named_and_the_rest_still_runs(tree):
    write(tree / 'esp32' / 'test' / 'test_bad.cpp', FAILING_CPP)
    write(tree / 'esp32' / 'test' / 'test_good.cpp', PASSING_CPP)
    write(tree / 's2a' / 'tests' / 'test_c.py', PASSING_PY)
    code, out = run(tree)
    assert_failed(code, out, 'test_bad.cpp')
    assert 'test_good.cpp' not in out.split('Host tests failed')[1]
    assert '== test_good.cpp' in out


def test_cpp_test_that_does_not_compile_is_named(tree):
    write(tree / 'esp32' / 'test' / 'test_broken.cpp', 'int main() { return x; }\n')
    code, out = run(tree)
    assert_failed(code, out, 'test_broken.cpp')


def test_failing_js_test_is_named(tree):
    write(tree / 'esp32' / 'test' / 'test_bad.js', FAILING_JS)
    code, out = run(tree)
    assert_failed(code, out, 'test_bad.js')


def test_failing_pytest_suite_is_named(tree):
    write(tree / 's2a' / 'tests' / 'test_bad.py', FAILING_PY)
    code, out = run(tree)
    assert_failed(code, out, 'test_bad.py::test_bad')


def test_cpp_test_links_the_firmware_sources_from_its_header(tree):
    write(tree / 'esp32' / 'aurora' / 'twice.h', 'int twice(int x);\n')
    write(tree / 'esp32' / 'aurora' / 'twice.cpp', 'int twice(int x) { return 2 * x; }\n')
    write(tree / 'esp32' / 'test' / 'test_twice.cpp',
          '// Links: twice.cpp\n#include "../aurora/twice.h"\n'
          'int main() { return twice(2) == 4 ? 0 : 1; }\n')
    code, out = run(tree)
    assert code == 0, out


JSON_TEST = ('// Uses: ArduinoJson\n#include <ArduinoJson.h>\n'
             'int main() { return ARDUINOJSON_FAKE == 42 ? 0 : 1; }\n')


def fake_arduinojson(lib):
    """A stand-in library folder laid out like the real one (src/ArduinoJson.h)."""
    write(lib / 'src' / 'ArduinoJson.h', '#define ARDUINOJSON_FAKE 42\n')


@pytest.mark.parametrize('libraries', ['Arduino/libraries', 'Documents/Arduino/libraries'])
def test_arduinojson_is_found_in_the_usual_library_folders(tree, libraries):
    fake_arduinojson(tree / 'home' / libraries / 'ArduinoJson')
    write(tree / 'esp32' / 'test' / 'test_json.cpp', JSON_TEST)
    code, out = run(tree)
    assert code == 0, out


def test_arduinojson_dir_from_the_environment_wins(tree):
    fake_arduinojson(tree / 'elsewhere' / 'ArduinoJson')
    write(tree / 'home' / 'Arduino' / 'libraries' / 'ArduinoJson' / 'src' / 'ArduinoJson.h',
          '#error wrong ArduinoJson\n')
    write(tree / 'esp32' / 'test' / 'test_json.cpp', JSON_TEST)
    code, out = run(tree, ARDUINOJSON_DIR=str(tree / 'elsewhere' / 'ArduinoJson'))
    assert code == 0, out


def test_missing_arduinojson_says_where_it_looked(tree):
    write(tree / 'esp32' / 'test' / 'test_json.cpp', JSON_TEST)
    code, out = run(tree)
    assert_failed(code, out, 'test_json.cpp')
    assert 'ArduinoJson not found' in out
    assert 'ARDUINOJSON_DIR' in out
