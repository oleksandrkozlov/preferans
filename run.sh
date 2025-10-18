#!/bin/bash -x

IP_ADDRESS=${1:-0.0.0.0}
HTTP_PORT=${2:-8000}
WS_PORT=${3:-8080}

./build-server/bin/server $IP_ADDRESS $WS_PORT ./server/data/game.dat > /dev/tty 2>&1 &
python3 -m http.server -d build-client/bin -b $IP_ADDRESS $HTTP_PORT > /dev/tty 2>&1 &
