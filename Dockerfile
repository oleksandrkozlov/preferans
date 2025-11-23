FROM archlinux@sha256:c136b06a4f786b84c1cc0d2494fabdf9be8811d15051cd4404deb5c3dc0b2e57
# or archlinux:latest

# Release
RUN pacman -Syu --noconfirm \
    && pacman -S --noconfirm \
    boost \
    botan \
    chrono-date \
    cmake \
    docopt \
    emscripten \
    fmt \
    gcc \
    make \
    microsoft-gsl \
    nginx \
    ninja \
    openssl \
    protobuf \
    range-v3 \
    spdlog \
    sudo \
    && pacman -Scc --noconfirm \
    && ln -sf /usr/share/zoneinfo/Europe/Berlin /etc/localtime

# Debug/Develop
RUN pacman -S --noconfirm \
    catch2 \
    ccache \
    clang \
    debugedit \
    fakeroot \
    gdb \
    git \
    less \
    openssh \
    pkgfile \
    python \
    python-protobuf \
    python-pytest \
    python-pytest-asyncio \
    python-pytest-timeout \
    python-websockets \
    vim \
    && pacman -Scc --noconfirm \
    && pkgfile --update

RUN cd /tmp \
    && mkdir stdexec \
    && cd stdexec \
    && git init -b main \
    && git remote add origin https://github.com/NVIDIA/stdexec.git \
    && git fetch --depth 1 origin 394a7c50237fbe3151350027a7786f001de67355 \
    && git checkout FETCH_HEAD \
    && cmake -H. -Bbuild -GNinja -DCMAKE_BUILD_TYPE=Release -DSTDEXEC_BUILD_TESTS=OFF -DSTDEXEC_BUILD_EXAMPLES=OFF -DSTDEXEC_ENABLE_ASIO=ON -DSTDEXEC_ENABLE_TBB=ON \
    && cmake --build build --parallel \
    && cmake --install build \
    && cd - \
    && rm -rf stdexec

RUN useradd -m -G wheel archuser \
    && echo 'archuser ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER archuser

RUN cd /tmp \
    && git clone --depth=1 https://aur.archlinux.org/include-what-you-use.git \
    && cd include-what-you-use \
    && makepkg -si --noconfirm \
    && cd - \
    && rm -rf include-what-you-use

RUN cd /tmp \
    && git clone --depth=1 https://aur.archlinux.org/cmake-format.git \
    && cd cmake-format \
    && makepkg -si --noconfirm \
    && cd - \
    && rm -rf cmake-format

CMD ["/bin/bash"]
