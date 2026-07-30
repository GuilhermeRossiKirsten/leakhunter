#!/usr/bin/env bash
#
# Build LeakHunter, build all four demonstrations, run each under the tool, and
# leave one timestamped report per binary.
#
#   ./scripts/run_all_pocs.sh [output-dir]
#
# Default output: build/all-poc-reports/
#
# The four are deliberately unalike, and together they cover most of what the
# tool has to handle:
#
#   docindex     four planted defects in a realistic multi-file program
#   service      never exits on its own -- the script stops it, as you would
#   pipeline23   C++23, std::expected error path
#   pipeline98   the same program in C++98
#
# Each poc is configured and built **standalone**, the way you would build any
# other project. That is part of the point: nothing here links against
# LeakHunter or includes a header of ours.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
build="$root/build"
reports="${1:-$build/all-poc-reports}"
jobs="$(nproc 2>/dev/null || echo 4)"

leakhunter="$build/bin/leakhunter"

# poc2 never exits, so it is stopped after this many seconds.
service_seconds="${LEAKHUNTER_SERVICE_SECONDS:-4}"

banner() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
note()   { printf '   %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 1. LeakHunter itself
# ---------------------------------------------------------------------------
banner "building LeakHunter"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
cmake --build "$build" -j"$jobs" > /dev/null
note "$("$leakhunter" --version)"

# ---------------------------------------------------------------------------
# 2. The four demonstrations, each configured and built on its own
# ---------------------------------------------------------------------------
declare -a built_names=()
declare -a built_paths=()

build_poc() {
    local dir="$1" binary="$2"
    banner "building $dir ($binary)"

    if [[ ! -d "$root/$dir" ]]; then
        note "no $dir directory; skipping"
        return
    fi

    if ! cmake -S "$root/$dir" -B "$root/$dir/build" \
         -DCMAKE_BUILD_TYPE=RelWithDebInfo > "$root/$dir/build-configure.log" 2>&1; then
        note "configure failed -- see $dir/build-configure.log"
        return
    fi
    if ! cmake --build "$root/$dir/build" -j"$jobs" > "$root/$dir/build-compile.log" 2>&1; then
        note "build failed -- see $dir/build-compile.log"
        return
    fi

    local path="$root/$dir/build/$binary"
    if [[ ! -x "$path" ]]; then
        # poc3 skips itself when the toolchain has no usable <expected>; that is
        # a documented outcome, not a failure of this script.
        note "$binary was not produced (see $dir/README.md); skipping"
        grep -i "skipped" "$root/$dir/build-configure.log" | sed 's/^/   /' || true
        return
    fi

    note "built $dir/build/$binary"
    built_names+=("$binary")
    built_paths+=("$path")
}

build_poc poc  docindex
build_poc poc2 service
build_poc poc3 pipeline23
build_poc poc4 pipeline98

if [[ ${#built_paths[@]} -eq 0 ]]; then
    echo "nothing was built; stopping" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 3. Run each under LeakHunter
# ---------------------------------------------------------------------------
mkdir -p "$reports"
declare -a exit_codes=()

run_one() {
    local name="$1" path="$2"
    banner "running $name under LeakHunter"

    local status=0
    if [[ "$name" == "service" ]]; then
        # Never exits on its own. Stopping it is the demonstration: LeakHunter
        # forwards the signal, the target flushes, and the report is written.
        note "this one never exits; stopping it after ${service_seconds}s (Ctrl-C by hand)"
        "$leakhunter" --output "$reports" "$path" &
        local watched=$!
        sleep "$service_seconds"
        kill -INT "$watched" 2>/dev/null
        wait "$watched"
        status=$?
    else
        "$leakhunter" --output "$reports" "$path"
        status=$?
    fi

    exit_codes+=("$status")
    note "exit $status"
}

for index in "${!built_paths[@]}"; do
    run_one "${built_names[$index]}" "${built_paths[$index]}"
done

# ---------------------------------------------------------------------------
# 4. What was produced
# ---------------------------------------------------------------------------
banner "reports in $reports"
printf '\n'
ls -1 "$reports" | sed 's/^/   /'

printf '\n'
if command -v python3 > /dev/null 2>&1; then
    python3 - "$reports" <<'PY'
import glob, json, os, sys

directory = sys.argv[1]
rows = []
for path in sorted(glob.glob(os.path.join(directory, "*.json"))):
    try:
        data = json.load(open(path))
    except Exception as error:                       # a half-written file, say
        rows.append((os.path.basename(path), "unreadable: %s" % error, "", "", ""))
        continue
    summary = data["summary"]
    rows.append((
        os.path.basename(path),
        os.path.basename(data["run"]["command"].split()[0]),
        str(summary["leakCount"]),
        str(summary["leakedBytes"]),
        str(summary["mismatchedFreeCount"]),
    ))

width = max((len(r[0]) for r in rows), default=10)
print("   %-*s  %-12s %7s %10s %11s" % (width, "report", "target", "leaks", "bytes", "mismatches"))
print("   %s" % ("-" * (width + 45)))
for name, target, leaks, byte_count, mismatches in rows:
    print("   %-*s  %-12s %7s %10s %11s" % (width, name, target, leaks, byte_count, mismatches))
PY
fi

printf '\n'
echo "   Every file is kept. Run this again and you get a second set beside the"
echo "   first -- the names carry the target and the local timestamp, so nothing"
echo "   is overwritten."
printf '\n'
echo "   Open any of them:  xdg-open $reports/<name>.html"
printf '\n'

# Exit non-zero if any run found something, so this is usable as a gate.
for status in "${exit_codes[@]}"; do
    if [[ "$status" -ne 0 ]]; then
        exit 1
    fi
done
exit 0
