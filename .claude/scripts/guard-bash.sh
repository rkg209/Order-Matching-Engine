#!/usr/bin/env bash
# PreToolUse(Bash) hook: block dangerous or policy-violating commands.
#
# Reads the tool-use JSON on stdin, inspects .tool_input.command.
# Exit 2 = BLOCK the command; stderr is fed back to Claude as the reason.
# Exit 0 = allow.

set -uo pipefail

PAYLOAD="$(cat)"
CMD=""
if command -v jq >/dev/null 2>&1; then
  CMD=$(jq -r '.tool_input.command // empty' <<<"$PAYLOAD" 2>/dev/null) || CMD=""
fi

# FAIL CLOSED. If the payload could not be parsed, scan the raw text instead of
# waving the command through. A guard that allows everything the moment its input
# is malformed is not a guard — and malformed input is exactly what an evasion
# would look like. Over-blocking here is cheap; under-blocking is not.
if [[ -z "$CMD" ]]; then
  CMD="$PAYLOAD"
fi

[[ -z "${CMD:-}" ]] && exit 0

block() {
  echo "BLOCKED by .claude/scripts/guard-bash.sh" >&2
  echo "" >&2
  echo "$1" >&2
  exit 2
}

# --- MANDATORY RULE 1: no Co-Authored-By trailer in commits -------------------
# Breaks pushes to GitHub. See CLAUDE.md. This is why the rule is a hook and not
# just an instruction: instructions are forgotten, hooks are not.
if grep -qiE '\bgit[[:space:]]+(-[^[:space:]]+[[:space:]]+)*commit\b' <<<"$CMD"; then
  if grep -qiE 'co-authored-by' <<<"$CMD"; then
    block "This git commit carries a 'Co-Authored-By:' trailer.

MANDATORY RULE 1 in CLAUDE.md forbids it — it causes failures when pushing to GitHub.
Rewrite the commit message with no trailer. The message ends at the body."
  fi
fi

# --- Protect the committed benchmark baselines (DR-7) -------------------------
# Baselines change ONLY via the deliberate /perf-baseline command.
if grep -qE '(rm|mv|truncate|>[[:space:]]*)[^|]*benchmarks/baselines' <<<"$CMD"; then
  block "This command modifies or deletes benchmarks/baselines/.

Per DR-7 and constitution Principle 1, the committed baseline is modified ONLY by the
explicit /perf-baseline command. A baseline is never overwritten as a side effect of a
change that made things slower — that would erase the very regression it should catch."
fi

# --- Destructive filesystem ---------------------------------------------------
if grep -qE '\brm[[:space:]]+(-[a-zA-Z]*[rR][a-zA-Z]*[[:space:]]+)*-{0,2}[a-zA-Z]*[fF]' <<<"$CMD" \
   && grep -qE '\brm\b[^|;]*(-[a-zA-Z]*r[a-zA-Z]*f|-[a-zA-Z]*f[a-zA-Z]*r|-rf|-fr)' <<<"$CMD"; then
  # Allow rm -rf strictly inside build/ and the scratchpad; block everything else.
  if ! grep -qE '\brm[[:space:]]+-[rRfF]+[[:space:]]+[^[:space:]]*(build/|/scratchpad/|\.deps/)' <<<"$CMD"; then
    block "Recursive force-delete outside build/. Refusing.

If you genuinely need this, ask the user to run it themselves."
  fi
fi

# --- Git history rewriting on the shared branch -------------------------------
if grep -qE '\bgit[[:space:]]+push\b[^|;]*(--force\b|-f\b)' <<<"$CMD" \
   && ! grep -qE '--force-with-lease' <<<"$CMD"; then
  block "Force-push. Refusing.

Use --force-with-lease if a rewrite is genuinely intended, and only on a branch you own."
fi

if grep -qE '\bgit[[:space:]]+reset[[:space:]]+--hard\b' <<<"$CMD"; then
  block "'git reset --hard' discards uncommitted work irreversibly. Refusing.

Stash or commit first, or ask the user to run this themselves."
fi

exit 0
