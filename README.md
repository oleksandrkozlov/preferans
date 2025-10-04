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
xhost +local:docker
DOCKER_BUILDKIT=0 docker build --tag preferans .
docker run --privileged -ti -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v $HOME:$HOME -w $PWD --network host --name preferans preferans
```

### Server

```
cmake -S server -B build-server -GNinja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-server --target server
./build-server/bin/server 0.0.0.0 8080 &
```

### Client

```
git submodule update --init --recursive
emcmake cmake -S client -B build-client -GNinja -DCMAKE_BUILD_TYPE=Debug \
    -Dprotobuf_BUILD_TESTS=OFF -DPLATFORM=Web
cmake --build build-client
python3 -m http.server -d build-client/bin 8000 &
```

### Test

```
cmake --build build-server --target test_server
./build-server/bin/test_server
./tests/run-tests.sh
```

### Dependencies

* [boost.asio](https://www.boost.org/doc/libs/latest/doc/html/boost_asio.html)
* [boost.beast](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/index.html)
* [boost.uuid](https://www.boost.org/doc/libs/latest/libs/uuid/doc/html/uuid.html)
* [cmake](https://cmake.org)
* [docopt](http://docopt.org/)
* [emscripten](https://emscripten.org)
* [fmt](https://fmt.dev)
* [gsl](https://github.com/microsoft/GSL)
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
