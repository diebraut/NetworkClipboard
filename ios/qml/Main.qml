import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 420
    height: 680
    visible: true
    title: "Network Clipboard"

    property string lastAutoSentText: ""
    property string observedLocalClipboardText: ""
    property string rawPreviewText: ""
    property bool forceNextNetworkText: false
    property bool waitingForServerText: false

    function deviceName() {
        return Qt.platform.os === "ios" ? "iPhone" : "Android"
    }

    function syncClipboardToPreview() {
        if (networkClipboard.serverActive && waitingForServerText)
            return

        const text = localClipboard.text()
        if (text.trim().length === 0)
            return

        if (text === observedLocalClipboardText)
            return

        observedLocalClipboardText = text
        rawPreviewText = text

        if (networkClipboard.serverActive && text !== lastAutoSentText) {
            lastAutoSentText = text
            networkClipboard.sendText(text, deviceName())
        }
    }

    function scheduleClipboardSync() {
        clipboardSyncTimer.restart()
    }

    function appInForeground() {
        return Qt.application.state === Qt.ApplicationActive
    }

    function refreshNetworkClipboard(force) {
        if (appInForeground() && networkClipboard.serverActive) {
            if (force) {
                forceNextNetworkText = true
                waitingForServerText = true
                serverReadGuardTimer.restart()
            }
            networkClipboard.pollLatest()
        }
    }

    function serverDisplayText(name, active) {
        const label = active && name.length > 0 ? name : "Kein Server aktiv"
        return "<span style=\"color:" + (active ? "#16a34a" : "#6b7280")
            + "; font-weight:600; text-decoration:" + (active ? "none" : "line-through")
            + ";\">" + label + "</span>"
    }

    function serverListText(name, mainServer, active) {
        const role = mainServer ? "Main-Server" : "Subserver"
        return serverDisplayText(name, active)
            + "<span style=\"color:#6b7280;\"> · " + role + "</span>"
    }

    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
    }

    function linkHref(link) {
        if (link.toLowerCase().startsWith("www."))
            return "https://" + link
        return link
    }

    function richClipboardText(text) {
        const linkPattern = /(?:https?:\/\/|www\.)[^\s<>"']+/ig
        let result = ""
        let lastIndex = 0
        let match = null

        while ((match = linkPattern.exec(text)) !== null) {
            const rawLink = match[0]
            const link = rawLink.replace(/[.,;:!?)]*$/, "")
            const trailing = rawLink.slice(link.length)

            result += escapeHtml(text.slice(lastIndex, match.index))
            result += "<a href=\"" + escapeHtml(linkHref(link)) + "\">" + escapeHtml(link) + "</a>"
            result += escapeHtml(trailing)
            lastIndex = match.index + rawLink.length
        }

        result += escapeHtml(text.slice(lastIndex))
        return result.replace(/\n/g, "<br>")
    }

    onActiveChanged: {
        if (active)
            scheduleClipboardSync()
    }

    Component.onCompleted: {
        if (networkClipboard.serverActive)
            refreshNetworkClipboard(true)
        else
            scheduleClipboardSync()
        if (Qt.platform.os === "ios" && localClipboard.shouldOfferPasteSettings)
            pasteSettingsDialog.open()
    }

    Timer {
        id: clipboardSyncTimer
        interval: 250
        repeat: false
        onTriggered: syncClipboardToPreview()
    }

    Timer {
        id: serverReadGuardTimer
        interval: 1500
        repeat: false
        onTriggered: waitingForServerText = false
    }

    Connections {
        target: Qt.application
        function onStateChanged() {
            if (appInForeground()) {
                scheduleClipboardSync()
                refreshNetworkClipboard(false)
            }
        }
    }

    Connections {
        target: networkClipboard
        function onLatestReceived(text) {
            const forceUpdate = forceNextNetworkText
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()

            if (!forceUpdate && rawPreviewText === text)
                return

            lastAutoSentText = text
            localClipboard.setText(text)
            rawPreviewText = text
        }

        function onServerActiveChanged() {
            if (networkClipboard.serverActive) {
                lastAutoSentText = localClipboard.text()
                refreshNetworkClipboard(true)
            } else {
                lastAutoSentText = ""
                forceNextNetworkText = false
            }
        }

    }

    Timer {
        id: networkPollTimer
        interval: 1000
        repeat: true
        running: Qt.application.state === Qt.ApplicationActive && networkClipboard.serverActive
        onTriggered: refreshNetworkClipboard(false)
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
                        text: serverListText(
                            networkClipboard.serverName,
                            networkClipboard.selectedServerMain,
                            networkClipboard.serverActive)
                    }
                }

                delegate: ItemDelegate {
                    width: serverBox.width
                    highlighted: serverBox.highlightedIndex === index
                    contentItem: Text {
                        textFormat: Text.RichText
                        text: serverListText(modelData.name, modelData.main, modelData.active)
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
                text: "Suchen"
                onClicked: networkClipboard.discoverServer()
            }
        }

        Item {
            id: clipboardFieldset
            Layout.fillWidth: true
            Layout.fillHeight: true

            property string fieldsetTitle: networkClipboard.serverActive
                ? "Inhalt Netz-Zwischenablage"
                : "Letzte bekannte Netz-Zwischenablage"

            Rectangle {
                id: clipboardBox
                anchors.fill: parent
                anchors.topMargin: Math.round(clipboardFieldsetTitle.implicitHeight / 2)
                color: "transparent"
                border.color: "#9ca3af"
                border.width: 1
                radius: 2

                ScrollView {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.topMargin: 12
                    anchors.bottomMargin: 8

                    TextArea {
                        id: preview
                        readOnly: true
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        selectByKeyboard: true
                        persistentSelection: true
                        textFormat: TextEdit.RichText
                        text: richClipboardText(rawPreviewText)
                        onLinkActivated: function(link) { Qt.openUrlExternally(link) }
                        background: Rectangle {
                            color: "transparent"
                            border.width: 0
                        }
                    }
                }
            }

            Rectangle {
                id: clipboardFieldsetTitleBackground
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: clipboardBox.top
                width: clipboardFieldsetTitle.implicitWidth + 10
                height: clipboardFieldsetTitle.implicitHeight
                color: root.color

                Label {
                    id: clipboardFieldsetTitle
                    anchors.centerIn: parent
                    width: parent.width - 10
                    text: clipboardFieldset.fieldsetTitle
                    font.pixelSize: 13
                    color: "#111827"
                    elide: Text.ElideRight
                }
            }
        }

        Label {
            text: networkClipboard.status
            Layout.fillWidth: true
        }
    }

    Dialog {
        id: pasteSettingsDialog
        title: "Einsetzen erlauben"
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        anchors.centerIn: parent
        closePolicy: Popup.NoAutoClose
        onAccepted: {
            localClipboard.markPasteSettingsOfferSeen()
            localClipboard.openAppSettings()
        }
        onRejected: localClipboard.markPasteSettingsOfferSeen()

        Label {
            width: Math.min(pasteSettingsDialog.availableWidth, 340)
            wrapMode: Text.WordWrap
            text: "iOS kann Network Clipboard erlauben, Text aus anderen Apps ohne Nachfrage einzusetzen. Die App kann diese Einstellung nicht selbst setzen, aber die App-Einstellungen öffnen."
        }
    }
}
