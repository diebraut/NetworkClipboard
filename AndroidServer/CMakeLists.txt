cmake_minimum_required(VERSION 3.16)

project(NetworkClipboardAndroidServer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Network)

qt_standard_project_setup()

qt_add_executable(NetworkClipboardAndroidServer
    src/main.cpp
    src/AndroidServerController.cpp
    src/AndroidServerController.h
    src/ApiServer.cpp
    src/ApiServer.h
    src/ClipboardEntry.cpp
    src/ClipboardEntry.h
    src/ClipboardStore.cpp
    src/ClipboardStore.h
)

set_source_files_properties(qml/Main.qml
    PROPERTIES QT_RESOURCE_ALIAS Main.qml
)

qt_add_qml_module(NetworkClipboardAndroidServer
    URI NetworkClipboardAndroidServer
    VERSION 1.0
    QML_FILES qml/Main.qml
)

target_link_libraries(NetworkClipboardAndroidServer
    PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::Network
)

if(ANDROID)
    set_target_properties(NetworkClipboardAndroidServer PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        QT_ANDROID_APP_ICON "@mipmap/ic_launcher"
    )
endif()
