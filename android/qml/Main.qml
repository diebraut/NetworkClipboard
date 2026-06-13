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

    function serverDisplayText(name, active) {
        const label = active && name.length > 0 ? name : "Kein Server aktiv"
        return "<span style=\"color:" + (active ? "#16a34a" : "#6b7280")
            + "; font-weight:600; text-decoration:" + (active ? "none" : "line-through")
            + ";\">" + label + "</span>"
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

        Label {
            text: "Clipboard-Server"
            Layout.fillWidth: true
            font.pixelSize: 13
            color: "#6b7280"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ComboBox {
                id: serverBox
                Layout.fillWidth: true
                model: networkClipboard.servers
                textRole: "name"
                currentIndex: networkClipboard.selectedServerIndex
                enabled: networkClipboard.servers.length > 0
                editable: false
                onActivated: function(index) {
                    networkClipboard.selectServer(index)
                }

                contentItem: Item {
                    implicitHeight: serverText.implicitHeight

                    Text {
                        id: serverText
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: 12
                    rightPadding: serverBox.indicator.width + serverBox.spacing + 8
                    width: parent.width
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.RichText
                        text: serverDisplayText(networkClipboard.serverName, networkClipboard.serverActive)
                    }
                }

                delegate: ItemDelegate {
                    width: serverBox.width
                    highlighted: serverBox.highlightedIndex === index
                    contentItem: Text {
                        textFormat: Text.RichText
                        text: serverDisplayText(modelData.name, index === networkClipboard.selectedServerIndex && networkClipboard.serverActive)
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        serverBox.currentIndex = index
                        networkClipboard.selectServer(index)
                        serverBox.popup.close()
                    }
                }
            }

            Button {
                text: "Aktualisieren"
                onClicked: networkClipboard.discoverServer()
            }
        }

        Button {
            text: "Send Clipboard to Network"
            Layout.fillWidth: true
            enabled: networkClipboard.serverActive
            onClicked: {
                networkClipboard.sendText(preview.text, Qt.platform.os === "ios" ? "iPhone" : "Android")
            }
        }

        Button {
            text: "Get from Network Clipboard"
            Layout.fillWidth: true
            enabled: networkClipboard.serverActive
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
