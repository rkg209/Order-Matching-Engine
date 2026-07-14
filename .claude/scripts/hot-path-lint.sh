#!/usr/bin/env bash
# Scan a hot-path source file for constructs forbidden by the constitution.
#
# The hot path is engine/ and book/. Off-hot-path files are skipped entirely.
# Exits 0 always (advisory), but prints violations to stderr so the PostToolUse
# hook surfaces them to Claude as feedback.
#
# Carve-out (constitution Principle 3): steady_clock IS permitted on the hot path
# for latency capture. Only system_clock / wall-clock reads are violations.

set -uo pipefail

FILE="${1:-}"
[[ -z "$FILE" || ! -f "$FILE" ]] && exit 0

# Only lint the hot path.
case "$FILE" in
  */engine/*|engine/*|*/book/*|book/*) ;;
  *) exit 0 ;;
esac

# Strip comments and string literals before matching, so prose about `new` in a
# comment does not trip the linter.
STRIPPED=$(sed -e 's://.*::' -e 's:"[^"]*"::g' "$FILE")

violation=0
check() {
  local pattern="$1" message="$2"
  local hits
  hits=$(grep -nE "$pattern" <<<"$STRIPPED" || true)
  if [[ -n "$hits" ]]; then
    violation=1
    while IFS= read -r line; do
      printf '  %s:%s  %s\n' "$FILE" "${line%%:*}" "$message" >&2
    done <<<"$hits"
  fi
}

check '\bnew\b[[:space:]]+[A-Za-z_]' 'HOT-PATH: heap allocation via `new` (use ObjectPool)'
check '\b(malloc|calloc|realloc|strdup)[[:space:]]*\(' 'HOT-PATH: C heap allocation'
check '\bthrow\b'                     'HOT-PATH: exceptions are forbidden'
check '\bvirtual\b'                   'HOT-PATH: virtual dispatch is forbidden'
check '\b(std::)?(mutex|lock_guard|unique_lock|shared_mutex|condition_variable)\b' \
                                      'HOT-PATH: locking primitive (single-writer principle)'
check '\b(std::)?(cout|cerr|clog|printf|fprintf|puts)\b' \
                                      'HOT-PATH: logging / iostream is forbidden'
check '\bspdlog::'                    'HOT-PATH: logging is forbidden'
check '\.push_back[[:space:]]*\('     'HOT-PATH: dynamic container growth (pre-size, or use a pool)'
check '\bstd::(vector|unordered_map|unordered_set|map|set|string|deque|list)\b' \
                                      'HOT-PATH: dynamic STL container (use pre-sized/open-addressing)'
check '\bsystem_clock\b'              'HOT-PATH: wall-clock read breaks determinism (steady_clock only)'
check '\b(std::rand|rand|srand|mt19937|random_device)\b' \
                                      'HOT-PATH: randomness breaks determinism'

if [[ $violation -eq 1 ]]; then
  echo "" >&2
  echo "hot-path-lint: violations found in $FILE (see .specify/memory/constitution.md, Principle 4)." >&2
  echo "If one is a deliberate, justified exception, say so in the code and in progress_report.md." >&2
fi

exit 0
