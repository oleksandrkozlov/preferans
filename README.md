Preferans
=========

Web Multiplayer Card Game in C++
--------------------------------

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
```

### Dependencies

* [cmake](https://cmake.org)
* [gsl](https://github.com/microsoft/GSL)
* [fmt](https://fmt.dev)
* [range-v3](https://ericniebler.github.io/range-v3)
* [raylib](https://www.raylib.com)
* [raylib-cpp](https://robloach.github.io/raylib-cpp)
* [wasm](https://webassembly.org)
* [emscripten](https://emscripten.org)
