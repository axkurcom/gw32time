#!/usr/bin/env bash
set -euo pipefail

echo "[smoke] cli checker surface"

rg -q "gw32time checker <host" src/cli/cli.c
rg -q -- "--parallel" src/cli/cli.c
rg -q -- "--json" src/cli/cli.c
rg -q -- "--explain" src/cli/cli.c
rg -q "servers test <host>" src/cli/cli.c
