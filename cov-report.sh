#!/usr/bin/env bash
#
# cov-report.sh - build a coverage report from data in obj/cov/
#
# Does NOT build or run anything. Expects a prior `make cov` (or at least a
# coverage-instrumented run) to have left .gcno/.gcda files in obj/cov/.
#
# Outputs:
#   obj/cov/report/*.gcov      - per-file annotated gcov reports
#   obj/cov/html/index.html    - HTML report (only if gcovr is installed)
# and prints a per-.cpp-file coverage table to the terminal.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
COV_DIR="$ROOT/obj/cov"
REPORT_DIR="$COV_DIR/report"
HTML_DIR="$COV_DIR/html"

if [ ! -d "$COV_DIR" ]; then
    echo "error: $COV_DIR not found - run 'make cov' first" >&2
    exit 1
fi

shopt -s nullglob
gcda_files=("$COV_DIR"/*.gcda)
shopt -u nullglob

if [ ${#gcda_files[@]} -eq 0 ]; then
    echo "error: no .gcda files in $COV_DIR - run the instrumented binaries (make test) first" >&2
    exit 1
fi

rm -rf "$REPORT_DIR" "$HTML_DIR"
mkdir -p "$REPORT_DIR"

# Pick a gcov that matches the g++ that produced the .gcno files.
# (e.g. on AL2023, /usr/bin/gcov is gcc 11 but g++ may be gcc14 -> gcc14-gcov)
if [ -z "${GCOV:-}" ]; then
    gxx_major="$(g++ -dumpversion | cut -d. -f1)"
    for cand in "gcov-$gxx_major" "gcc$gxx_major-gcov" gcov; do
        if command -v "$cand" > /dev/null 2>&1 &&
           [ "$("$cand" --version | sed -n 's/^gcov (GCC) \([0-9]*\).*/\1/p')" = "$gxx_major" ]; then
            GCOV="$cand"
            break
        fi
    done
    if [ -z "${GCOV:-}" ]; then
        echo "error: no gcov matching g++ $gxx_major found (set GCOV=... to override)" >&2
        exit 1
    fi
fi

# Run gcov from the repo root so relative source paths in the .gcno resolve,
# then move the annotated .gcov files into the report dir.
gcov_log="$REPORT_DIR/gcov.log"
(
    cd "$ROOT"
    "$GCOV" --object-directory "$COV_DIR" "${gcda_files[@]}" > "$gcov_log" 2>&1
    mv -- *.gcov "$REPORT_DIR"/
) || {
    echo "error: $GCOV failed, see $gcov_log" >&2
    exit 1
}

# Optional HTML report if gcovr is available.
if command -v gcovr > /dev/null 2>&1; then
    mkdir -p "$HTML_DIR"
    # Sources may be recorded under either the physical path or a symlinked
    # form (e.g. /home/user -> /local/home/user): accept both.
    ROOT_PHYS="$(readlink -f "$ROOT")"
    HOME_PHYS="$(readlink -f "$HOME")"
    ROOT_HOME="${ROOT_PHYS/#$HOME_PHYS/$HOME}"
    gcovr --root "$ROOT" \
          --filter "$ROOT/" --filter "$ROOT_PHYS/" --filter "$ROOT_HOME/" \
          --gcov-executable "$GCOV" \
          --html-details "$HTML_DIR/index.html" \
          --html-title "lua coverage" \
          "$COV_DIR" > /dev/null
    html_note="html report: $HTML_DIR/index.html"
else
    html_note="(install gcovr for an html report)"
fi

# Parse gcov output into a table. gcov prints pairs like:
#   File 'lapi.cpp'
#   Lines executed:91.23% of 800
# Only .cpp files in the project root are shown.
awk -v root="$ROOT" '
/^File / {
    # line looks like: File 'lapi.cpp'  -> strip "File '" prefix and "'" suffix
    file = substr($0, 7, length($0) - 7)
    next
}
/^Lines executed:/ {
    if (file == "") next
    pct = $0;   sub(/^Lines executed:/, "", pct); sub(/% of.*/, "", pct)
    total = $0; sub(/.*% of /, "", total)
    # keep only project .cpp files (gcov also reports headers/system files)
    name = file
    sub("^" root "/", "", name)
    if (name ~ /^[^\/]+\.cpp$/) {
        # a file can appear once per gcda; keep the entry with most lines
        if (!(name in lines) || total + 0 > lines[name] + 0) {
            lines[name] = total
            percent[name] = pct
        }
    }
    file = ""
}
END {
    printf "\n%-28s %10s %10s\n", "file", "lines", "coverage"
    printf "%-28s %10s %10s\n", "----------------------------", "----------", "----------"
    # collect keys and sort by coverage % descending (name ascending on ties)
    n = 0
    for (f in lines) sorted[++n] = f
    for (i = 1; i < n; i++) {
        for (j = i + 1; j <= n; j++) {
            a = sorted[i]; b = sorted[j]
            if (percent[b] + 0 > percent[a] + 0 ||
                (percent[b] + 0 == percent[a] + 0 && b < a)) {
                sorted[i] = b; sorted[j] = a
            }
        }
    }
    tot_lines = 0; tot_exec = 0
    for (i = 1; i <= n; i++) {
        f = sorted[i]
        printf "%-28s %10d %9.2f%%\n", f, lines[f], percent[f]
        tot_lines += lines[f]
        tot_exec  += lines[f] * percent[f] / 100.0
    }
    printf "%-28s %10s %10s\n", "----------------------------", "----------", "----------"
    if (tot_lines > 0)
        printf "%-28s %10d %9.2f%%\n", "TOTAL", tot_lines, 100.0 * tot_exec / tot_lines
}
' "$gcov_log"

echo ""
echo "annotated sources: $REPORT_DIR/"
echo "$html_note"
