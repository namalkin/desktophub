# syntax=docker/dockerfile:1
#
# Сборка Windows .exe в Linux-контейнере (MinGW-w64).
# Docker нужен только здесь; готовый .exe от Docker уже не зависит.

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64 \
        binutils-mingw-w64-x86-64 \
        cmake \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

# posix-варианты toolchain по умолчанию тянут libwinpthread; с -static он
# вкомпилируется внутрь, но win32-threads дают меньший бинарник.
RUN update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-win32 || true; \
    update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-win32 || true

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY res/ ./res/
COPY src/ ./src/

ARG WIDGET_DESKTOP_MODE=2

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-mingw.cmake \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DWIDGET_DESKTOP_MODE=${WIDGET_DESKTOP_MODE} \
    && cmake --build build \
    && x86_64-w64-mingw32-strip --strip-all build/ShortcutDock.exe \
    && ls -la build/ShortcutDock.exe

# Слой-результат: `docker build --output` вытащит из него только .exe.
FROM scratch AS export
COPY --from=build /src/build/ShortcutDock.exe /ShortcutDock.exe
