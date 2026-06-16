import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 420
    height: 680
    visible: true
    title: "Network Clipboard Server"

    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Label {
            text: "Android Clipboard Server"
            font.pixelSize: 24
            font.bold: true
            Layout.fillWidth: true
        }

        GroupBox {
            title: "Server Info"
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                TextArea {
                    text: androidServer.serverInfo
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    Layout.fillWidth: true
                    background: Rectangle { color: "transparent"; border.width: 0 }
                }

                Label {
                    visible: androidServer.serverUrlsText.length > 0
                    text: "Test auf dem iPhone: erste URL mit /api/discovery in Safari öffnen."
                    color: "#6b7280"
                    font.pixelSize: 12
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            CheckBox {
                text: "Auto-send Android Clipboard"
                checked: androidServer.autoPublish
                onToggled: androidServer.autoPublish = checked
                Layout.fillWidth: true
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: "Publish Now"
                onClicked: androidServer.publishClipboardNow()
                Layout.fillWidth: true
            }

            Button {
                text: "Copy Info"
                onClicked: androidServer.copyServerInfo()
                Layout.fillWidth: true
            }
        }

        GroupBox {
            title: "Latest Clipboard"
            Layout.fillWidth: true
            Layout.fillHeight: true

            ScrollView {
                anchors.fill: parent

                TextArea {
                    text: androidServer.latestContent
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    background: Rectangle { color: "transparent"; border.width: 0 }
                }
            }
        }

        Label {
            text: androidServer.status
            color: "#4b5563"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        Label {
            text: "Hinweis: Android kann Clipboard-Zugriff im Hintergrund je nach Version einschränken. Für echten Dauerbetrieb folgt später ein Foreground-Service."
            color: "#6b7280"
            font.pixelSize: 12
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }
}
