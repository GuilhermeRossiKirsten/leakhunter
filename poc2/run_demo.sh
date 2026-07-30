#!/usr/bin/env bash
#
# Watch a running service leak, then stop it and get the report.
#
#   ./poc2/run_demo.sh [seconds]      (default: 4)
#
# Stops the target for you after a few seconds, so the demo is one command. By
# hand it is the same thing with Ctrl-C:
#
#   leakhunter ./build/poc2/service

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
build="$root/build"
seconds="${1:-4}"
reports="$build/poc2-report"

if [[ ! -x "$build/bin/leakhunter" || ! -x "$build/poc2/service" ]]; then
    echo "==> building LeakHunter and the service"
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
    cmake --build "$build" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
fi

echo "==> starting the service under LeakHunter; stopping it in ${seconds}s"
echo "    (by hand this is just Ctrl-C)"
echo

"$build/bin/leakhunter" --output "$reports" "$build/poc2/service" &
watched=$!

sleep "$seconds"
kill -INT "$watched" 2>/dev/null || true

set +e
wait "$watched"
status=$?
set -e

echo
echo "==> leakhunter exited $status (0 = clean, 1 = findings)"
echo "    HTML: $reports/report.html"
echo "    JSON: $reports/report.json"
echo
echo "    The target never exited on its own. Everything above is what it had"
echo "    done up to the moment it was stopped."
