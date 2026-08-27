#!/bin/sh
# Compare the assembled system prompt in the working tree against a git ref.
#
#   promptdiff.sh              vs HEAD
#   promptdiff.sh main         vs another ref
#
# Renders both sides with host/prompt.sh, which compiles main/agent_prompt.c
# itself, so what is compared is what the device would assemble. No device and no
# firmware build involved.
#
# Exit 0 identical, 1 changed, 2 could not run. Usable as a gate.
#
# The point is that the BASELINE COMES FROM GIT, not from having remembered to
# capture one before editing. That is the step people skip.
set -e

REF="${1:-HEAD}"
REPO=$(git rev-parse --show-toplevel)
cd "$REPO"

if ! git rev-parse --verify --quiet "$REF^{commit}" >/dev/null; then
    echo "promptdiff: no such ref: $REF" >&2
    exit 2
fi
if [ ! -x host/prompt.sh ]; then
    echo "promptdiff: host/prompt.sh missing or not executable" >&2
    exit 2
fi

WT=$(mktemp -d "${TMPDIR:-/tmp}/promptdiff.XXXXXX")
OUT=$(mktemp -d "${TMPDIR:-/tmp}/promptout.XXXXXX")
cleanup() {
    git worktree remove --force "$WT/tree" >/dev/null 2>&1 || true
    rm -rf "$WT" "$OUT"
}
trap cleanup EXIT INT TERM

git worktree add --detach --quiet "$WT/tree" "$REF"

if [ ! -x "$WT/tree/host/prompt.sh" ]; then
    echo "promptdiff: $REF has no host/prompt.sh -- nothing to compare against" >&2
    exit 2
fi

# stdout only. prompt.sh logs the byte/block summary to STDERR, and letting that
# into one side and not the other manufactures a difference that is not real.
render() {  # render <dir> <label> <extra-args...>
    dir=$1; label=$2; shift 2
    if ! ( cd "$dir" && ./host/prompt.sh "$@" 2>"$OUT/$label.err" >"$OUT/$label.txt" ); then
        echo "promptdiff: rendering $label failed:" >&2
        sed 's/^/    /' "$OUT/$label.err" >&2
        exit 2
    fi
}

render "$WT/tree" before
render "$REPO"    after
render "$WT/tree" before-resumed --resumed
render "$REPO"    after-resumed  --resumed

summary() { sed -n 's/.*system prompt: \(.*\)/\1/p' "$OUT/$1.err" | head -1; }

echo "ref $REF   $(summary before)"
echo "worktree   $(summary after)"
echo

rc=0
for pair in "before:after:default" "before-resumed:after-resumed:--resumed"; do
    a=${pair%%:*}; rest=${pair#*:}; b=${rest%%:*}; what=${rest##*:}
    if diff -u "$OUT/$a.txt" "$OUT/$b.txt" > "$OUT/$what.diff"; then
        echo "$what: identical"
    else
        echo "$what: CHANGED"
        sed 's/^/    /' "$OUT/$what.diff"
        rc=1
    fi
done

exit $rc
