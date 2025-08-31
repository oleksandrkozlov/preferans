Preferans
=========

Web Multiplayer Card Game in C++
--------------------------------

Server is built with `Boost.Beast` (`WebSocket`) and `Boost.Asio` for networking.
Client uses `Emscripten` for `WebSocket` communication and compilation to
`WebAssembly`, with `raylib`/`raylib-cpp` for rendering and `raygui` for the
GUI.
Both use `Protobuf` for message serialization.

### Docker

```
xhost +local:docker
DOCKER_BUILDKIT=0 docker build --tag preferans .
docker run --privileged -ti -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix -v /home/olkozlo/:/home/olkozlo/ -w /home/olkozlo/Work/workspace/preferans --network host --name preferans preferans
```

### Build Server

```
cmake -S server -B build-server -GNinja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-server --target server
./build-server/bin/server 0.0.0.0 8080
```

### Build Client

```
emcmake cmake -S client -B build-client -GNinja -DCMAKE_BUILD_TYPE=Debug -Dprotobuf_BUILD_TESTS=OFF -DPLATFORM=Web
cmake --build build-client
python3 -m http.server -d build-client/bin 8000
```

### Test

```
cmake --build build-server --target server_test
./build-server/bin/server_test
./tests/run-tests.sh
```

### Dependencies

* [boost.asio](https://www.boost.org/doc/libs/latest/doc/html/boost_asio.html)
* [boost.beast](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/index.html)
* [boost.uuid](https://www.boost.org/doc/libs/latest/libs/uuid/doc/html/uuid.html)
* [cmake](https://cmake.org)
* [docopt.cpp](https://github.com/docopt/docopt.cpp)
* [emscripten](https://emscripten.org)
* [fmt](https://fmt.dev)
* [gsl](https://github.com/microsoft/GSL)
* [protobuf](https://github.com/protocolbuffers/protobuf)
* [range-v3](https://ericniebler.github.io/range-v3)
* [raygui](https://github.com/raysan5/raygui)
* [raylib-cpp](https://robloach.github.io/raylib-cpp)
* [raylib](https://www.raylib.com)
* [spdlog](https://github.com/gabime/spdlog)
* [wasm](https://webassembly.org)

### License

- **Code**: [MIT](LICENSE)
- **Cards**: [Public Domain / CC0](client/resources/cards/LICENSE)
- **Fonts**: [Bitstream Vera / DejaVu](client/resources/fonts/LICENSE)
- **Styles**: [zlib](client/resources/styles/LICENSE)
- **Shells**: [zlib / MIT / NCSA](client/resources/shells/LICENSE)
