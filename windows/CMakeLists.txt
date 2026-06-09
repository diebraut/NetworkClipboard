cmake_minimum_required(VERSION 3.16)

project(NetworkClipboardWindows VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Network)

qt_standard_project_setup()

qt_add_executable(NetworkClipboardWindows
    src/main.cpp
    src/ApiServer.cpp
    src/ApiServer.h
    src/ClipboardEntry.cpp
    src/ClipboardEntry.h
    src/ClipboardStore.cpp
    src/ClipboardStore.h
    src/TrayController.cpp
    src/TrayController.h
    resources/appicon.rc
)

target_link_libraries(NetworkClipboardWindows
    PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
)

if(WIN32)
    set_target_properties(NetworkClipboardWindows PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
