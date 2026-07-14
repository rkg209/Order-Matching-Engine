---
description: Append an entry to progress_report.md for the work just completed.
allowed-tools: Read, Edit, Bash
---

Append a new entry to `progress_report.md` covering **$ARGUMENTS** (default: the work done in this
session so far).

Last entry number used:
!`grep -oE '^## \[[0-9]{3}\]' progress_report.md 2>/dev/null | tail -1 || echo "(none — start at 001)"`

Uncommitted changes to describe:
!`git status --short 2>/dev/null | head -30`

## Rules (MANDATORY RULE 2 in CLAUDE.md)

**Append only.** Never edit or delete an existing entry. If a past entry turns out to be wrong, write
a *new* entry that corrects it and says explicitly which one it corrects. The file is the honest
history of the project, including the parts we got wrong — a history with the mistakes edited out is
worth nothing to a reader and nothing in an interview.

Use the next sequential number. Never reuse one.

## Format (strict)

```markdown
## [NNN] YYYY-MM-DD — Short imperative title

**What:** What changed, concretely. Name the files and the behavior. A reader who was not here
should be able to tell exactly what is different now.

**Why:** The problem this solved or the requirement it satisfies. Cite the spec / FR / NFR id.
"Because the spec said so" is not a why — say what would have been *wrong* without it.

**How:** The approach, and what was considered and rejected. This is where the engineering reasoning
lives, and it is the most valuable part of the entry. A decision recorded without its alternatives
cannot be re-evaluated later.

**Issues:** What broke, and how it was resolved. Omit the subsection ONLY if genuinely nothing did.
```

## Be honest

- If a benchmark regressed, the entry says the numbers and by how much.
- If a test is failing or a piece is stubbed, the entry says so plainly. Do not describe intent as if
  it were outcome.
- If we took a shortcut, name it and name what it will cost later.

The point of this file is that at the end, one document tells the whole story of how this engine was
built — including every wrong turn and how we got out of it. Entries that only record successes make
the file a marketing document, which makes it useless.
