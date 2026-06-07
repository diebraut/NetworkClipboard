cmake_minimum_required(VERSION 3.16)

project(NetworkClipboardAndroid VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Network)

qt_standard_project_setup()

qt_add_executable(NetworkClipboardAndroid
    src/main.cpp
    src/NetworkClipboardClient.cpp
    src/NetworkClipboardClient.h
    src/ClipboardBridge.cpp
    src/ClipboardBridge.h
)

qt_add_qml_module(NetworkClipboardAndroid
    URI NetworkClipboardAndroid
    VERSION 1.0
    QML_FILES qml/Main.qml
)

target_link_libraries(NetworkClipboardAndroid
    PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::Network
)
