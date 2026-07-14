#!/usr/bin/env bash
# PostToolUse(Edit|Write) hook: format the written file, then hot-path-lint it.
#
# Reads the tool-use JSON on stdin and extracts .tool_input.file_path.
# Never fails the edit — a missing formatter degrades to a no-op.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# --- extract the edited file path from the hook payload -----------------------
PAYLOAD="$(cat)"
if command -v jq >/dev/null 2>&1; then
  FILE=$(jq -r '.tool_input.file_path // empty' <<<"$PAYLOAD")
else
  FILE=$(sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' <<<"$PAYLOAD" | head -1)
fi

[[ -z "${FILE:-}" || ! -f "$FILE" ]] && exit 0

# Only act on C/C++ sources.
case "$FILE" in
  *.cpp|*.cc|*.cxx|*.hpp|*.h|*.hxx) ;;
  *) exit 0 ;;
esac

# --- clang-format ------------------------------------------------------------
# Homebrew LLVM is keg-only on macOS, so clang-format is NOT on PATH by default.
CLANG_FORMAT=""
for candidate in \
  /opt/homebrew/opt/llvm/bin/clang-format \
  /usr/local/opt/llvm/bin/clang-format \
  "$(command -v clang-format 2>/dev/null || true)"
do
  if [[ -n "$candidate" && -x "$candidate" ]]; then CLANG_FORMAT="$candidate"; break; fi
done

if [[ -n "$CLANG_FORMAT" ]]; then
  "$CLANG_FORMAT" -i --style=file "$FILE" 2>/dev/null || true
fi

# --- hot-path lint -----------------------------------------------------------
"$REPO_ROOT/.claude/scripts/hot-path-lint.sh" "$FILE"

exit 0
