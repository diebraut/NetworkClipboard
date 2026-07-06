cmake_minimum_required(VERSION 3.16)

project(NetworkClipboardMacServer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Network)

qt_standard_project_setup()

qt_add_executable(NetworkClipboardMacServer MACOSX_BUNDLE
    src/main.cpp
    src/ApiServer.cpp
    src/ApiServer.h
    src/ClipboardEntry.cpp
    src/ClipboardEntry.h
    src/ClipboardContentWindow.cpp
    src/ClipboardContentWindow.h
    src/ClipboardStore.cpp
    src/ClipboardStore.h
    src/MacServerController.cpp
    src/MacServerController.h
    resources.qrc
)

target_link_libraries(NetworkClipboardMacServer
    PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
)

set_target_properties(NetworkClipboardMacServer PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "Network Clipboard Mac Server"
    MACOSX_BUNDLE_GUI_IDENTIFIER "de.localtools.NetworkClipboardMacServer"
)
