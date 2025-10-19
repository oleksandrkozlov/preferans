Preferans
=========

Web Multiplayer Card Game in C++
--------------------------------

The server is implemented using `Boost.Beast` (for `WebSocket`) and
`Boost.Asio` for networking. The client is compiled to `WebAssembly` via
`Emscripten`, using `WebSockets` for communication. Rendering is handled with
`raylib`/`raylib-cpp`, and the GUI is built with `raygui`. Both server and
client use `Protobuf` for message serialization.

### Docker

```
git submodule update --init --recursive
xhost +local:docker
sudo DOCKER_BUILDKIT=0 docker build --tag preferans .
sudo docker run --privileged -ti -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v $HOME:$HOME -w $PWD --network host --name preferans preferans
```

### Build

```
./build.sh [<host>] [(ws | wss)] [<ws-port>] [(Debug | Release)]
```
Example:
```
./build.sh
```
Equivalent to:
```
./build.sh 0.0.0.0 ws 8080 Release
```
Which runs:
```
cmake -S server -B build-server -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build-server --target server
mkdir -p ./server/data
touch ./server/data/game.dat
source /usr/local/share/emsdk/emsdk_env.sh
emcmake cmake -S client -B build-client -GNinja -DCMAKE_BUILD_TYPE=Release \
    -Dprotobuf_BUILD_TESTS=OFF -DPLATFORM=Web \
    -DCMAKE_WEBSOCKET_URL=ws://0.0.0.0:8080
cmake --build build-client --target docopt fmt libprotobuf libprotobuf-lite raylib spdlog
cmake --build build-client --target client
```

### Run

```
./run.sh [<ip-address>] [<http-port>] [<ws-port>] [--cert=<fullchain.pem> --key=<privkey.pem> --dh=<ssl-dhparams.pem>]
```
Example:
```
./run.sh
```
Equivalent to:
```
./run.sh 0.0.0.0 8000 8080
```
Which runs:
```
./build-server/bin/server 0.0.0.0 8080 ./server/data/game.dat &
python3 -m http.server -d build-client/bin -b 0.0.0.0 8000 &
```

### Test (Debug)

```
cmake --build build-server --target test_server
./build-server/bin/test_server
./tests/run-tests.sh
```

### Dependencies

* [boost.asio](https://www.boost.org/doc/libs/latest/doc/html/boost_asio.html)
* [boost.beast](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/index.html)
* [botan](https://botan.randombit.net/)
* [cmake](https://cmake.org)
* [date](https://howardhinnant.github.io/date/date.html)
* [docopt](http://docopt.org/)
* [emscripten](https://emscripten.org)
* [fmt](https://fmt.dev)
* [gsl](https://github.com/microsoft/GSL)
* [openssl](https://www.openssl.org/)
* [protobuf](https://protobuf.dev)
* [range-v3](https://ericniebler.github.io/range-v3/)
* [raygui](https://github.com/raysan5/raygui)
* [raylib-cpp](https://robloach.github.io/raylib-cpp/)
* [raylib](https://www.raylib.com)
* [spdlog](https://gabime.github.io/spdlog/)
* [wasm](https://webassembly.org)

### License

- **Code**: [MIT](LICENSE)
- **Cards**: [Public Domain](client/resources/cards/LICENSE)
- **Fonts**: [DejaVu / Font Awesome Free (OFL)](client/resources/fonts/LICENSE)
- **Styles**: [zlib](client/resources/styles/LICENSE)
- **Shells**: [zlib / MIT / NCSA](client/resources/shells/LICENSE)
