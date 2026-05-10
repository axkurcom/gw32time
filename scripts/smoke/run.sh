#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

for t in scripts/smoke/[0-9][0-9]-*.sh; do
  echo "==> $t"
  bash "$t"
done

echo "[smoke] all checks passed"
