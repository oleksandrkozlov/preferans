# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2025 Oleksandr Kozlov

FROM ubuntu:25.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt update && apt install -y \
    build-essential \
    ccache \
    clang-format \
    clang-tidy \
    clangd \
    cmake \
    cmake-format \
    git \
    iwyu \
    libasound2-dev \
    libboost1.88-dev \
    libbotan-3-dev \
    libcatch2-dev \
    libdocopt-dev \
    libgl1-mesa-dev \
    libglfw3-dev \
    libglu1-mesa-dev \
    libhowardhinnant-date-dev \
    libmsgsl-dev \
    libprotobuf-dev \
    librange-v3-dev \
    libssl-dev \
    libwayland-dev \
    libx11-dev \
    libxcursor-dev \
    libxi-dev \
    libxinerama-dev \
    libxkbcommon-dev \
    libxrandr-dev \
    nginx \
    ninja-build \
    pkg-config \
    protobuf-compiler \
    python3 \
    python3-dev \
    python3-pip \
    python3-protobuf \
    python3-pytest \
    python3-pytest-asyncio \
    python3-pytest-timeout \
    python3-websockets \
    sudo \
    valgrind \
    vim-gtk3 \
    xsel

RUN git clone --depth=1 --branch 4.0.17 https://github.com/emscripten-core/emsdk.git /usr/local/share/emsdk \
    && cd /usr/local/share/emsdk \
    && ./emsdk install latest \
    && ./emsdk activate latest

RUN git clone --depth=1 --branch 12.1.0 git@github.com:fmtlib/fmt.git \
    && cmake -Hfmt -Bfmt/build -GNinja -DCMAKE_BUILD_TYPE=Release -DFMT_TEST=OFF \
    && cmake --build fmt/build --target install --parallel \
    && rm -rf fmt

RUN git clone --depth=1 --branch v1.16.0 git@github.com:gabime/spdlog.git \
    && cmake -Hspdlog -Bspdlog/build -GNinja -DCMAKE_BUILD_TYPE=Release -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_FMT_EXTERNAL=ON \
    && cmake --build spdlog/build --target install --parallel \
    && rm -rf spdlog

RUN echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER ubuntu

CMD ["/bin/bash"]
