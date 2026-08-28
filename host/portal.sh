#!/bin/sh
# Open the captive-portal page in a browser, no device involved.
#
#   ./portal.sh              the populated state: three fake networks, no key stored
#   ./portal.sh --key-set    the same, but with a key already in NVS
#   ./portal.sh --raw        the file untouched, so /scan really fails
#
# main/portal.src.html can be opened directly -- it is a plain file, that being the
# point of embedding it rather than keeping it a C string. What you get that way
# is the scan-FAILED state, because there is no device answering /scan: a real
# state worth checking, and what --raw gives you.
#
# The other two modes prepend a fetch() shim so the populated state is visible.
# The shim is written into build/, never into main/, so the embedded page cannot
# pick it up.
set -e
cd "$(dirname "$0")"

KEY_SET=false
RAW=""
for arg in "$@"; do
    case "$arg" in
        --key-set) KEY_SET=true ;;
        --raw)     RAW=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

mkdir -p build
OUT=build/portal-preview.html

if [ -n "$RAW" ]; then
    cp ../main/portal.src.html "$OUT"
else
    # Inserted before the page's own <script>, so its fetch() calls see the shim.
    cat > build/portal_shim.html <<EOF
<script>
/* PREVIEW ONLY -- host/portal.sh writes this, the firmware never sees it. */
window.fetch = function (u) {
  if (String(u).indexOf('/scan') === 0) {
    return Promise.resolve({ json: function () {
      return Promise.resolve({ key_set: $KEY_SET, nets: [
        { ssid: 'kitchen-2.4', rssi: -42, open: false },
        { ssid: 'a-very-long-network-name-to-check-wrapping', rssi: -58, open: false },
        { ssid: 'guest', rssi: -71, open: true } ] });
    } });
  }
  /* /save: resolve so the button settles, but nothing is saved and nothing reboots. */
  return Promise.resolve({ ok: true, text: function () { return Promise.resolve('saved'); } });
};
</script>
EOF
    awk 'BEGIN{done=0}
         /^<script>$/ && !done { while ((getline line < "build/portal_shim.html") > 0) print line; done=1 }
         {print}' ../main/portal.src.html > "$OUT"
fi

echo "wrote $OUT"
case "$(uname)" in
    Darwin) exec open "$OUT" ;;
    *)      echo "open it in a browser" ;;
esac
