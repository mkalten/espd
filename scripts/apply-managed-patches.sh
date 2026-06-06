#!/usr/bin/env bash
# Apply patches to managed_components after idf.py update-dependencies.
#
# Usage:
#   ./scripts/apply-managed-patches.sh [patch1.patch ...]
#   (no arguments → applies all patches listed in 'patches' array below)
#
# Patches are standard unified-diff patches with paths relative to the
# project root (e.g. managed_components/espressif__foo/bar.c).
# Unlike pd patches, 'patch' (not 'git apply') is used because managed
# components are not a git repo.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.."; pwd)"
PATCH_DIR="${ROOT_DIR}/patches"

# List of patches to apply in order.
# Add / remove entries here as needed.
managed_patches=(
  "esp_codec_dev-i2c-korvo2-timings.patch"
)

if [[ $# -gt 0 ]]; then
  managed_patches=("$@")
fi

for p in "${managed_patches[@]}"; do
  patch_path="${PATCH_DIR}/${p}"
  if [[ ! -f "${patch_path}" ]]; then
    echo "error: missing patch ${patch_path}" >&2
    exit 1
  fi

  # Check if already applied (reverse-apply succeeds → already in place)
  if patch -p1 -R --dry-run -d "${ROOT_DIR}" < "${patch_path}" >/dev/null 2>&1; then
    echo "skipping ${p} (already applied)"
    continue
  fi

  # Check if the patch applies cleanly
  if patch -p1 --dry-run -d "${ROOT_DIR}" < "${patch_path}" >/dev/null 2>&1; then
    echo "applying ${p}"
    patch -p1 -d "${ROOT_DIR}" < "${patch_path}"
  else
    echo "error: ${p} does not apply (conflict or wrong component version)" >&2
    echo "  Make sure idf.py update-dependencies was run first." >&2
    patch -p1 --dry-run -d "${ROOT_DIR}" < "${patch_path}" >&2 || true
    exit 1
  fi
done

echo "done"
