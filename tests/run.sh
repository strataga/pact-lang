#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build

# Check both frontends through the compiler's check path before running either
# representation. Successful check output is intentionally not compared: only
# runtime behavior is part of the text/JSON equivalence contract.
./pact check --allow math.double examples/double.pact > build/text.check.out
./pact check --allow math.double examples/double.json > build/json.check.out

./pact run --allow math.double examples/double.pact > build/text.out
./pact run --allow math.double examples/double.json > build/json.out
diff -u build/text.out build/json.out
grep -qx '42' build/text.out

# Unversioned JSON stays compatible as v1, and an explicit v1 behaves the same.
python3 - <<'PY'
import json

with open("examples/double.json", encoding="utf-8") as stream:
    program = json.load(stream)
program["version"] = 1
with open("build/version-1.json", "w", encoding="utf-8") as stream:
    json.dump(program, stream)
program["version"] = 999
with open("build/version-unsupported.json", "w", encoding="utf-8") as stream:
    json.dump(program, stream)
PY
./pact run --allow math.double build/version-1.json > build/version-1.out
grep -qx '42' build/version-1.out
if ./pact check --allow math.double build/version-unsupported.json > build/version-unsupported.out 2>&1; then
  echo 'expected unsupported JSON AST version to fail' >&2
  exit 1
fi

# Explicit compilation must produce a native executable with the same result.
./pact compile -o build/native --allow math.double examples/double.pact > build/native.compile.out
./build/native > build/native.out
grep -qx '42' build/native.out

# The text frontend accepts the full signed i64 range.
cat > build/min-i64.pact <<'PACT'
return -9223372036854775808;
PACT
./pact run build/min-i64.pact > build/min-i64.out
grep -qx -- '-9223372036854775808' build/min-i64.out

# Declaring a capability in a program is not enough: the host must grant it.
if ./pact check examples/double.pact > build/denied.out 2>&1; then
  echo 'expected missing tool grant to fail' >&2
  exit 1
fi

# Unknown names are type errors reported as machine-readable JSON too.
cat > build/unknown-variable.pact <<'PACT'
return missing;
PACT
if ./pact check build/unknown-variable.pact > build/unknown-variable.out 2>&1; then
  echo 'expected unknown variable check to fail' >&2
  exit 1
fi

# Deeply nested JSON expressions fail with a diagnostic instead of exhausting
# the compiler's parser stack.
python3 - <<'PY'
import json

expr = {"int": 0}
for _ in range(128):
    expr = {"bin": {"op": "+", "left": {"int": 0}, "right": expr}}
with open("build/deep-expression.json", "w", encoding="utf-8") as stream:
    json.dump({"capabilities": [], "body": [{"return": expr}]}, stream)
with open("build/deep-expression.pact", "w", encoding="utf-8") as stream:
    stream.write("return " + "(" * 129 + "0" + ")" * 129 + ";\n")
PY
if ./pact check build/deep-expression.json > build/deep-expression.out 2>&1; then
  echo 'expected deeply nested JSON expression to fail' >&2
  exit 1
fi
if ./pact check build/deep-expression.pact > build/deep-expression-text.out 2>&1; then
  echo 'expected deeply nested text expression to fail' >&2
  exit 1
fi

# Diagnostics are a machine-readable JSON contract, including stable codes.
python3 -c '
import json
import sys

def contains_code(value):
    if isinstance(value, dict):
        return any(key == "code" and item == code or contains_code(item)
                   for key, item in value.items())
    if isinstance(value, list):
        return any(contains_code(item) for item in value)
    return False

for path, code in ((sys.argv[1], "PACT-E003"),
                   (sys.argv[2], "PACT-T001"),
                   (sys.argv[3], "PACT-J010"),
                   (sys.argv[4], "PACT-P005"),
                   (sys.argv[5], "PACT-J011")):
    with open(path, encoding="utf-8") as stream:
        diagnostic = json.load(stream)
    if diagnostic.get("diagnostic_version") != 1:
        raise SystemExit("expected diagnostic_version 1 in " + path)
    if not contains_code(diagnostic):
        raise SystemExit("expected JSON diagnostic with code " + code)
' build/denied.out build/unknown-variable.out build/deep-expression.out build/deep-expression-text.out build/version-unsupported.out

printf 'Pact prototype checks passed.\n'
