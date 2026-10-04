#!/usr/bin/env bash
set -euo pipefail

COMMIT="89eb80cb440d44c31a762c402074871d56610190"

if [[ ! -d .git ]]; then
    echo "ERROR: ejecuta este script desde la raíz de algoTrading."
    exit 1
fi

echo "============================================================"
echo "LEGACY BUILD WIRING @ $COMMIT"
echo "============================================================"

show_file() {
    local path="$1"
    echo
    echo "===== $path ====="
    if git cat-file -e "${COMMIT}:${path}" 2>/dev/null; then
        git show "${COMMIT}:${path}"
    else
        echo "(no existe en ese commit)"
    fi
}

show_file "meson.build"
show_file "lib/meson.build"
show_file "lib/src/meson.build"

echo
echo "===== MESON FILES UNDER lib/src ====="
git ls-tree -r --name-only "$COMMIT" lib/src \
    | grep '/meson\.build$' \
    | sort

echo
echo "===== SOURCE-GROUP VARIABLES ====="
for f in $(git ls-tree -r --name-only "$COMMIT" lib/src | grep '/meson\.build$' | sort); do
    git show "${COMMIT}:${f}" 2>/dev/null \
      | grep -nE '^[A-Za-z0-9_]+_(sources|inc)[[:space:]]*=' \
      | sed "s#^#${f}:#"
done

echo
echo "============================================================"
echo "NO SE HA MODIFICADO NADA."
echo "Pásame esta salida completa."
echo "============================================================"
