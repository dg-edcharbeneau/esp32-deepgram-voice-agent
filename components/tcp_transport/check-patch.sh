#!/bin/sh
# Verify this component is still ESP-IDF's tcp_transport plus exactly the local
# patches we think it is.
#
#   ./check-patch.sh            fail if the diff against $IDF_PATH has moved
#   ./check-patch.sh --update   accept the current diff as the new baseline
#
# WHY THIS EXISTS. transport_ws.c is a copy of one upstream file; its siblings
# and both include directories come out of $IDF_PATH at build time. The #error
# in transport_ws.c catches a MAJOR.MINOR bump, but it cannot see upstream
# editing that file in place within 5.5.x -- this can. Nothing here touches the
# device or the firmware build.
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

# Explicit labels: the default diff header carries mtimes, which would make the
# baseline differ on every checkout for no reason.
gen() {
    diff -u --label upstream/transport_ws.c --label local/transport_ws.c \
        "$UPSTREAM" transport_ws.c || true
}

if [ "$1" = "--update" ]; then
    gen > local.patch
    echo "local.patch regenerated against $(basename "$IDF_PATH")"
    echo "Review the diff before committing it -- this file IS the record of"
    echo "what we changed, so an accidental update hides a real drift."
    exit 0
fi

if [ ! -f local.patch ]; then
    echo "local.patch is missing. Regenerate it with: $0 --update" >&2
    exit 2
fi

if gen | diff -q - local.patch >/dev/null 2>&1; then
    echo "ok  transport_ws.c is upstream + $(grep -c '^@@' local.patch) local hunks, unchanged"
    exit 0
fi

echo "DRIFT: the diff against $IDF_PATH no longer matches local.patch." >&2
echo >&2
echo "Either upstream changed the file, or the local copy did. What changed:" >&2
echo >&2
gen | diff -u local.patch - >&2 || true
echo >&2
echo "If upstream moved, re-apply the LOCAL PATCH hunks to the new file." >&2
echo "If the local change was intended, run: $0 --update" >&2
exit 1
