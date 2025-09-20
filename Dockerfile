FROM ubuntu:25.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt update && apt install -y \
    build-essential \
    clang-format \
    clangd \
    cmake \
    cmake-format \
    emscripten \
    git \
    libasound2-dev \
    libboost1.88-dev \
    libcatch2-dev \
    libdocopt-dev \
    libfmt-dev \
    libgl1-mesa-dev \
    libglfw3-dev \
    libglu1-mesa-dev \
    libmsgsl-dev \
    libprotobuf-dev \
    librange-v3-dev \
    libspdlog-dev \
    libwayland-dev \
    libx11-dev \
    libxcursor-dev \
    libxi-dev \
    libxinerama-dev \
    libxkbcommon-dev \
    libxrandr-dev \
    ninja-build \
    protobuf-compiler \
    pytest-asyncio \
    python3 \
    python3-dev \
    python3-pip \
    python3-protobuf \
    python3-pytest \
    python3-pytest-timeout \
    python3-websockets \
    sudo \
    valgrind \
    vim-gtk3 \
    xsel
#   ccache \
#   clang \
#   clang-tidy \
#   cppcheck \
#   curl \
#   doxygen \
#   graphviz \
#   iwyu \
#   kcov \
#   libgtest-dev \
#   pkg-config \

# RUN pip3 install \
#    breathe \
#    sphinx \
#    sphinx_rtd_theme

RUN git clone --depth=1 --branch 5.5 https://github.com/raysan5/raylib.git raylib \
    && emcmake cmake -Hraylib -Braylib/build -GNinja -DCMAKE_BUILD_TYPE=Release -DPLATFORM=Web -DUSE_EXTERNAL_GLFW=ON  -DBUILD_EXAMPLES=OFF \
    && cmake --build raylib/build --target install --parallel \
    && rm -rf raylib

RUN git clone --depth=1 --branch v5.5.0 https://github.com/RobLoach/raylib-cpp.git raylib-cpp \
    && cmake -Hraylib-cpp -Braylib-cpp/build -GNinja -DCMAKE_BUILD_TYPE=Release -DBUILD_RAYLIB_CPP_EXAMPLES=OFF \
    && cmake --build raylib-cpp/build --target install --parallel \
    && rm -rf raylib-cpp

RUN echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER ubuntu

CMD ["/bin/bash"]
