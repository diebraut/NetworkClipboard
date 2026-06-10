import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    width: 420
    height: 680
    visible: true
    title: "Network Clipboard"

    function refreshClipboardPreview() {
        preview.text = localClipboard.text()
    }

    onActiveChanged: {
        if (active)
            refreshClipboardPreview()
    }

    Component.onCompleted: {
        refreshClipboardPreview()
        networkClipboard.discoverServer()
    }

    Connections {
        target: networkClipboard
        function onLatestReceived(text) {
            localClipboard.setText(text)
            preview.text = text
        }

    }

    Connections {
        target: localClipboard
        function onTextChanged() {
            refreshClipboardPreview()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Label {
            text: "Network Clipboard"
            font.pixelSize: 24
            font.bold: true
            Layout.fillWidth: true
        }

        Rectangle {
            visible: networkClipboard.servers.length === 0
            color: "#e5e7eb"
            radius: 6
            Layout.fillWidth: true
            Layout.preferredHeight: 44

            Label {
                anchors.centerIn: parent
                text: "Keine Clipboard-Server gefunden."
                color: "#6b7280"
            }
        }

        Label {
            visible: networkClipboard.servers.length === 1
            text: networkClipboard.serverName
            Layout.fillWidth: true
            font.pixelSize: 18
            font.bold: true
        }

        ComboBox {
            visible: networkClipboard.servers.length > 1
            model: networkClipboard.servers
            textRole: "name"
            currentIndex: networkClipboard.selectedServerIndex
            Layout.fillWidth: true
            onActivated: function(index) {
                networkClipboard.selectServer(index)
            }
        }

        Button {
            text: "Send Clipboard to Network"
            Layout.fillWidth: true
            enabled: networkClipboard.selectedServerIndex >= 0
            onClicked: {
                networkClipboard.sendText(preview.text, Qt.platform.os === "ios" ? "iPhone" : "Android")
            }
        }

        Button {
            text: "Get from Network Clipboard"
            Layout.fillWidth: true
            enabled: networkClipboard.selectedServerIndex >= 0
            onClicked: {
                networkClipboard.getLatest()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: preview
                placeholderText: "Latest received text"
                readOnly: false
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                selectByKeyboard: true
                persistentSelection: true
                textFormat: TextEdit.PlainText
            }
        }

        Label {
            text: networkClipboard.status
            Layout.fillWidth: true
        }
    }
}
