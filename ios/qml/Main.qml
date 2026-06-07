import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    width: 420
    height: 680
    visible: true
    title: "Network Clipboard"

    Connections {
        target: networkClipboard
        function onLatestReceived(text) {
            localClipboard.setText(text)
            preview.text = text
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

        TextField {
            placeholderText: "Server URL"
            text: networkClipboard.serverUrl
            Layout.fillWidth: true
            onEditingFinished: networkClipboard.serverUrl = text
        }

        TextField {
            placeholderText: "API token"
            echoMode: TextInput.Password
            text: networkClipboard.token
            Layout.fillWidth: true
            onEditingFinished: networkClipboard.token = text
        }

        Button {
            text: "Send Clipboard to Network"
            Layout.fillWidth: true
            onClicked: networkClipboard.sendText(localClipboard.text(), Qt.platform.os === "ios" ? "iPhone" : "Android")
        }

        Button {
            text: "Get from Network Clipboard"
            Layout.fillWidth: true
            onClicked: networkClipboard.getLatest()
        }

        TextArea {
            id: preview
            placeholderText: "Latest received text"
            readOnly: true
            wrapMode: TextEdit.Wrap
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Label {
            text: networkClipboard.status
            Layout.fillWidth: true
        }
    }
}
