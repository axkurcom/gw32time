#!/usr/bin/env bash
set -euo pipefail

echo "[smoke] build+verify"
make clean
make
make verify
