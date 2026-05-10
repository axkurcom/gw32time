#!/usr/bin/env bash
set -euo pipefail

echo "[smoke] gui ntp table surface"

rg -q 'L"Reach"' src/gui/gui.c
rg -q 'L"Delay"' src/gui/gui.c
rg -q 'L"Jitter"' src/gui/gui.c
rg -q 'L"Score"' src/gui/gui.c
rg -q "hints.ai_family = AF_UNSPEC;" src/gui/gui.c
