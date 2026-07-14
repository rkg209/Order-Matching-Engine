---
description: Kill the engine mid-stream, restart, replay the journal, assert byte-identical state.
allowed-tools: Bash, Read
---

Prove the recovery story (FR-50, NFR-24). This is the test behind the resume line *"killed
mid-stream and restarted to byte-identical state."*

Steps:

1. Start the engine in `live` mode with a clean journal directory.
2. Drive a known command stream through it. Let it write journal segments and at least one snapshot.
3. **Kill it hard** — `SIGKILL`, not a graceful shutdown. A recovery mechanism that only works on
   clean shutdown is not a recovery mechanism; it is a save button.
4. Capture the pre-kill engine state (book contents, trade log, global sequence number).
5. Restart in recovery: load the latest valid snapshot, replay the journal tail from there.
6. Assert the recovered state is **byte-identical** to the pre-kill state at the same global sequence
   number. Not "equivalent" — identical.
7. Assert replay reproduces **all the same trades**, in the same order, with the same trade ids.

Also verify the hard cases, because these are where real recovery bugs live:

- Kill **during** a journal `fsync` — the last record may be **torn** (partially written). Recovery
  must detect the truncated tail record and discard it cleanly, not crash and not read garbage.
- Kill **during** a snapshot write — a half-written snapshot must never be loaded. This is what the
  atomic-rename + CRC32 protocol exists for.
- Kill with an **empty** journal (nothing to recover).

Recovery must require **no manual intervention** (NFR-24). If a human has to run a repair tool, it
failed.
