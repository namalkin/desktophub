cmake_minimum_required(VERSION 3.16)
project(ShortcutDock VERSION 1.0.0 LANGUAGES CXX)

enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

add_executable(ShortcutDock WIN32
    src/main.cpp
    src/widget.cpp
    src/settings.cpp
    src/dialog.cpp
    src/store.cpp
    src/icons.cpp
    src/dnd.cpp
    src/dragout.cpp
    src/sys.cpp
    src/json.cpp
    src/ui.cpp
    src/util.cpp
    res/app.rc
)

target_include_directories(ShortcutDock PRIVATE src)

# Способ крепления блоков к рабочему столу — см. WIDGET_DESKTOP_MODE в src/sys.h.
set(WIDGET_DESKTOP_MODE "2" CACHE STRING "1=Progman, 2=владелец значков, 3=WorkerW, 4=только z-порядок")

target_compile_definitions(ShortcutDock PRIVATE
    WIDGET_DESKTOP_MODE=${WIDGET_DESKTOP_MODE}
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX              # min/max для gdiplus подставляет ui.h через using std::min/max
    _WIN32_WINNT=0x0A00   # Windows 10
    __USE_MINGW_ANSI_STDIO=1
)

# Библиотеки — только те, что есть в самой Windows.
target_link_libraries(ShortcutDock PRIVATE
    gdiplus
    gdi32
    user32
    shell32   # трей, ShellExecute, SHGetKnownFolderPath, значки файлов
    ole32     # COM: приём перетаскивания, IShellLink, IShellItemImageFactory
    uuid      # CLSID_ShellLink, IID_IPersistFile и прочие идентификаторы
    comdlg32  # диалог выбора файла для «Добавить ярлык…»
    advapi32  # реестр: автозапуск
)

if(MINGW)
    target_compile_options(ShortcutDock PRIVATE
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
    target_link_options(ShortcutDock PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -mwindows
        -Wl,--gc-sections
        -s
    )
endif()

# Ресурсы .rc лежат рядом с манифестом — иначе windres его не найдёт.
set_source_files_properties(res/app.rc PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/res"
)

set_target_properties(ShortcutDock PROPERTIES
    OUTPUT_NAME "ShortcutDock"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
