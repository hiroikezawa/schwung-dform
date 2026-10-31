#!/usr/bin/env bash
# Native build + every test: engine (ASan/UBSan), real dlopen smoke, UI against
# Schwung's real page engine, then the Move build (check_abi.py rejects a libm
# symbol newer than Move's glibc 2.34).
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
bash "$MODULE/scripts/build.sh" native
OUT="$MODULE/build/native"
"${CC:-gcc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -fno-math-errno \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$MODULE/dsp" "$MODULE/tests/test_engine.c" "$MODULE/dsp/bk_engine.c" "$MODULE/dsp/bk_clock.c" "$MODULE/dsp/bk_motion.c" \
  -o "$OUT/test-engine" -lm
"$OUT/test-engine" --wav "$OUT/demo.wav"
python3 "$MODULE/tests/smoke.py" "$OUT/dsp.so"
# The UI test drives Schwung's own page engine and draw harness: it needs a
# Schwung checkout (SCHWUNG_ROOT, else ../schwung beside this repository).
SCHWUNG="${SCHWUNG_ROOT:-$(cd "$MODULE/.." && pwd)/schwung}"
if [[ ! -f "$SCHWUNG/src/shared/param_pages/page_controller.mjs" ]]; then
  echo "SKIP: UI test needs a Schwung checkout (set SCHWUNG_ROOT)" >&2
elif command -v node >/dev/null 2>&1; then
  SCHWUNG_ROOT="$SCHWUNG" node "$MODULE/tests/test_ui.mjs"
elif command -v node.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
  node.exe "$(wslpath -w "$MODULE/tests/test_ui.mjs")"
else
  echo "Node.js is required for the UI test" >&2; exit 1
fi
if command -v "${CROSS_PREFIX:-aarch64-linux-gnu-}gcc" >/dev/null 2>&1; then
  bash "$MODULE/scripts/build.sh" arm64
else
  echo "SKIP: no aarch64 cross compiler; ABI not checked" >&2
fi
