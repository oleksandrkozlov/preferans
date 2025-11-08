#!/bin/bash -x

# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2025 Oleksandr Kozlov

set -euo pipefail

IP_ADDRESS=${1:-0.0.0.0}
HTTP_PORT=${2:-8000}
WS_PORT=${3:-8080}
CERT=${4:-}
KEY=${5:-}
DH=${6:-}

ssl_opts=()
if [ -n "$CERT" ]; then
  ssl_opts+=( "--cert=${CERT}" )
fi
if [ -n "$KEY" ]; then
  ssl_opts+=( "--key=${KEY}" )
fi
if [ -n "$DH" ]; then
  ssl_opts+=( "--dh=${DH}" )
fi

./build-server/bin/server $IP_ADDRESS $WS_PORT ./server/data/game.dat "${ssl_opts[@]}" > /dev/tty 2>&1 &
python3 -m http.server -d build-client/bin -b $IP_ADDRESS $HTTP_PORT > /dev/tty 2>&1 &
