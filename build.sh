#!/bin/bash -x

IP_ADDRESS=${1:-0.0.0.0}
WS_PORT=${2:-8080}
BUILD_TYPE=${3:-Release}

cmake -S server -B build-server -GNinja -DCMAKE_BUILD_TYPE=$BUILD_TYPE
cmake --build build-server --target server
mkdir -p ./server/data
touch ./server/data/game.dat

source /usr/local/share/emsdk/emsdk_env.sh
emcmake cmake -S client -B build-client -GNinja -DCMAKE_BUILD_TYPE=$BUILD_TYPE -Dprotobuf_BUILD_TESTS=OFF -DPLATFORM=Web -DCMAKE_WEBSOCKET_URL=ws://$IP_ADDRESS:$WS_PORT
cmake --build build-client --target docopt fmt libprotobuf libprotobuf-lite raylib spdlog
cmake --build build-client --target client
