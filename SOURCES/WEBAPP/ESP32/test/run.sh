#!/usr/bin/env bash
# Runs every host test: each test_*.cpp and test_*.js in this folder, the
# test_*.py here and the sigma2aurora pytest suite. Exits non-zero and names
# the failing tests if any fails. Works in Linux and in Git Bash on Windows.
#
# Run from anywhere: SOURCES/WEBAPP/ESP32/test/run.sh
#
# A C++ test lists the firmware sources it links and the libraries it uses in
# its header, so a new test needs no change here:
#   // Links: matrixmodel.cpp AudioFilterFactory.cpp   (from ../aurora)
#   // Uses: ArduinoJson
#
# ArduinoJson (header-only) is taken from $ARDUINOJSON_DIR, else from
# ~/Arduino/libraries/ArduinoJson or ~/Documents/Arduino/libraries/ArduinoJson.
#
# Environment: CXX (default g++), NODE (default node), PYTHON (default python3,
# else python). AURORA_HOST_TEST_DIR and AURORA_PYTEST_DIR replace the two test
# folders; the script's own tests use them.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
HOST_DIR="${AURORA_HOST_TEST_DIR:-$HERE}"
PYTEST_DIR="${AURORA_PYTEST_DIR:-$REPO/SOURCES/sigma2aurora/tests}"
AURORA_DIR="$HOST_DIR/../aurora"
CXX="${CXX:-g++}"
NODE="${NODE:-node}"

if [ -z "${PYTHON:-}" ]; then
  if python3 -c '' >/dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

failed=()
passed=0

step() {  # step <name> <command...>: runs the command, records the result
  local name="$1"
  shift
  echo "== $name"
  if "$@"; then
    passed=$((passed + 1))
    return 0
  fi
  echo "FAILED: $name"
  failed+=("$name")
  return 1
}

# Prints the words after "// <key>:" in the header of a test file.
header_words() {
  sed -n "s|^// $2:[[:space:]]*||p" "$1" | head -n 1
}

find_arduinojson() {
  local dir
  for dir in "${ARDUINOJSON_DIR:-}" "$HOME/Arduino/libraries/ArduinoJson" \
             "$HOME/Documents/Arduino/libraries/ArduinoJson"; do
    [ -n "$dir" ] || continue
    if [ -f "$dir/src/ArduinoJson.h" ]; then echo "$dir/src"; return 0; fi
    if [ -f "$dir/ArduinoJson.h" ]; then echo "$dir"; return 0; fi
  done
  return 1
}

cpp_test() {
  local test="$1" bin="$BUILD/$(basename "$1" .cpp)"
  local sources=() flags=() word inc
  for word in $(header_words "$test" Links); do
    sources+=("$AURORA_DIR/$word")
  done
  for word in $(header_words "$test" Uses); do
    case "$word" in
      ArduinoJson)
        if ! inc="$(find_arduinojson)"; then
          echo "ArduinoJson not found. Install it in ~/Arduino/libraries or" \
               "~/Documents/Arduino/libraries, or set ARDUINOJSON_DIR to its folder."
          return 1
        fi
        flags+=(-I "$inc") ;;
      *) echo "unknown library in Uses: $word"; return 1 ;;
    esac
  done
  "$CXX" -std=gnu++11 -Wall "${flags[@]}" -o "$bin" "$test" "${sources[@]}" && "$bin"
}

js_test() {
  (cd "$REPO" && "$NODE" "$1")
}

# Runs one pytest folder; the failing test ids go to $BUILD/pytest-failed.
py_tests() {
  local log="$BUILD/pytest.log" status
  (cd "$1" && PYTHONDONTWRITEBYTECODE=1 "$PYTHON" -m pytest -q -rfE -p no:cacheprovider \
     --rootdir "$1" .) 2>&1 | tee "$log"
  status=${PIPESTATUS[0]}
  sed -nE 's/^(FAILED|ERROR) ([^ ]*).*/\2/p' "$log" > "$BUILD/pytest-failed"
  return "$status"
}

py_step() {  # py_step <dir>: one step per folder, the summary names each failing test
  local label="${1#"$REPO"/}" id
  step "$label" py_tests "$1" && return 0
  unset 'failed[${#failed[@]}-1]'
  while read -r id; do failed+=("$label/$id"); done < "$BUILD/pytest-failed"
  [ -s "$BUILD/pytest-failed" ] || failed+=("$label")
}

has_tests() {  # has_tests <dir> <glob>
  compgen -G "$1/$2" >/dev/null
}

for test in "$HOST_DIR"/test_*.cpp; do
  [ -f "$test" ] && step "$(basename "$test")" cpp_test "$test"
done
for test in "$HOST_DIR"/test_*.js; do
  [ -f "$test" ] && step "$(basename "$test")" js_test "$test"
done
has_tests "$HOST_DIR" 'test_*.py' && py_step "$HOST_DIR"
has_tests "$PYTEST_DIR" 'test_*.py' && py_step "$PYTEST_DIR"

echo
if [ ${#failed[@]} -eq 0 ]; then
  echo "All $passed host test suites passed."
  exit 0
fi
echo "Host tests failed ($passed suites passed):"
printf '  %s\n' "${failed[@]}"
exit 1
