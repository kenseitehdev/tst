#!/usr/bin/env bash
# tests/io/file_write_read.sh
# Tests basic file write + read, append, and cleanup

TMPFILE=$(mktemp /tmp/tst_io_XXXXXX)

fail() {
    echo "FAIL: $1"
    rm -f "$TMPFILE"
    exit 1
}

pass() {
    echo "PASS: $1"
}

# ── 1. write and read back ─────────────────────────────────────────
echo "hello tst" > "$TMPFILE"
content=$(cat "$TMPFILE")
[ "$content" = "hello tst" ] || fail "write/read: expected 'hello tst', got '$content'"
pass "write and read back"

# ── 2. append ─────────────────────────────────────────────────────
echo "second line" >> "$TMPFILE"
lines=$(wc -l < "$TMPFILE")
[ "$lines" -eq 2 ] || fail "append: expected 2 lines, got $lines"
pass "append"

# ── 3. file exists after write ────────────────────────────────────
[ -f "$TMPFILE" ] || fail "file existence: file not found after write"
pass "file exists"

# ── 4. overwrite ──────────────────────────────────────────────────
echo "overwritten" > "$TMPFILE"
content=$(cat "$TMPFILE")
[ "$content" = "overwritten" ] || fail "overwrite: expected 'overwritten', got '$content'"
pass "overwrite"

# ── 5. delete and confirm gone ────────────────────────────────────
rm "$TMPFILE"
[ ! -f "$TMPFILE" ] || fail "delete: file still exists after rm"
pass "delete"

echo "PASS: all file I/O checks passed"
exit 0