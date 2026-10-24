#!/bin/sh
# Build docs/orb.html: the orb face running live in a browser, on the firmware's
# own geometry compiled to WebAssembly. It is a page of the project site, which
# GitHub Pages serves from docs/ on main.
#
#   ./orb_web.sh          then open ../docs/orb.html
#
# Needs emcc (Emscripten). One self-contained file, wasm inlined as base64, so it
# opens from disk too. The output is committed, so rebuild and commit it after
# touching orb_geometry.c or the template; nothing here touches the device.
set -e
cd "$(dirname "$0")"

mkdir -p build
emcc -O2 -std=gnu11 -I../main orb_wasm.c ../main/orb_geometry.c -o build/orb_module.js \
  -sMODULARIZE=1 -sEXPORT_NAME=OrbModule -sSINGLE_FILE=1 -sENVIRONMENT=web -sFILESYSTEM=0 -sSINGLE_FILE_BINARY_ENCODE=0 \
  -sEXPORTED_FUNCTIONS=_orbw_init,_orbw_shell,_orbw_mode,_orbw_dots,_orbw_count,_orbw_lines,_orbw_line_count \
  -sEXPORTED_RUNTIME_METHODS=HEAPF32

python3 - <<'PY'
page = open("orb_web.html").read()
module = open("build/orb_module.js").read()
# A raw NUL breaks the inline script; SINGLE_FILE_BINARY_ENCODE=0 keeps it base64.
assert "/*@ORB_MODULE@*/" in page and "</script" not in module and "\0" not in module
open("../docs/orb.html", "w").write(page.replace("/*@ORB_MODULE@*/", module))
PY
echo "wrote docs/orb.html"
