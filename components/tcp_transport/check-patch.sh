#!/bin/sh
# Verify this component is still ESP-IDF's tcp_transport plus exactly the local
# patches we think it is.
#
#   ./check-patch.sh            fail if either side has moved
#   ./check-patch.sh --update   accept the current pair as the new baseline
#
# WHY THIS EXISTS. transport_ws.c is a copy of one upstream file; its siblings
# and both include directories come out of $IDF_PATH at build time. The #error
# in transport_ws.c catches a MAJOR.MINOR bump, but it cannot see upstream
# editing that file in place within 5.5.x -- this can. Nothing here touches the
# device or the firmware build.
#
# HOW IT CHECKS. The authority is baseline.sha256: the digest of the upstream
# file this fork was taken from, and the digest of our copy. Content, not diff
# text -- because Apple/FreeBSD diff and GNU diffutils emit different (both
# correct) unified diffs for the same pair of files, so a committed .patch is
# not portable enough to be a baseline and made this check fail in CI while
# passing on a maintainer's Mac. local.patch is still the human-readable record
# of WHAT was changed; the digests are what decides pass or fail.
set -e
cd "$(dirname "$0")"

if [ -z "$IDF_PATH" ]; then
    echo "IDF_PATH is not set. Run your ESP-IDF export.sh first." >&2
    exit 2
fi

UPSTREAM="$IDF_PATH/components/tcp_transport/transport_ws.c"
if [ ! -f "$UPSTREAM" ]; then
    echo "No upstream file at $UPSTREAM" >&2
    exit 2
fi

# sha256sum on Linux, shasum on macOS. Print the bare digest either way.
sha() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# Explicit labels: the default diff header carries mtimes, which would make the
# record differ on every checkout for no reason.
gen() {
    diff -u --label upstream/transport_ws.c --label local/transport_ws.c \
        "$UPSTREAM" transport_ws.c || true
}

UP_NOW=$(sha "$UPSTREAM")
LOCAL_NOW=$(sha transport_ws.c)

if [ "$1" = "--update" ]; then
    gen > local.patch
    # No $IDF_PATH in here: it is a machine-specific absolute path and this is
    # a public repo. The version is already recorded by the #error guard.
    cat > baseline.sha256 <<BASELINE
# Digests this fork is pinned to. Regenerate with ./check-patch.sh --update.
#   upstream  ESP-IDF 5.5.5's components/tcp_transport/transport_ws.c
#   local     components/tcp_transport/transport_ws.c, upstream + local.patch
upstream $UP_NOW
local    $LOCAL_NOW
BASELINE
    echo "baseline.sha256 and local.patch regenerated against $(basename "$IDF_PATH")"
    echo "Review the diff before committing it -- these files ARE the record of"
    echo "what we changed, so an accidental update hides a real drift."
    exit 0
fi

if [ ! -f baseline.sha256 ]; then
    echo "baseline.sha256 is missing. Regenerate it with: $0 --update" >&2
    exit 2
fi

UP_WANT=$(awk '$1 == "upstream" { print $2 }' baseline.sha256)
LOCAL_WANT=$(awk '$1 == "local" { print $2 }' baseline.sha256)

if [ -z "$UP_WANT" ] || [ -z "$LOCAL_WANT" ]; then
    echo "baseline.sha256 is malformed -- expected 'upstream <sha>' and 'local <sha>'." >&2
    exit 2
fi

if [ "$UP_NOW" = "$UP_WANT" ] && [ "$LOCAL_NOW" = "$LOCAL_WANT" ]; then
    echo "ok  transport_ws.c is upstream + $(grep -c '^@@' local.patch) local hunks, unchanged"
    exit 0
fi

echo "DRIFT detected." >&2
echo >&2
if [ "$UP_NOW" != "$UP_WANT" ]; then
    echo "UPSTREAM moved: $IDF_PATH's transport_ws.c is not the file this fork" >&2
    echo "was taken from." >&2
    echo "  baseline $UP_WANT" >&2
    echo "  now      $UP_NOW" >&2
    echo >&2
    echo "Re-apply the LOCAL PATCH hunks to the new upstream file, check the" >&2
    echo "#error version guard at the top of transport_ws.c, then: $0 --update" >&2
fi
if [ "$LOCAL_NOW" != "$LOCAL_WANT" ]; then
    echo "LOCAL copy moved: components/tcp_transport/transport_ws.c changed." >&2
    echo "  baseline $LOCAL_WANT" >&2
    echo "  now      $LOCAL_NOW" >&2
    echo >&2
    echo "If the change was intended, run: $0 --update" >&2
fi
echo >&2
echo "The current diff against $IDF_PATH, for reading:" >&2
echo >&2
gen >&2
exit 1
