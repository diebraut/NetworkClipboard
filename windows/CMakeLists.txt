cmake_minimum_required(VERSION 3.16)

project(NetworkClipboardWindows VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Network)

qt_standard_project_setup()

set(APP_ICON_RESOURCE "")
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.rc")
    enable_language(RC)
    set(APP_ICON_RESOURCE resources/appicon.rc)
endif()

qt_add_executable(NetworkClipboardWindows
    src/main.cpp
    src/ApiServer.cpp
    src/ApiServer.h
    src/ClipboardEntry.cpp
    src/ClipboardEntry.h
    src/ClipboardContentWindow.cpp
    src/ClipboardContentWindow.h
    src/ClipboardStore.cpp
    src/ClipboardStore.h
    src/TrayController.cpp
    src/TrayController.h
    ${APP_ICON_RESOURCE}
)

target_link_libraries(NetworkClipboardWindows
    PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
)
target_compile_definitions(NetworkClipboardWindows
    PRIVATE NETWORKCLIPBOARD_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

if(WIN32)
    set(STOP_NETWORKCLIPBOARD_SCRIPT
        "${CMAKE_CURRENT_BINARY_DIR}/stop_networkclipboard_processes.ps1")
    file(WRITE "${STOP_NETWORKCLIPBOARD_SCRIPT}" [=[
$ErrorActionPreference = 'SilentlyContinue'
sc.exe stop NetworkClipboardServer | Out-Null
Stop-Process -Name NetworkClipboardWindows,NetworkClipboardService -Force
exit 0
]=])

    add_custom_target(stop_running_networkclipboard_processes
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${STOP_NETWORKCLIPBOARD_SCRIPT}"
        COMMENT "Stopping running NetworkClipboard tray/service processes"
        VERBATIM
    )

    add_dependencies(NetworkClipboardWindows stop_running_networkclipboard_processes)
    set_target_properties(NetworkClipboardWindows PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(WIN32)
    qt_add_executable(NetworkClipboardService
        src/service_main.cpp
        src/ApiServer.cpp
        src/ApiServer.h
        src/ClipboardEntry.cpp
        src/ClipboardEntry.h
        src/ClipboardStore.cpp
        src/ClipboardStore.h
    )

    target_link_libraries(NetworkClipboardService
        PRIVATE Qt6::Core Qt6::Network
    )

    add_dependencies(NetworkClipboardService stop_running_networkclipboard_processes)
endif()
