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
    property string pendingAutoSendText: ""
    property string rawPreviewText: ""
    property string rawPreviewImageBase64: ""
    property string observedLocalImageFingerprint: ""
    property string lastAutoSentImageFingerprint: ""
    property string pendingAutoSendImageFingerprint: ""
    property string recentLocalImageFingerprint: ""
    property bool autoSendInFlight: false
    property bool forceNextNetworkText: false
    property bool waitingForServerText: false
    property double localPublishGuardUntil: 0
    readonly property bool compactLayout: width < 520

    function deviceName() {
        return Qt.platform.os === "ios" ? "iPhone" : "Android"
    }

    function shortLogText(text) {
        return text.slice(0, 80).replace(/\n/g, "\\n")
    }

    function debugState(event) {
        console.log("NCQML " + event
            + " imgLen=" + rawPreviewImageBase64.length
            + " textLen=" + rawPreviewText.length
            + " guardMs=" + Math.max(0, Math.round(localPublishGuardUntil - Date.now()))
            + " autoSend=" + autoSendInFlight
            + " waiting=" + waitingForServerText
            + " serverActive=" + networkClipboard.serverActive)
    }

    function syncClipboardToPreview() {
        if (localClipboard.hasImage()) {
            const fingerprint = localClipboard.imageFingerprint()
            if (fingerprint.length === 0) {
                debugState("local image fingerprint empty")
                return
            }

            const base64 = localClipboard.imageBase64()
            if (base64.length === 0) {
                debugState("local image base64 empty fp=" + fingerprint.slice(0, 12))
                return
            }

            const networkOriginImage = localClipboard.imageFromNetworkClipboard()
            console.log("NCQML local image candidate fp=" + fingerprint.slice(0, 12)
                + " base64Len=" + base64.length
                + " networkOrigin=" + networkOriginImage
                + " observed=" + observedLocalImageFingerprint.slice(0, 12)
                + " lastSent=" + lastAutoSentImageFingerprint.slice(0, 12))
            if (networkOriginImage) {
                debugState("ignore local network-origin image; server image remains authoritative")
                return
            }
            if (fingerprint !== observedLocalImageFingerprint) {
                localPublishGuardUntil = Date.now() + 8000
                recentLocalImageFingerprint = fingerprint
                observedLocalImageFingerprint = fingerprint
                rawPreviewText = ""
                rawPreviewImageBase64 = base64
                observedLocalClipboardText = ""
                lastAutoSentText = ""
                debugState("show local image fp=" + fingerprint.slice(0, 12))
            }

            if (networkClipboard.serverActive
                    && !waitingForServerText
                    && !autoSendInFlight
                    && !networkOriginImage
                    && fingerprint !== lastAutoSentImageFingerprint) {
                console.log("NCQML send local image fp=" + fingerprint.slice(0, 12))
                pendingAutoSendImageFingerprint = fingerprint
                autoSendInFlight = true
                networkClipboard.sendImage(base64, fingerprint, deviceName())
            }
            return
        }

        const text = localClipboard.text()
        if (text.trim().length === 0) {
            if (rawPreviewImageBase64.length > 0) {
                if (Date.now() < localPublishGuardUntil || recentLocalImageFingerprint.length > 0) {
                    debugState("keep image despite empty local text")
                    return
                }
                debugState("clear image because local text empty and guard expired")
                rawPreviewText = ""
                rawPreviewImageBase64 = ""
                observedLocalImageFingerprint = ""
            }
            return
        }

        if (text === observedLocalClipboardText)
            return

        observedLocalClipboardText = text
        localPublishGuardUntil = Date.now() + 4000
        recentLocalImageFingerprint = ""
        observedLocalImageFingerprint = ""
        lastAutoSentImageFingerprint = ""

        if (rawPreviewText !== text) {
            console.log("NCQML show local text and clear image text=" + shortLogText(text))
            rawPreviewText = text
            rawPreviewImageBase64 = ""
        }

        if (networkClipboard.serverActive
                && !waitingForServerText
                && !autoSendInFlight
                && text !== lastAutoSentText) {
            pendingAutoSendText = text
            autoSendInFlight = true
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
        if (!force && (autoSendInFlight || Date.now() < localPublishGuardUntil))
            return

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
        const label = name.length > 0 ? name : "Kein Server aktiv"
        return "<span style=\"color:" + (active ? "#16a34a" : "#6b7280")
            + "; font-weight:600; text-decoration:" + (active ? "none" : "line-through")
            + ";\">" + label + "</span>"
    }

    function serverListText(name, mainServer, active) {
        const role = mainServer ? "Main-Server" : "Subserver"
        return serverDisplayText(name, active)
            + "<span style=\"color:#6b7280;\"> - " + role + "</span>"
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
        scheduleClipboardSync()
    }

    Timer {
        id: clipboardSyncTimer
        interval: 250
        repeat: false
        onTriggered: syncClipboardToPreview()
    }

    Timer {
        id: localClipboardPollTimer
        interval: 500
        repeat: true
        running: Qt.application.state === Qt.ApplicationActive
        onTriggered: syncClipboardToPreview()
    }

    Timer {
        id: serverReadGuardTimer
        interval: 1500
        repeat: false
        onTriggered: {
            waitingForServerText = false
            scheduleClipboardSync()
        }
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
            console.log("NCQML latest text received len=" + text.length + " text=" + shortLogText(text))
            const forceUpdate = forceNextNetworkText
            if (!forceUpdate && (autoSendInFlight || Date.now() < localPublishGuardUntil)) {
                debugState("ignore latest text due guard")
                return
            }

            localPublishGuardUntil = 0
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()

            if (!forceUpdate && rawPreviewText === text && rawPreviewImageBase64.length === 0)
                return

            lastAutoSentText = text
            pendingAutoSendText = ""
            autoSendInFlight = false
            localClipboard.setText(text)
            console.log("NCQML show latest text and clear image text=" + shortLogText(text))
            rawPreviewText = text
            rawPreviewImageBase64 = ""
            observedLocalImageFingerprint = ""
            lastAutoSentImageFingerprint = ""
        }

        function onLatestImageReceived(base64) {
            console.log("NCQML latest image received base64Len=" + base64.length)
            const forceUpdate = forceNextNetworkText
            const incomingFingerprint = localClipboard.imageFingerprintFromBase64(base64)
            if (!forceUpdate && autoSendInFlight) {
                debugState("ignore latest image while auto-send in flight")
                return
            }
            if (!forceUpdate
                    && recentLocalImageFingerprint.length > 0
                    && incomingFingerprint.length > 0
                    && incomingFingerprint !== recentLocalImageFingerprint
                    && Date.now() < localPublishGuardUntil) {
                return
            }

            localPublishGuardUntil = 0
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()
            pendingAutoSendText = ""
            pendingAutoSendImageFingerprint = ""
            autoSendInFlight = false

            const alreadyLocalImage = incomingFingerprint.length > 0
                && (incomingFingerprint === observedLocalImageFingerprint
                    || incomingFingerprint === recentLocalImageFingerprint
                    || incomingFingerprint === lastAutoSentImageFingerprint)
            if (alreadyLocalImage) {
                observedLocalImageFingerprint = incomingFingerprint
                recentLocalImageFingerprint = incomingFingerprint
                lastAutoSentImageFingerprint = incomingFingerprint
                lastAutoSentText = ""
                observedLocalClipboardText = ""
                rawPreviewText = ""
                if (rawPreviewImageBase64.length === 0 || base64.length > rawPreviewImageBase64.length) {
                    console.log("NCQML replace local image with fuller server image oldLen="
                        + rawPreviewImageBase64.length + " newLen=" + base64.length)
                    rawPreviewImageBase64 = base64
                }
                debugState("latest image already local fp=" + incomingFingerprint.slice(0, 12))
                return
            }

            const fingerprint = incomingFingerprint
            observedLocalImageFingerprint = fingerprint
            recentLocalImageFingerprint = fingerprint
            lastAutoSentImageFingerprint = fingerprint
            lastAutoSentText = ""
            observedLocalClipboardText = ""
            rawPreviewText = ""
            rawPreviewImageBase64 = base64
            debugState("show latest image fp=" + fingerprint.slice(0, 12))

            localPublishGuardUntil = Date.now() + 2500
            Qt.callLater(function() {
                localClipboard.setImageBase64(base64)
            })
        }

        function onTextSent(text) {
            localPublishGuardUntil = Date.now() + 1500
            recentLocalImageFingerprint = ""
            lastAutoSentText = text
            lastAutoSentImageFingerprint = ""
            if (pendingAutoSendText === text)
                pendingAutoSendText = ""
            autoSendInFlight = false
        }

        function onTextSendFailed(text) {
            localPublishGuardUntil = 0
            if (pendingAutoSendText === text)
                pendingAutoSendText = ""
            autoSendInFlight = false
        }

        function onImageSent(fingerprint) {
            localPublishGuardUntil = Date.now() + 6000
            recentLocalImageFingerprint = fingerprint
            lastAutoSentImageFingerprint = fingerprint
            lastAutoSentText = ""
            if (pendingAutoSendImageFingerprint === fingerprint)
                pendingAutoSendImageFingerprint = ""
            autoSendInFlight = false
        }

        function onImageSendFailed(fingerprint) {
            localPublishGuardUntil = 0
            recentLocalImageFingerprint = ""
            if (pendingAutoSendImageFingerprint === fingerprint)
                pendingAutoSendImageFingerprint = ""
            autoSendInFlight = false
        }

        function onServerActiveChanged() {
            if (networkClipboard.serverActive) {
                waitingForServerText = false
                forceNextNetworkText = false
                scheduleClipboardSync()
            } else {
                lastAutoSentText = ""
                forceNextNetworkText = false
                waitingForServerText = false
                localPublishGuardUntil = 0
                recentLocalImageFingerprint = ""
                pendingAutoSendText = ""
                pendingAutoSendImageFingerprint = ""
                autoSendInFlight = false
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
        anchors.margins: root.compactLayout ? 12 : 20
        spacing: root.compactLayout ? 8 : 12

        Label {
            text: "Network Clipboard"
            font.pixelSize: 24
            font.bold: true
            Layout.fillWidth: true
        }

        Item {
            id: serverFieldset
            Layout.fillWidth: true
            Layout.preferredHeight: serverControls.implicitHeight + 28

            Rectangle {
                id: serverBoxFrame
                anchors.fill: parent
                anchors.topMargin: Math.round(serverFieldsetTitle.implicitHeight / 2)
                color: "transparent"
                border.color: "#9ca3af"
                border.width: 1
                radius: 2

                GridLayout {
                    id: serverControls
                    anchors.fill: parent
                    anchors.margins: 12
                    anchors.topMargin: 14
                columns: root.compactLayout ? 1 : 2
                columnSpacing: 8
                rowSpacing: 4

                ComboBox {
                    id: serverBox
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    model: networkClipboard.servers
                    textRole: "name"
                    currentIndex: networkClipboard.selectedServerIndex
                    enabled: networkClipboard.manualServerSelection
                        && networkClipboard.servers.length > 0
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
                            anchors.leftMargin: 12
                            anchors.rightMargin: serverBox.indicator.width + serverBox.spacing + 8
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
                        enabled: networkClipboard.manualServerSelection && modelData.active
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

                CheckBox {
                    text: "Manuelle Serverwahl"
                    checked: networkClipboard.manualServerSelection
                    Layout.alignment: Qt.AlignLeft
                    onToggled: networkClipboard.manualServerSelection = checked
                }
            }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: serverBoxFrame.top
                width: serverFieldsetTitle.implicitWidth + 10
                height: serverFieldsetTitle.implicitHeight
                color: root.color

                Label {
                    id: serverFieldsetTitle
                    anchors.centerIn: parent
                    width: parent.width - 10
                    text: "Clipboard-Server"
                    font.pixelSize: 13
                    color: "#111827"
                }
            }
        }

        Item {
            id: clipboardFieldset
            Layout.fillWidth: true
            Layout.fillHeight: true

            property string fieldsetTitle: rawPreviewImageBase64.length > 0
                ? "Bild Netz-Zwischenablage"
                : (networkClipboard.serverActive
                    ? "Inhalt Netz-Zwischenablage"
                    : "Letzte bekannte Netz-Zwischenablage")

            Rectangle {
                id: clipboardBox
                anchors.fill: parent
                anchors.topMargin: Math.round(clipboardFieldsetTitle.implicitHeight / 2)
                color: "transparent"
                border.color: "#9ca3af"
                border.width: 1
                radius: 2

                ScrollView {
                    visible: rawPreviewImageBase64.length === 0
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

                Image {
                    visible: rawPreviewImageBase64.length > 0
                    anchors.fill: parent
                    anchors.margins: 12
                    source: visible ? "data:image/png;base64," + rawPreviewImageBase64 : ""
                    fillMode: Image.PreserveAspectFit
                    horizontalAlignment: Image.AlignHCenter
                    verticalAlignment: Image.AlignVCenter
                    cache: false
                    asynchronous: false
                    onStatusChanged: console.log("NCQML image status=" + status + " imgLen=" + rawPreviewImageBase64.length)
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
            wrapMode: Text.WordWrap
        }
    }
}
