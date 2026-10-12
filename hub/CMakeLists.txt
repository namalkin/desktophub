cmake_minimum_required(VERSION 3.16)

# Версия — одна на весь репозиторий, в файле VERSION. Центр сравнивает её с тегом
# последнего релиза, а release.ps1 ставит тег из того же файла.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" HUB_VERSION)
string(STRIP "${HUB_VERSION}" HUB_VERSION)

project(WidgetHub VERSION ${HUB_VERSION} LANGUAGES CXX)

enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

add_executable(WidgetHub WIN32
    src/main.cpp
    src/hub.cpp
    src/catalog.cpp
    src/control.cpp
    src/download.cpp
    src/update.cpp
    src/markdown.cpp
    src/store.cpp
    src/sys.cpp
    src/json.cpp
    src/ui.cpp
    src/util.cpp
    res/app.rc
)

target_include_directories(WidgetHub PRIVATE src)

target_compile_definitions(WidgetHub PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX              # min/max для gdiplus подставляет ui.h через using std::min/max
    _WIN32_WINNT=0x0A00   # Windows 10
    __USE_MINGW_ANSI_STDIO=1
    HUB_VERSION="${HUB_VERSION}"
)

# Библиотеки — только те, что есть в самой Windows.
target_link_libraries(WidgetHub PRIVATE
    gdiplus
    gdi32
    user32
    shell32   # трей, ShellExecute, SHGetKnownFolderPath
    shlwapi   # SHCreateMemStream: картинки из ресурса
    ole32
    uuid
    comdlg32  # «Указать файл…»
    advapi32  # реестр: автозапуск
    wininet   # скачивание с GitHub
    bcrypt    # SHA-256: совпадает ли виджет с файлом релиза
    dwmapi    # тёмный заголовок окна
)

if(MINGW)
    target_compile_options(WidgetHub PRIVATE
        -Os
        -fno-rtti
        -ffunction-sections
        -fdata-sections
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-missing-field-initializers  # {sizeof(x)} — обычная идиома WinAPI
        -finput-charset=UTF-8
    )
    # -static: libstdc++/libgcc/winpthread внутрь .exe — на целевой машине ничего не нужно.
    target_link_options(WidgetHub PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -mwindows
        -Wl,--gc-sections
        -s
    )
endif()

# Ресурсы .rc лежат рядом с манифестом и catalog.pak — иначе windres их не найдёт.
set_source_files_properties(res/app.rc PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/res"
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/catalog.pak"
)

set_target_properties(WidgetHub PROPERTIES
    OUTPUT_NAME "WidgetHub"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
