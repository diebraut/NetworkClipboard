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
    property string rawPreviewImageBase64: ""
    property string rawPreviewImageSource: ""
    property string pendingPasteboardImageBase64: ""
    property string pendingPasteboardImageFingerprint: ""
    property string observedLocalImageFingerprint: ""
    property string lastAutoSentImageFingerprint: ""
    property string pendingAutoSendImageFingerprint: ""
    property string recentNetworkImageFingerprint: ""
    property bool imageSendInFlight: false
    property double observedPasteboardChangeCount: -1
    property bool forceNextNetworkText: false
    property bool waitingForServerText: false
    property double localPublishGuardUntil: 0
    property int clipboardSyncRetries: 0
    property bool currentPreviewFromLocalClipboard: false
    property string currentPreviewHistoryKey: ""
    readonly property bool compactLayout: width < 520

    function deviceName() {
        return Qt.platform.os === "ios" ? "iPhone" : "Android"
    }

    function syncClipboardToPreview() {
        let pasteboardChangeCount = -1
        if (Qt.platform.os === "ios") {
            pasteboardChangeCount = localClipboard.pasteboardChangeCount()
            if (pasteboardChangeCount >= 0
                    && pasteboardChangeCount === observedPasteboardChangeCount
                    && !pendingAutoSendImageFingerprint
                    && !imageSendInFlight) {
                clipboardSyncRetries = 0
                return
            }
        }

        if (localClipboard.hasImage()) {
            const fingerprint = localClipboard.imageFingerprint()
            if (fingerprint.length === 0)
                return

            const base64 = localClipboard.imageBase64()
            if (base64.length === 0)
                return

            observedPasteboardChangeCount = pasteboardChangeCount

            if (fingerprint !== observedLocalImageFingerprint) {
                observedLocalImageFingerprint = fingerprint
                observedLocalClipboardText = ""
                lastAutoSentText = ""
                showLocalPreviewImage(base64, fingerprint)
            }

            if (networkClipboard.serverActive
                    && !imageSendInFlight
                    && fingerprint !== recentNetworkImageFingerprint
                    && fingerprint !== lastAutoSentImageFingerprint) {
                localPublishGuardUntil = Date.now() + 3000
                pendingAutoSendImageFingerprint = fingerprint
                imageSendInFlight = true
                networkClipboard.sendImage(base64, fingerprint, deviceName())
            }
            clipboardSyncRetries = 0
            return
        }

        if (networkClipboard.serverActive && waitingForServerText)
            return

        const text = localClipboard.text()
        if (text.trim().length === 0)
            return

        if (text === observedLocalClipboardText) {
            observedPasteboardChangeCount = pasteboardChangeCount
            clipboardSyncRetries = 0
            return
        }

        observedPasteboardChangeCount = pasteboardChangeCount
        observedLocalClipboardText = text
        observedLocalImageFingerprint = ""
        lastAutoSentImageFingerprint = ""
        recentNetworkImageFingerprint = ""
        showLocalPreviewText(text)
        pendingPasteboardImageBase64 = ""
        pendingPasteboardImageFingerprint = ""
        deferredPasteboardImageTimer.stop()
        localClipboard.clearPreviewImage()

        if (networkClipboard.serverActive && text !== lastAutoSentText) {
            localPublishGuardUntil = Date.now() + 3000
            lastAutoSentText = text
            networkClipboard.sendText(text, deviceName())
        }
        clipboardSyncRetries = 0
    }

    function scheduleClipboardSync() {
        clipboardSyncRetries = Qt.platform.os === "ios" ? 20 : 12
        clipboardSyncTimer.interval = Qt.platform.os === "ios" ? 700 : 350
        clipboardSyncTimer.restart()
    }

    function showPreviewImage(base64) {
        rawPreviewText = ""
        if (rawPreviewImageBase64 === base64 && rawPreviewImageBase64.length > 0)
            return

        rawPreviewImageBase64 = base64
        rawPreviewImageSource = ""
        imagePreview.triedFileFallback = false
    }

    function historyKey(type, content) {
        return type + ":" + content
    }

    function addHistoryEntry(type, content) {
        if (content.length === 0)
            return

        const key = historyKey(type, content)
        for (let i = 0; i < clipboardHistoryModel.count; ++i) {
            if (clipboardHistoryModel.get(i).key === key) {
                clipboardHistoryModel.move(i, 0, 1)
                return
            }
        }

        clipboardHistoryModel.insert(0, {
            "type": type,
            "content": content,
            "key": key,
            "createdAt": Qt.formatTime(new Date(), "HH:mm")
        })

        while (clipboardHistoryModel.count > 20)
            clipboardHistoryModel.remove(clipboardHistoryModel.count - 1)
    }

    function moveCurrentLocalPreviewToHistory() {
        if (!currentPreviewFromLocalClipboard)
            return

        if (rawPreviewImageBase64.length > 0)
            addHistoryEntry("image", rawPreviewImageBase64)
        else if (rawPreviewText.trim().length > 0)
            addHistoryEntry("text", rawPreviewText)

        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryKey = ""
    }

    function showLocalPreviewText(text) {
        rawPreviewText = text
        rawPreviewImageBase64 = ""
        rawPreviewImageSource = ""
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryKey = historyKey("text", text)
        if (networkClipboard.serverActive)
            addHistoryEntry("text", text)
    }

    function showLocalPreviewImage(base64, fingerprint) {
        showPreviewImage(base64)
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryKey = historyKey("image", fingerprint.length > 0 ? fingerprint : base64)
        if (networkClipboard.serverActive)
            addHistoryEntry("image", base64)
    }

    function showNetworkPreviewText(text) {
        rawPreviewText = text
        rawPreviewImageBase64 = ""
        rawPreviewImageSource = ""
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryKey = ""
    }

    function showNetworkPreviewImage(base64) {
        showPreviewImage(base64)
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryKey = ""
    }

    function setHistoryEntryCurrent(index) {
        if (index < 0 || index >= clipboardHistoryModel.count)
            return

        const entry = clipboardHistoryModel.get(index)
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryKey = entry.key

        if (entry.type === "image") {
            const fingerprint = localClipboard.imageFingerprintFromBase64(entry.content)
            localPublishGuardUntil = networkClipboard.serverActive ? Date.now() + 3000 : 0
            observedLocalImageFingerprint = fingerprint
            observedLocalClipboardText = ""
            lastAutoSentText = ""
            showPreviewImage(entry.content)

            if (localClipboard.setImageBase64(entry.content))
                observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount

            if (networkClipboard.serverActive
                    && !imageSendInFlight
                    && fingerprint !== recentNetworkImageFingerprint
                    && fingerprint !== lastAutoSentImageFingerprint) {
                pendingAutoSendImageFingerprint = fingerprint
                imageSendInFlight = true
                networkClipboard.sendImage(entry.content, fingerprint, deviceName())
            }
            return
        }

        const text = entry.content
        localPublishGuardUntil = networkClipboard.serverActive ? Date.now() + 3000 : 0
        observedLocalClipboardText = text
        observedLocalImageFingerprint = ""
        lastAutoSentImageFingerprint = ""
        recentNetworkImageFingerprint = ""
        rawPreviewText = text
        rawPreviewImageBase64 = ""
        rawPreviewImageSource = ""
        localClipboard.setText(text)
        observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount

        if (networkClipboard.serverActive && text !== lastAutoSentText) {
            lastAutoSentText = text
            networkClipboard.sendText(text, deviceName())
        }
    }

    function appInForeground() {
        return Qt.application.state === Qt.ApplicationActive
    }

    function refreshNetworkClipboard(force) {
        if (Date.now() < localPublishGuardUntil)
            return

        if (appInForeground() && networkClipboard.serverActive) {
            if (force) {
                forceNextNetworkText = true
                waitingForServerText = true
                serverReadGuardTimer.restart()
            }
            if (force)
                networkClipboard.forcePollLatest()
            else
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
        scheduleClipboardSync()
        if (networkClipboard.serverActive)
            refreshNetworkClipboard(true)
        if (Qt.platform.os === "ios" && localClipboard.shouldOfferPasteSettings)
            pasteSettingsDialog.open()
    }

    Timer {
        id: clipboardSyncTimer
        interval: 350
        repeat: false
        onTriggered: {
            interval = Qt.platform.os === "ios" ? 700 : 350
            syncClipboardToPreview()
            if (clipboardSyncRetries > 0) {
                --clipboardSyncRetries
                restart()
            }
        }
    }

    Timer {
        id: serverReadGuardTimer
        interval: 1500
        repeat: false
        onTriggered: waitingForServerText = false
    }

    Timer {
        id: deferredPasteboardImageTimer
        interval: 150
        repeat: false
        onTriggered: {
            if (pendingPasteboardImageBase64.length === 0)
                return

            const base64 = pendingPasteboardImageBase64
            const fingerprint = pendingPasteboardImageFingerprint
            pendingPasteboardImageBase64 = ""
            pendingPasteboardImageFingerprint = ""

            if (!localClipboard.setImageBase64(base64))
                return

            observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount
            observedLocalImageFingerprint = fingerprint
            recentNetworkImageFingerprint = fingerprint
            lastAutoSentImageFingerprint = fingerprint
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
        target: localClipboard
        function onClipboardChanged() {
            scheduleClipboardSync()
        }
    }

    Connections {
        target: networkClipboard
        function onLatestReceived(text) {
            localPublishGuardUntil = 0
            const forceUpdate = forceNextNetworkText
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()

            if (!forceUpdate && rawPreviewText === text && rawPreviewImageBase64.length === 0)
                return

            lastAutoSentText = text
            lastAutoSentImageFingerprint = ""
            observedLocalClipboardText = text
            localClipboard.setText(text)
            observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount
            showNetworkPreviewText(text)
            pendingPasteboardImageBase64 = ""
            pendingPasteboardImageFingerprint = ""
            deferredPasteboardImageTimer.stop()
            localClipboard.clearPreviewImage()
            observedLocalImageFingerprint = ""
            recentNetworkImageFingerprint = ""
        }

        function onLatestImageReceived(base64) {
            localPublishGuardUntil = 0
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()
            pendingAutoSendImageFingerprint = ""
            imageSendInFlight = false

            const incomingFingerprint = localClipboard.imageFingerprintFromBase64(base64)
            const alreadyLocalImage = incomingFingerprint.length > 0
                && (incomingFingerprint === observedLocalImageFingerprint
                    || incomingFingerprint === recentNetworkImageFingerprint
                    || incomingFingerprint === lastAutoSentImageFingerprint)
            if (alreadyLocalImage) {
                observedLocalImageFingerprint = incomingFingerprint
                recentNetworkImageFingerprint = incomingFingerprint
                lastAutoSentImageFingerprint = incomingFingerprint
                observedLocalClipboardText = ""
                lastAutoSentText = ""
                showNetworkPreviewImage(base64)
                return
            }

            observedLocalClipboardText = ""
            lastAutoSentText = ""
            showNetworkPreviewImage(base64)

            observedLocalImageFingerprint = incomingFingerprint
            recentNetworkImageFingerprint = incomingFingerprint
            lastAutoSentImageFingerprint = incomingFingerprint
            pendingPasteboardImageBase64 = base64
            pendingPasteboardImageFingerprint = incomingFingerprint
            localPublishGuardUntil = Date.now() + 2500
            deferredPasteboardImageTimer.restart()
        }

        function onImageSent(fingerprint) {
            localPublishGuardUntil = 0
            lastAutoSentImageFingerprint = fingerprint
            lastAutoSentText = ""
            if (pendingAutoSendImageFingerprint === fingerprint)
                pendingAutoSendImageFingerprint = ""
            imageSendInFlight = false
        }

        function onImageSendFailed(fingerprint) {
            if (pendingAutoSendImageFingerprint === fingerprint)
                pendingAutoSendImageFingerprint = ""
            imageSendInFlight = false
        }

        function onServerActiveChanged() {
            if (networkClipboard.serverActive) {
                observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount
                lastAutoSentText = observedLocalClipboardText
                moveCurrentLocalPreviewToHistory()
                localPublishGuardUntil = 0
                refreshNetworkClipboard(true)
            } else {
                lastAutoSentText = ""
                lastAutoSentImageFingerprint = ""
                forceNextNetworkText = false
                pendingAutoSendImageFingerprint = ""
                recentNetworkImageFingerprint = ""
                imageSendInFlight = false
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

    ListModel {
        id: clipboardHistoryModel
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
                ? (currentPreviewFromLocalClipboard && !networkClipboard.serverActive
                    ? "Bild Lokale-Zwischenablage"
                    : "Bild Netz-Zwischenablage")
                : (currentPreviewFromLocalClipboard && !networkClipboard.serverActive
                    ? "Inhalt Lokale-Zwischenablage"
                    : "Inhalt Netz-Zwischenablage")

            Rectangle {
                id: clipboardBox
                anchors.fill: parent
                anchors.topMargin: Math.round(clipboardFieldsetTitle.implicitHeight / 2)
                color: "transparent"
                border.color: "#9ca3af"
                border.width: 1
                radius: 2

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.topMargin: 14
                    spacing: 8

                    Rectangle {
                        Layout.preferredWidth: root.compactLayout ? 154 : 210
                        Layout.fillHeight: true
                        color: "transparent"
                        border.color: "#e5e7eb"
                        border.width: 1
                        radius: 2

                        Label {
                            visible: clipboardHistoryModel.count === 0
                            anchors.centerIn: parent
                            width: parent.width - 16
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: "Kein lokaler Verlauf"
                            color: "#6b7280"
                        }

                        ListView {
                            visible: clipboardHistoryModel.count > 0
                            anchors.fill: parent
                            anchors.margins: 6
                            clip: true
                            spacing: 6
                            model: clipboardHistoryModel

                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 76
                                color: "transparent"
                                border.color: "#e5e7eb"
                                border.width: 1
                                radius: 2

                                Image {
                                    id: historyThumb
                                    visible: model.type === "image"
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.margins: 6
                                    width: 44
                                    height: 44
                                    source: visible ? "data:image/png;base64," + model.content : ""
                                    fillMode: Image.PreserveAspectFit
                                    cache: false
                                    asynchronous: true
                                }

                                Rectangle {
                                    visible: model.type !== "image"
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.margins: 6
                                    width: 44
                                    height: 44
                                    color: "#f3f4f6"
                                    border.color: "#d1d5db"
                                    border.width: 1

                                    Label {
                                        anchors.centerIn: parent
                                        text: "TXT"
                                        font.pixelSize: 11
                                        color: "#6b7280"
                                    }
                                }

                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 56
                                    anchors.right: parent.right
                                    anchors.rightMargin: 6
                                    anchors.top: parent.top
                                    anchors.topMargin: 6
                                    height: 36
                                    textFormat: Text.RichText
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    text: model.type === "image"
                                        ? "<span style=\"font-weight:600;\">Bild</span><br><span style=\"color:#6b7280;\">"
                                            + model.createdAt + "</span>"
                                        : richClipboardText(model.content)
                                }

                                Button {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 6
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 5
                                    height: 24
                                    text: "Aktuell"
                                    onClicked: setHistoryEntryCurrent(index)
                                }
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ScrollView {
                            visible: rawPreviewImageBase64.length === 0
                            anchors.fill: parent

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
                            id: imagePreview
                            property bool triedFileFallback: false
                            visible: rawPreviewImageBase64.length > 0
                            anchors.fill: parent
                            source: visible
                                ? (rawPreviewImageSource.length > 0
                                    ? rawPreviewImageSource
                                    : "data:image/png;base64," + rawPreviewImageBase64)
                                : ""
                            fillMode: Image.PreserveAspectFit
                            horizontalAlignment: Image.AlignHCenter
                            verticalAlignment: Image.AlignVCenter
                            cache: false
                            asynchronous: false
                            onStatusChanged: {
                                if (status === Image.Error
                                        && !triedFileFallback
                                        && rawPreviewImageBase64.length > 0) {
                                    triedFileFallback = true
                                    rawPreviewImageSource = localClipboard.setPreviewImageBase64(rawPreviewImageBase64)
                                }
                            }
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
            wrapMode: Text.WordWrap
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
