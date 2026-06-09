cmake_minimum_required(VERSION 3.16)

project(NetworkClipboardIOS VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Network)

qt_standard_project_setup()

qt_add_executable(NetworkClipboardIOS
    src/main.cpp
    src/NetworkClipboardClient.cpp
    src/NetworkClipboardClient.h
    src/ClipboardBridge.cpp
    src/ClipboardBridge.h
    ../icons/ios/Assets.xcassets
)

set_source_files_properties(../icons/ios/Assets.xcassets
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources
)

set_source_files_properties(qml/Main.qml
    PROPERTIES QT_RESOURCE_ALIAS Main.qml
)

qt_add_qml_module(NetworkClipboardIOS
    URI NetworkClipboardIOS
    VERSION 1.0
    QML_FILES qml/Main.qml
)

target_link_libraries(NetworkClipboardIOS
    PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::Network
)

if(IOS)
    set_target_properties(NetworkClipboardIOS PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER de.localtools.networkclipboard
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
        XCODE_ATTRIBUTE_INFOPLIST_KEY_NSLocalNetworkUsageDescription "Network Clipboard sucht im lokalen Netzwerk nach dem Clipboard-Server."
    )
endif()
