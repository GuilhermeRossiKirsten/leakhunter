#!/usr/bin/env bash
#
# One command to see LeakHunter work on something that looks like real code.
#
#   ./poc/run_demo.sh
#
# Builds LeakHunter and the demo target if needed, runs one under the other, and
# prints where the reports landed.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
build="$root/build"
reports="${1:-$root/build/poc-report}"

if [[ ! -x "$build/bin/leakhunter" || ! -x "$build/poc/docindex" ]]; then
    echo "==> building LeakHunter and the demo"
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
    cmake --build "$build" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
fi

echo "==> docindex without LeakHunter: exits 0, looks healthy"
echo
"$build/poc/docindex"

echo
echo "==> the same binary under LeakHunter"
echo

# Exit code 1 means findings, which is the whole point, so do not let -e stop us.
set +e
"$build/bin/leakhunter" --output "$reports" "$build/poc/docindex"
status=$?
set -e

echo
echo "==> leakhunter exited $status (0 = clean, 1 = findings)"
echo "    HTML: $reports/report.html"
echo "    JSON: $reports/report.json"
echo
echo "    Try also:"
echo "      $build/bin/leakhunter --suppressions $here/docindex.supp $build/poc/docindex"
echo "      $build/bin/leakhunter --include-runtime $build/poc/docindex"
echo "      jq '.groups[] | {function, count, totalBytes, location}' $reports/report.json"
