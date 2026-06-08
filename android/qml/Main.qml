import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    width: 420
    height: 680
    visible: true
    title: "Network Clipboard"

    function applySettings() {
        const text = (serverUrlField.text + "\n" + tokenField.text).trim()
        const urlMatch = text.match(/https?:\/\/[^\s]+/)
        const tokenMatch = text.match(/Bearer\s+([^\s]+)/i)

        const serverUrl = urlMatch ? urlMatch[0].replace(/\/+$/, "") : serverUrlField.text.trim().replace(/\/+$/, "")
        const token = tokenMatch ? tokenMatch[1] : tokenField.text.trim().replace(/^Bearer\s+/i, "")

        serverUrlField.text = serverUrl
        tokenField.text = token
        networkClipboard.serverUrl = serverUrl
        networkClipboard.token = token
    }

    function refreshClipboardPreview() {
        preview.text = localClipboard.text()
    }

    onActiveChanged: {
        if (active)
            refreshClipboardPreview()
    }

    Component.onCompleted: {
        serverUrlField.text = networkClipboard.serverUrl
        tokenField.text = networkClipboard.token
        refreshClipboardPreview()
    }

    Connections {
        target: networkClipboard
        function onLatestReceived(text) {
            localClipboard.setText(text)
            preview.text = text
        }

        function onServerUrlChanged() {
            serverUrlField.text = networkClipboard.serverUrl
        }

        function onTokenChanged() {
            tokenField.text = networkClipboard.token
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

        TextField {
            id: serverUrlField
            placeholderText: "Server URL, z.B. http://192.168.178.42:8787"
            text: networkClipboard.serverUrl
            Layout.fillWidth: true
            onEditingFinished: networkClipboard.serverUrl = text
            onAccepted: applySettings()
        }

        TextField {
            id: tokenField
            placeholderText: "API token"
            echoMode: TextInput.Password
            text: networkClipboard.token
            Layout.fillWidth: true
            onEditingFinished: networkClipboard.token = text
            onAccepted: applySettings()
        }

        Button {
            text: "Server suchen"
            Layout.fillWidth: true
            onClicked: networkClipboard.discoverServer()
        }

        Button {
            text: "Send Clipboard to Network"
            Layout.fillWidth: true
            onClicked: {
                applySettings()
                networkClipboard.sendText(preview.text, Qt.platform.os === "ios" ? "iPhone" : "Android")
            }
        }

        Button {
            text: "Get from Network Clipboard"
            Layout.fillWidth: true
            onClicked: {
                applySettings()
                networkClipboard.getLatest()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: preview
                placeholderText: "Latest received text"
                readOnly: true
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                textFormat: TextEdit.PlainText
            }
        }

        Label {
            text: networkClipboard.status
            Layout.fillWidth: true
        }
    }
}
