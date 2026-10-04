#!/usr/bin/env bash
set -euo pipefail

# Ejecutar desde la raíz de algoTrading.
#
# Genera:
#   handoff_code_YYYYMMDD_HHMMSS/
#       research.tar.gz
#       lib.tar.gz
#       validation.tar.gz
#       ...
#       root_files.tar.gz
#       SHA256SUMS.txt
#
# No modifica ni mueve ninguna carpeta original.

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="handoff_code_${STAMP}"

mkdir -p "$OUT"

# Carpetas que quiero revisar por separado.
DIRS=(
    "research"
    "lib"
    "validation"
    "config"
    "deploy"
    "live_trading"
    "dashboard"
    "scripts"
    "tools"
    "docs"
)

# Basura/generated files que no aportan código fuente.
EXCLUDES=(
    "--exclude=.git"
    "--exclude=.venv"
    "--exclude=build"
    "--exclude=storage"
    "--exclude=__pycache__"
    "--exclude=*.pyc"
    "--exclude=*.pyo"
    "--exclude=.pytest_cache"
    "--exclude=.mypy_cache"
    "--exclude=.ruff_cache"
    "--exclude=.cache"
    "--exclude=node_modules"
    "--exclude=.DS_Store"
)

echo "Creating handoff packages in: $OUT"
echo

for dir in "${DIRS[@]}"; do
    if [[ -d "$dir" ]]; then
        echo "Packing $dir ..."
        tar \
            "${EXCLUDES[@]}" \
            -czf "${OUT}/${dir}.tar.gz" \
            "$dir"
    else
        echo "Skipping missing directory: $dir"
    fi
done

echo
echo "Packing important root files ..."

ROOT_FILES=()

for f in \
    meson.build \
    meson_options \
    README.md \
    CURRENT_STATE.md \
    TO-DO.txt \
    .gitignore \
    .dockerignore
do
    if [[ -f "$f" ]]; then
        ROOT_FILES+=("$f")
    fi
done

if (( ${#ROOT_FILES[@]} > 0 )); then
    tar -czf "${OUT}/root_files.tar.gz" "${ROOT_FILES[@]}"
fi

echo
echo "Generating checksums ..."

(
    cd "$OUT"
    sha256sum ./*.tar.gz > SHA256SUMS.txt
)

echo
echo "Done."
echo
du -h "$OUT"/*
echo
echo "Output directory:"
echo "  $OUT"
