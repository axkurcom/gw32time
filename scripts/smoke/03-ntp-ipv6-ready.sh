#!/usr/bin/env bash
set -euo pipefail

echo "[smoke] ntp ipv6-ready surface"

rg -q "hints.ai_family = AF_UNSPEC;" src/core/ntp/ntp_socket.c
rg -q "AF_INET6" src/core/ntp/ntp_socket.c
rg -q "host\\[0\\] == '\\['" src/core/ntp/ntp_socket.c
rg -q "from_addr.ss_family != endpoint->ai_family" src/core/ntp/ntp_socket.c
