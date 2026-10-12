# syntax=docker/dockerfile:1
#
# Сборка WidgetHub.exe в Linux-контейнере (MinGW-w64).
# Контекст — корень репозитория, а не папка hub: центру нужны README, скриншоты и
# иконки всех виджетов. Что именно попадает в контекст, решает Dockerfile.dockerignore.
#
#   docker build -f hub/Dockerfile --target export --output type=local,dest=hub/dist .

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64 \
        binutils-mingw-w64-x86-64 \
        cmake \
        ninja-build \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# win32-threads дают меньший бинарник, чем posix-вариант toolchain.
RUN update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-win32 || true; \
    update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-win32 || true

WORKDIR /repo
COPY . .

# Каталог упаковывается в один ресурс: README, картинки, иконки виджетов.
RUN python3 hub/tools/pack.py /repo hub/catalog.json hub/res/catalog.pak

WORKDIR /repo/hub
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/repo/hub/cmake/toolchain-mingw.cmake \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
    && cmake --build build \
    && x86_64-w64-mingw32-strip --strip-all build/WidgetHub.exe \
    && ls -la build/WidgetHub.exe

FROM scratch AS export
COPY --from=build /repo/hub/build/WidgetHub.exe /WidgetHub.exe
