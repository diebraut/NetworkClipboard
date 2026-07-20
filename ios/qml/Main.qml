import QtQuick
import QtQuick.Controls
import QtCore
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
    property string pendingPreviewImageBase64: ""
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
    property int currentPreviewHistoryIndex: -1
    property bool loadingClipboardHistory: false
    property bool previewImageLoading: false
    property bool initialServerReadPending: true
    property int initialServerReadRetries: 0
    property double localClipboardSyncPausedUntil: 0
    property bool serverContentUnavailable: false
    readonly property bool compactLayout: width < 520
    property bool addContentActionsVisible: false
    property bool photoGalleryVisible: false
    property bool photoGalleryLoading: false
    property var recentPhotos: []
    property int selectedPhotoIndex: -1
    property bool selectedPhotoApplying: false
    property string pendingSelectedPhotoId: ""
    property string photoApplyError: ""
    property bool cameraImageLoading: false
    property double cameraImageLoadingStartedAt: 0
    property string pendingCameraImageBase64: ""
    property string cameraApplyError: ""
    readonly property bool debugLogsEnabled: false
    property int pendingDeleteHistoryIndex: -1
    readonly property bool showingServerUnavailablePreview: serverContentUnavailable
        && !currentPreviewFromLocalClipboard
        && currentPreviewHistoryIndex <= 0
    readonly property bool showingInitialServerReadPreview: initialServerReadPending
        && !serverContentUnavailable
        && !currentPreviewFromLocalClipboard
        && currentPreviewHistoryIndex < 0
    readonly property string serverUnavailableKey: "server-unavailable"
    property string serverUnavailableText: "Inhalt Server nicht erreichbar"

    function deviceName() {
        return Qt.platform.os === "ios" ? "iPhone" : "Android"
    }

    function openPhotoGallery() {
        addContentActionsVisible = false
        photoGalleryVisible = true
        photoGalleryLoading = true
        recentPhotos = []
        selectedPhotoIndex = -1
        selectedPhotoApplying = false
        pendingSelectedPhotoId = ""
        photoApplyError = ""
        cameraApplyError = ""
        localClipboard.loadRecentPhotos(18)
    }

    function openCameraCapture() {
        addContentActionsVisible = false
        cameraImageLoading = false
        cameraApplyError = ""
        debugLog("camera open requested")
        localClipboard.openCamera()
    }

    function applySelectedPhoto() {
        if (selectedPhotoIndex < 0 || selectedPhotoIndex >= recentPhotos.length)
            return

        const photo = recentPhotos[selectedPhotoIndex]
        const content = photo.content || ""
        if (content.length > 0) {
            applyPhotoContent(content)
            return
        }

        const assetId = photo.id || ""
        if (assetId.length === 0)
            return

        selectedPhotoApplying = true
        pendingSelectedPhotoId = assetId
        photoApplyError = ""
        debugLog("selected photo apply requested",
            "assetId=" + assetId)
        localClipboard.loadPhotoContent(assetId)
    }

    function applyPhotoContent(content) {
        if (content.length === 0)
            return

        const fingerprint = localClipboard.imageFingerprintFromBase64(content)
        if (fingerprint.length === 0) {
            selectedPhotoApplying = false
            return
        }

        photoGalleryVisible = false
        selectedPhotoApplying = false
        pendingSelectedPhotoId = ""
        photoApplyError = ""
        initialServerReadPending = false
        initialServerReadTimer.stop()
        cancelForcedServerPreview()
        localPublishGuardUntil = networkClipboard.serverActive ? Date.now() + 3000 : 0
        observedLocalImageFingerprint = fingerprint
        observedLocalClipboardText = ""
        lastAutoSentText = ""
        showLocalPreviewImage(content, fingerprint)

        if (localClipboard.setImageBase64(content))
            observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount

        if (networkClipboard.serverActive && !imageSendInFlight) {
            pendingAutoSendImageFingerprint = fingerprint
            imageSendInFlight = true
            debugLog("selected photo send requested",
                "fingerprint=" + fingerprint
                + " base64Len=" + content.length)
            networkClipboard.sendImage(content, fingerprint, deviceName())
        } else if (networkClipboard.serverActive) {
            debugLog("selected photo send skipped",
                "fingerprint=" + fingerprint
                + " imageSendInFlight=" + imageSendInFlight)
        }
    }

    function debugPreviewText(text) {
        const safeText = typeof text === "string" ? text : ""
        return "len=" + safeText.length + " head=\""
            + safeText.slice(0, 40).replace(/\n/g, "\\n") + "\""
    }

    function debugLog(event, details) {
        if (!debugLogsEnabled)
            return
        console.log("[NC QML] " + event + (details ? " " + details : ""))
    }

    function syncClipboardToPreview() {
        let pasteboardChangeCount = -1
        let pasteboardChanged = false
        if (Qt.platform.os === "ios") {
            pasteboardChangeCount = localClipboard.pasteboardChangeCount()
            pasteboardChanged = observedPasteboardChangeCount >= 0
                && pasteboardChangeCount >= 0
                && pasteboardChangeCount !== observedPasteboardChangeCount
            if (pasteboardChangeCount >= 0
                    && pasteboardChangeCount === observedPasteboardChangeCount
                    && !pendingAutoSendImageFingerprint
                    && !imageSendInFlight) {
                clipboardSyncRetries = 0
                return
            }
        }

        if (Date.now() < localClipboardSyncPausedUntil && !pasteboardChanged) {
            clipboardSyncRetries = 0
            return
        }

        if (pasteboardChanged) {
            localClipboardSyncPausedUntil = 0
            initialServerReadPending = false
            initialServerReadTimer.stop()
            cancelForcedServerPreview()
        }

        if (networkClipboard.serverActive && isBrowsingHistoryEntry() && !pasteboardChanged) {
            clipboardSyncRetries = 0
            return
        }

        if ((initialServerReadPending || (networkClipboard.serverActive && waitingForServerText))
                && !pasteboardChanged) {
            clipboardSyncRetries = 0
            return
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
                debugLog("local image detected",
                    "fingerprint=" + fingerprint
                    + " active=" + networkClipboard.serverActive
                    + " initialPending=" + initialServerReadPending)
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
        debugLog("local text detected",
            debugPreviewText(text)
            + " active=" + networkClipboard.serverActive
            + " initialPending=" + initialServerReadPending)
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
        if (Date.now() < localClipboardSyncPausedUntil)
            return

        if (networkClipboard.serverActive && isBrowsingHistoryEntry()
                && Qt.platform.os !== "ios")
            return

        clipboardSyncRetries = Qt.platform.os === "ios" ? 20 : 12
        clipboardSyncTimer.interval = Qt.platform.os === "ios" ? 700 : 350
        clipboardSyncTimer.restart()
    }

    function cancelForcedServerPreview() {
        forceNextNetworkText = false
        waitingForServerText = false
        serverReadGuardTimer.stop()
    }

    function shouldIgnoreIncomingImageDuringLocalPublish(incomingFingerprint) {
        if (!imageSendInFlight && Date.now() >= localPublishGuardUntil)
            return false

        const expectedFingerprint = pendingAutoSendImageFingerprint.length > 0
            ? pendingAutoSendImageFingerprint
            : lastAutoSentImageFingerprint
        if (expectedFingerprint.length > 0 && incomingFingerprint === expectedFingerprint)
            return false

        cancelForcedServerPreview()
        return true
    }

    function isBrowsingHistoryEntry() {
        return currentPreviewHistoryIndex > 0 && !currentPreviewFromLocalClipboard
    }

    function findHistoryIndex(key) {
        if (key.length === 0)
            return -1

        for (let i = 0; i < clipboardHistoryModel.count; ++i) {
            if (clipboardHistoryModel.get(i).key === key)
                return i
        }

        return -1
    }

    function findHistoryIndexPreferIndex(key, preferredIndex) {
        if (key.length === 0)
            return -1

        if (preferredIndex >= 0
                && preferredIndex < clipboardHistoryModel.count
                && clipboardHistoryModel.get(preferredIndex).key === key)
            return preferredIndex

        return findHistoryIndex(key)
    }

    function showPreviewImage(base64) {
        rawPreviewText = ""
        if (rawPreviewImageBase64 === base64 && rawPreviewImageBase64.length > 0) {
            debugLog("preview image unchanged",
                "base64Len=" + base64.length
                + " source=" + (rawPreviewImageSource.length > 0 ? "file" : "data"))
            return
        }

        debugLog("preview image set", "base64Len=" + base64.length)
        rawPreviewImageBase64 = base64
        pendingPreviewImageBase64 = ""
        previewImageLoading = false
        rawPreviewImageSource = ""
        imagePreview.triedFileFallback = false
    }

    function showSelectedHistoryImage(base64) {
        rawPreviewText = ""
        rawPreviewImageBase64 = ""
        rawPreviewImageSource = ""
        pendingPreviewImageBase64 = base64
        previewImageLoading = true
        selectedImageLoadTimer.restart()
    }

    function clearPreviewImageDisplay() {
        rawPreviewImageBase64 = ""
        pendingPreviewImageBase64 = ""
        previewImageLoading = false
        rawPreviewImageSource = ""
        imagePreview.triedFileFallback = false
    }

    function historyKey(type, content) {
        return type + ":" + content
    }

    function isServerUnavailableEntry(entry) {
        return entry && entry.type === "server-unavailable"
    }

    function serverUnavailableEntry() {
        return {
            "type": "server-unavailable",
            "content": serverUnavailableText,
            "key": serverUnavailableKey,
            "imageId": "",
            "thumbnail": "",
            "preview": "",
            "createdAt": ""
        }
    }

    function ensureServerUnavailableHistoryEntry() {
        serverContentUnavailable = true

        for (let i = clipboardHistoryModel.count - 1; i >= 0; --i) {
            const entry = clipboardHistoryModel.get(i)
            if (!isServerUnavailableEntry(entry))
                continue
            if (i === 0) {
                clipboardHistoryModel.set(0, serverUnavailableEntry())
                continue
            }
            clipboardHistoryModel.remove(i)
            if (currentPreviewHistoryIndex > i)
                --currentPreviewHistoryIndex
            else if (currentPreviewHistoryIndex === i) {
                currentPreviewHistoryIndex = -1
                currentPreviewHistoryKey = ""
            }
        }

        if (clipboardHistoryModel.count === 0 || !isServerUnavailableEntry(clipboardHistoryModel.get(0))) {
            clipboardHistoryModel.insert(0, serverUnavailableEntry())
            if (currentPreviewHistoryIndex >= 0)
                ++currentPreviewHistoryIndex
        }

        while (clipboardHistoryModel.count > 15)
            clipboardHistoryModel.remove(clipboardHistoryModel.count - 1)
    }

    function showServerUnavailablePlaceholder(message) {
        serverUnavailableText = message && message.length > 0
            ? message
            : "Inhalt Server nicht erreichbar"
        ensureServerUnavailableHistoryEntry()
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryKey = ""
        currentPreviewHistoryIndex = -1
        clearPreviewImageDisplay()
        rawPreviewText = ""
        previewImageLoading = false
        positionHistoryAtStart()
    }

    function markCurrentLocalClipboardAsSeen() {
        clipboardSyncRetries = 0
        clipboardSyncTimer.stop()
        observedPasteboardChangeCount = Qt.platform.os === "ios"
            ? localClipboard.pasteboardChangeCount()
            : observedPasteboardChangeCount
    }

    function removeServerUnavailablePlaceholder() {
        let removed = false
        for (let i = clipboardHistoryModel.count - 1; i >= 0; --i) {
            if (!isServerUnavailableEntry(clipboardHistoryModel.get(i)))
                continue
            clipboardHistoryModel.remove(i)
            removed = true
            if (currentPreviewHistoryIndex > i)
                --currentPreviewHistoryIndex
            else if (currentPreviewHistoryIndex === i) {
                currentPreviewHistoryIndex = -1
                currentPreviewHistoryKey = ""
            }
        }
        if (removed)
            saveClipboardHistory()
    }

    function normalizedHistoryContent(type, content) {
        if (typeof content !== "string")
            return ""

        if (type === "text" || type === "url")
            return content.replace(/\r\n/g, "\n").replace(/\r/g, "\n").trim()

        return content.trim()
    }

    function historyDedupeKey(entry) {
        if (!entry || typeof entry.type !== "string")
            return ""

        if (entry.type === "server-unavailable")
            return serverUnavailableKey

        if (entry.type === "image") {
            const imageData = imageDataForHistoryEntry(entry)
            if (imageData.length > 0) {
                const visualImageFingerprint = localClipboard.imageVisualFingerprintFromBase64(imageData)
                if (visualImageFingerprint.length > 0)
                    return historyKey("image", visualImageFingerprint)
                const fingerprint = localClipboard.imageFingerprintFromBase64(imageData)
                if (fingerprint.length > 0)
                    return historyKey("image", fingerprint)
            }

            const visualData = entry.thumbnail && entry.thumbnail.length > 0
                ? entry.thumbnail
                : (entry.preview || "")
            if (visualData.length > 0) {
                const visualFingerprint = localClipboard.imageVisualFingerprintFromBase64(visualData)
                if (visualFingerprint.length > 0)
                    return historyKey("image", visualFingerprint)
                const fingerprint = localClipboard.imageFingerprintFromBase64(visualData)
                if (fingerprint.length > 0)
                    return historyKey("image", fingerprint)
            }
            if (entry.imageId && entry.imageId.length > 0)
                return historyKey("image", entry.imageId)
            if (entry.key && entry.key.length > 0)
                return entry.key
            return ""
        }

        if (entry.type === "text" || entry.type === "url") {
            const content = normalizedHistoryContent(entry.type, entry.content || "")
            return content.length > 0 ? historyKey(entry.type, content) : ""
        }

        return ""
    }

    function historyImageFingerprintFromKey(key) {
        if (typeof key !== "string" || key.indexOf("image:") !== 0)
            return ""
        const fingerprint = key.slice(6).toLowerCase()
        if (fingerprint.length !== 64)
            return ""
        for (let i = 0; i < fingerprint.length; ++i) {
            const code = fingerprint.charCodeAt(i)
            const isHex = (code >= 48 && code <= 57)
                || (code >= 97 && code <= 102)
            if (!isHex)
                return ""
        }
        return fingerprint
    }

    function hexBitCount(value) {
        switch (value) {
        case 0: return 0
        case 1:
        case 2:
        case 4:
        case 8:
            return 1
        case 3:
        case 5:
        case 6:
        case 9:
        case 10:
        case 12:
            return 2
        case 7:
        case 11:
        case 13:
        case 14:
            return 3
        case 15:
            return 4
        default:
            return 64
        }
    }

    function imageFingerprintDistance(left, right) {
        if (left.length !== 64 || right.length !== 64)
            return 64

        let distance = 0
        for (let i = 0; i < left.length; ++i)
            distance += hexBitCount(parseInt(left.charAt(i), 16) ^ parseInt(right.charAt(i), 16))
        return distance
    }

    function sameHistoryDedupeKey(left, right) {
        if (left === right)
            return true

        const leftImage = historyImageFingerprintFromKey(left)
        const rightImage = historyImageFingerprintFromKey(right)
        return leftImage.length > 0
            && rightImage.length > 0
            && imageFingerprintDistance(leftImage, rightImage) <= 8
    }

    function imageDataForHistoryEntry(entry) {
        if (!entry || entry.type !== "image")
            return ""
        if (entry.content && entry.content.length > 0)
            return entry.content
        if (entry.imageId && entry.imageId.length > 0) {
            const content = localClipboard.loadHistoryImageBase64(entry.imageId)
            if (content.length > 0)
                return content
        }
        if (entry.preview && entry.preview.length > 0)
            return entry.preview
        if (entry.thumbnail && entry.thumbnail.length > 0)
            return entry.thumbnail
        return ""
    }

    function historyCanScroll(view) {
        return view && historyContentWidth(view) > view.width + 1
    }

    function historyContentWidth(view) {
        if (!view || clipboardHistoryModel.count <= 0)
            return 0

        return clipboardHistoryModel.count * historyTileWidth(view)
            + Math.max(0, clipboardHistoryModel.count - 1) * view.spacing
    }

    function historyDesiredTileWidthForWidth(width) {
        const containerWidth = clipboardBox && clipboardBox.width > 0
            ? clipboardBox.width
            : width
        const widthFromGroupBox = Math.round(containerWidth * 0.07)
        return Math.max(64, widthFromGroupBox)
    }

    function historyVisibleTileCountForWidth(width, spacing) {
        if (width <= 0)
            return 1

        return Math.max(1, Math.floor((width + spacing)
            / (historyDesiredTileWidthForWidth(width) + spacing)))
    }

    function historyTileWidthForWidth(width, spacing) {
        const count = historyVisibleTileCountForWidth(width, spacing)
        if (width <= 0)
            return 64

        return Math.max(64, Math.floor((width - spacing * (count - 1)) / count))
    }

    function historyContentWidthForWidth(width, spacing) {
        if (clipboardHistoryModel.count <= 0)
            return 0

        return clipboardHistoryModel.count * historyTileWidthForWidth(width, spacing)
            + Math.max(0, clipboardHistoryModel.count - 1) * spacing
    }

    function historyMaxContentX(view) {
        if (!historyCanScroll(view))
            return 0
        return Math.max(0, historyContentWidth(view) - view.width)
    }

    function clampHistoryContentX(view) {
        if (!view)
            return

        const maxContentX = historyMaxContentX(view)
        if (view.contentX > maxContentX)
            view.contentX = maxContentX
        else if (view.contentX < 0)
            view.contentX = 0
    }

    function historyDesiredTileWidth(view) {
        return historyDesiredTileWidthForWidth(view ? view.width : 0)
    }

    function historyVisibleTileCount(view) {
        if (!view || view.width <= 0)
            return 1

        return historyVisibleTileCountForWidth(view.width, view.spacing)
    }

    function historyTileWidth(view) {
        const count = historyVisibleTileCount(view)
        if (!view || view.width <= 0)
            return 64

        return historyTileWidthForWidth(view.width, view.spacing)
    }

    function historyFirstVisibleIndex(view) {
        const stride = historyTileWidth(view) + view.spacing
        if (stride <= 0)
            return 0

        return Math.max(0, Math.round(view.contentX / stride))
    }

    function scrollHistoryBy(direction) {
        if (!historyCanScroll(historyListView))
            return

        const visibleCount = historyVisibleTileCount(historyListView)
        const maxFirstIndex = Math.max(0, clipboardHistoryModel.count - visibleCount)
        const firstIndex = historyFirstVisibleIndex(historyListView)
        const targetIndex = Math.max(0, Math.min(maxFirstIndex, firstIndex + direction * visibleCount))
        historyListView.positionViewAtIndex(targetIndex, ListView.Beginning)
    }

    function saveClipboardHistory() {
        if (loadingClipboardHistory)
            return

        const entries = []
        const seen = ({})
        for (let i = 0; i < clipboardHistoryModel.count && i < 15; ++i) {
            const entry = clipboardHistoryModel.get(i)
            if (isServerUnavailableEntry(entry))
                continue
            if (!isValidHistoryEntry(entry))
                continue
            const dedupeKey = historyDedupeKey(entry)
            if (dedupeKey.length === 0 || seen[dedupeKey])
                continue
            if (i > 0)
                seen[dedupeKey] = true
            entries.push({
                "type": entry.type,
                "content": entry.content,
                "key": entry.key,
                "imageId": entry.imageId || "",
                "thumbnail": entry.thumbnail || "",
                "preview": entry.preview || entry.thumbnail || "",
                "createdAt": entry.createdAt
            })
        }
        clipboardHistorySettings.entriesJson = JSON.stringify(entries)
    }

    function historyThumbnail(type, content) {
        if (type !== "image")
            return ""
        return localClipboard.thumbnailBase64FromBase64(content, 96)
    }

    function historyPreview(type, content) {
        if (type !== "image")
            return ""
        return localClipboard.thumbnailBase64FromBase64(content, 640)
    }

    function displayImageForHistoryEntry(entry) {
        return entry.preview || entry.thumbnail || ""
    }

    function previewFromFullImage(base64) {
        const preview = historyPreview("image", base64)
        return preview.length > 0 ? preview : historyThumbnail("image", base64)
    }

    function looksLikePngBase64(value) {
        return typeof value === "string" && value.startsWith("iVBOR")
    }

    function isValidHistoryEntry(entry) {
        if (!entry || typeof entry.type !== "string")
            return false
        if (entry.type === "server-unavailable")
            return true
        if (entry.type === "image") {
            const imageData = looksLikePngBase64(entry.thumbnail) ? entry.thumbnail
                : (looksLikePngBase64(entry.preview) ? entry.preview : entry.content)
            return looksLikePngBase64(imageData)
                && localClipboard.imageHasMeaningfulContentBase64(imageData)
        }
        if (entry.type === "text" || entry.type === "url")
            return normalizedHistoryContent(entry.type, entry.content || "").length > 0
        return false
    }

    function cleanClipboardHistoryModel() {
        const entries = []
        const seen = ({})
        const seenImageKeys = []
        let changed = false

        for (let i = 0; i < clipboardHistoryModel.count; ++i) {
            const entry = clipboardHistoryModel.get(i)
            if (!isValidHistoryEntry(entry)) {
                changed = true
                continue
            }

            const dedupeKey = historyDedupeKey(entry)
            let duplicate = dedupeKey.length === 0 || seen[dedupeKey]
            if (!duplicate && entry.type === "image") {
                for (let imageIndex = 0; imageIndex < seenImageKeys.length; ++imageIndex) {
                    if (sameHistoryDedupeKey(dedupeKey, seenImageKeys[imageIndex])) {
                        duplicate = true
                        break
                    }
                }
            }
            if (duplicate) {
                changed = true
                continue
            }

            if (i > 0)
                seen[dedupeKey] = true
            if (i > 0 && entry.type === "image")
                seenImageKeys.push(dedupeKey)
            const snapshot = historyEntrySnapshot(entry)
            if (snapshot.type === "text" || snapshot.type === "url")
                snapshot.content = normalizedHistoryContent(snapshot.type, snapshot.content)
            if (snapshot.key !== dedupeKey)
                changed = true
            snapshot.key = dedupeKey
            entries.push(snapshot)
        }

        while (entries.length > 15) {
            entries.pop()
            changed = true
        }

        if (!changed)
            return

        const selectedKey = currentPreviewHistoryKey
        clipboardHistoryModel.clear()
        for (let j = 0; j < entries.length; ++j)
            clipboardHistoryModel.append(entries[j])
        currentPreviewHistoryIndex = selectedKey.length > 0
            ? findHistoryIndexPreferIndex(selectedKey, currentPreviewHistoryIndex)
            : -1
    }

    function showRestoredHistoryEntry(index) {
        if (index < 0 || index >= clipboardHistoryModel.count)
            return

        const entry = clipboardHistoryModel.get(index)
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryKey = entry.key
        currentPreviewHistoryIndex = index
        localClipboardSyncPausedUntil = Date.now() + 8000

        if (entry.type === "image") {
            const preview = displayImageForHistoryEntry(entry)
            if (preview.length === 0)
                return
            showPreviewImage(preview)
            return
        }

        clearPreviewImageDisplay()
        rawPreviewText = entry.content
    }

    function restoreClipboardHistory() {
        loadingClipboardHistory = true
        clipboardHistoryModel.clear()

        try {
            const entries = JSON.parse(clipboardHistorySettings.entriesJson)
            if (Array.isArray(entries)) {
                const seen = ({})
                for (let i = 0; i < entries.length && i < 15; ++i) {
                    const entry = entries[i]
                    if (!entry || typeof entry.type !== "string")
                        continue
                    if (entry.type === "server-unavailable")
                        continue
                    const entryContent = normalizedHistoryContent(entry.type,
                        typeof entry.content === "string" ? entry.content : "")

                    const key = typeof entry.key === "string" && entry.key.length > 0
                        ? entry.key
                        : historyKey(entry.type, entryContent)
                    const imageId = entry.type === "image"
                        ? (typeof entry.imageId === "string" && entry.imageId.length > 0
                            ? entry.imageId
                            : (entryContent.length > 0 ? localClipboard.saveHistoryImageBase64(entryContent) : ""))
                        : ""
                    const thumbnail = typeof entry.thumbnail === "string" ? entry.thumbnail : ""
                    const preview = typeof entry.preview === "string" && entry.preview.length > 0
                        ? entry.preview
                        : (entry.type === "image" && entryContent.length > 0 ? historyPreview(entry.type, entryContent) : thumbnail)
                    const restoredEntry = {
                        "type": entry.type,
                        "content": entry.type === "image" ? "" : entryContent,
                        "key": entry.type === "image" && imageId.length > 0 ? historyKey(entry.type, imageId) : key,
                        "imageId": imageId,
                        "thumbnail": thumbnail,
                        "preview": preview,
                        "createdAt": typeof entry.createdAt === "string" ? entry.createdAt : ""
                    }
                    const dedupeKey = historyDedupeKey(restoredEntry)
                    if (isValidHistoryEntry(restoredEntry) && dedupeKey.length > 0 && !seen[dedupeKey]) {
                        seen[dedupeKey] = true
                        clipboardHistoryModel.append(restoredEntry)
                    }
                }
            }
        } catch (error) {
            clipboardHistorySettings.entriesJson = "[]"
        }

        loadingClipboardHistory = false
        cleanClipboardHistoryModel()
        if (clipboardHistoryModel.count > 0) {
            if (initialServerReadPending) {
                currentPreviewFromLocalClipboard = false
                currentPreviewHistoryKey = ""
                currentPreviewHistoryIndex = -1
                clearPreviewImageDisplay()
                rawPreviewText = ""
            } else {
                showRestoredHistoryEntry(0)
            }
            saveClipboardHistory()
        }
    }

    function moveHistoryEntryToIndex(index, targetIndex) {
        if (index < 0 || index >= clipboardHistoryModel.count)
            return -1

        const entries = []
        for (let i = 0; i < clipboardHistoryModel.count; ++i)
            entries.push(historyEntrySnapshot(clipboardHistoryModel.get(i)))

        const movedEntry = entries[index]
        entries.splice(index, 1)
        const boundedTargetIndex = Math.max(0, Math.min(targetIndex, entries.length))
        entries.splice(boundedTargetIndex, 0, movedEntry)

        clipboardHistoryModel.clear()
        for (let j = 0; j < entries.length; ++j)
            clipboardHistoryModel.append(entries[j])

        return boundedTargetIndex
    }

    function findHistoryIndexForDedupeKey(key, preferredIndex) {
        if (key.length === 0)
            return -1

        if (preferredIndex >= 0
                && preferredIndex < clipboardHistoryModel.count) {
            const preferredKey = historyDedupeKey(clipboardHistoryModel.get(preferredIndex))
            if (preferredKey === key || sameHistoryDedupeKey(preferredKey, key))
                return preferredIndex
        }

        for (let i = 0; i < clipboardHistoryModel.count; ++i) {
            const entryKey = historyDedupeKey(clipboardHistoryModel.get(i))
            if (entryKey === key || sameHistoryDedupeKey(entryKey, key))
                return i
        }

        return -1
    }

    function addHistoryEntryAt(type, content, targetIndex) {
        const normalizedContent = normalizedHistoryContent(type, content)
        if (normalizedContent.length === 0)
            return -1

        const imageContent = type === "image" ? normalizedContent : ""
        const imageId = type === "image" ? localClipboard.saveHistoryImageBase64(imageContent) : ""
        if (type === "image" && imageId.length === 0)
            return -1
        const thumbnail = type === "image" ? historyThumbnail(type, imageContent) : ""
        if (type === "image" && thumbnail.length === 0)
            return -1
        if (type === "image" && !localClipboard.imageHasMeaningfulContentBase64(thumbnail))
            return -1

        const storedContent = type === "image" ? "" : normalizedContent
        const imageFingerprint = type === "image" ? localClipboard.imageFingerprintFromBase64(imageContent) : ""
        const key = type === "image"
            ? historyKey(type, imageFingerprint.length > 0 ? imageFingerprint : imageId)
            : historyKey(type, normalizedContent)
        const incomingDedupeKey = type === "image"
            ? historyDedupeKey({
                "type": type,
                "content": storedContent,
                "key": key,
                "imageId": imageId,
                "thumbnail": thumbnail,
                "preview": ""
            })
            : key
        for (let i = 0; i < clipboardHistoryModel.count; ++i) {
            const existing = clipboardHistoryModel.get(i)
            const existingDedupeKey = historyDedupeKey(existing)
            const sameImage = type === "image"
                && existing.type === "image"
                && (existing.imageId === imageId
                    || existingDedupeKey === key
                    || sameHistoryDedupeKey(existingDedupeKey, incomingDedupeKey))
            if (existing.key === key || sameHistoryDedupeKey(existingDedupeKey, incomingDedupeKey) || sameImage) {
                if (targetIndex > 0 && i === 0)
                    continue

                if (type === "image") {
                    if ((!existing.imageId || existing.imageId.length === 0) && imageId.length > 0)
                        clipboardHistoryModel.setProperty(i, "imageId", imageId)
                    if (!existing.thumbnail || existing.thumbnail.length === 0)
                        clipboardHistoryModel.setProperty(i, "thumbnail", thumbnail)
                }
                if (i > 0 || targetIndex === 0)
                    moveHistoryEntryToIndex(i, targetIndex)
                cleanClipboardHistoryModel()
                saveClipboardHistory()
                return findHistoryIndexForDedupeKey(incomingDedupeKey, targetIndex)
            }
        }

        const boundedTargetIndex = Math.max(0, Math.min(targetIndex, clipboardHistoryModel.count))
        clipboardHistoryModel.insert(boundedTargetIndex, {
            "type": type,
            "content": storedContent,
            "key": key,
            "imageId": imageId,
            "thumbnail": thumbnail,
            "preview": type === "image" ? historyPreview(type, imageContent) : "",
            "createdAt": Qt.formatTime(new Date(), "HH:mm")
        })

        while (clipboardHistoryModel.count > 15)
            clipboardHistoryModel.remove(clipboardHistoryModel.count - 1)

        cleanClipboardHistoryModel()
        saveClipboardHistory()
        return findHistoryIndexForDedupeKey(incomingDedupeKey, boundedTargetIndex)
    }

    function addHistoryEntry(type, content) {
        return addHistoryEntryAt(type, content, 0)
    }

    function addHistoryEntryAfterCurrent(type, content) {
        const targetIndex = clipboardHistoryModel.count > 0 ? 1 : 0
        return addHistoryEntryAt(type, content, targetIndex)
    }

    function historyEntryTypeLabel(type, content) {
        if (type === "image")
            return "Bild"
        if (type === "server-unavailable")
            return serverUnavailableText
        const safeContent = typeof content === "string" ? content : ""
        if (safeContent.toLowerCase().startsWith("http://") || safeContent.toLowerCase().startsWith("https://"))
            return "URL"
        return "Text"
    }

    function historyEntryIconLabel(type, content) {
        if (type === "image")
            return ""
        if (type === "server-unavailable")
            return "!"
        return historyEntryTypeLabel(type, content) === "URL" ? "URL" : "TXT"
    }

    function historyEntryPreview(type, content, createdAt) {
        if (type === "image")
            return "<span style=\"font-weight:600;\">Bild</span><br><span style=\"color:#6b7280;\">" + createdAt + "</span>"
        if (type === "server-unavailable")
            return "<span style=\"font-weight:600;\">Server</span><br><span style=\"color:#b91c1c;\">nicht erreichbar</span>"

        const safeContent = typeof content === "string" ? content : ""
        let preview = safeContent.replace(/\s+/g, " ").trim()
        if (preview.length > 42)
            preview = preview.slice(0, 39) + "..."
        return "<span style=\"font-weight:600;\">" + historyEntryTypeLabel(type, safeContent)
            + "</span><br><span style=\"color:#6b7280;\">" + escapeHtml(preview) + "</span>"
    }

    function historyTileText(type, content, index) {
        if (type === "image")
            return (index === 0 ? "Aktuell" : "Alt") + "\nBild"
        if (type === "server-unavailable")
            return "Server\nnicht erreichbar"

        const safeContent = typeof content === "string" ? content : ""
        let preview = safeContent.replace(/\s+/g, " ").trim()
        if (preview.length === 0)
            preview = historyEntryTypeLabel(type, safeContent)
        if (preview.length > 22)
            preview = preview.slice(0, 21) + "..."
        return preview
    }

    function moveCurrentLocalPreviewToHistory() {
        if (!currentPreviewFromLocalClipboard)
            return

        const existingIndex = findHistoryIndexPreferIndex(currentPreviewHistoryKey, currentPreviewHistoryIndex)
        if (existingIndex >= 0) {
            currentPreviewHistoryIndex = existingIndex
            return
        }

        if (rawPreviewImageBase64.length > 0)
            currentPreviewHistoryIndex = addHistoryEntryAfterCurrent("image", rawPreviewImageBase64)
        else if (rawPreviewText.trim().length > 0)
            currentPreviewHistoryIndex = addHistoryEntryAfterCurrent("text", rawPreviewText)

        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0
            ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key
            : ""
    }

    function showLocalPreviewText(text) {
        debugLog("show local text",
            debugPreviewText(text)
            + " active=" + networkClipboard.serverActive)
        if (networkClipboard.serverActive) {
            serverContentUnavailable = false
            removeServerUnavailablePlaceholder()
        } else {
            ensureServerUnavailableHistoryEntry()
        }
        clearPreviewImageDisplay()
        rawPreviewText = text
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryIndex = networkClipboard.serverActive
            ? addHistoryEntry("text", text)
            : addHistoryEntryAfterCurrent("text", text)
        const normalizedText = normalizedHistoryContent("text", text)
        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0
            ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key
            : (normalizedText.length > 0 ? historyKey("text", normalizedText) : "")
    }

    function showLocalPreviewImage(base64, fingerprint) {
        debugLog("show local image",
            "base64Len=" + base64.length
            + " fingerprint=" + fingerprint
            + " active=" + networkClipboard.serverActive)
        if (networkClipboard.serverActive) {
            serverContentUnavailable = false
            removeServerUnavailablePlaceholder()
        } else {
            ensureServerUnavailableHistoryEntry()
        }
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryIndex = networkClipboard.serverActive
            ? addHistoryEntry("image", base64)
            : addHistoryEntryAfterCurrent("image", base64)
        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0 ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key : historyKey("image", fingerprint)
        showPreviewImage(currentPreviewHistoryIndex >= 0
            ? displayImageForHistoryEntry(clipboardHistoryModel.get(currentPreviewHistoryIndex))
            : previewFromFullImage(base64))
    }

    function showNetworkPreviewText(text) {
        debugLog("show network text", debugPreviewText(text))
        serverContentUnavailable = false
        removeServerUnavailablePlaceholder()
        clearPreviewImageDisplay()
        rawPreviewText = text
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryIndex = addHistoryEntry("text", text)
        const normalizedText = normalizedHistoryContent("text", text)
        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0
            ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key
            : (normalizedText.length > 0 ? historyKey("text", normalizedText) : "")
    }

    function showNetworkPreviewImage(base64) {
        debugLog("show network image", "base64Len=" + base64.length)
        serverContentUnavailable = false
        removeServerUnavailablePlaceholder()
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryIndex = addHistoryEntry("image", base64)
        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0 ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key : ""
        showPreviewImage(currentPreviewHistoryIndex >= 0
            ? displayImageForHistoryEntry(clipboardHistoryModel.get(currentPreviewHistoryIndex))
            : previewFromFullImage(base64))
    }

    function selectHistoryEntry(index) {
        if (index < 0 || index >= clipboardHistoryModel.count)
            return

        const entry = clipboardHistoryModel.get(index)
        if (serverContentUnavailable && index === 0)
            return
        if (isServerUnavailableEntry(entry))
            return
        if (index > 0) {
            initialServerReadPending = false
            initialServerReadTimer.stop()
            cancelForcedServerPreview()
            localClipboardSyncPausedUntil = Date.now() + 8000
        }
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryKey = entry.key
        currentPreviewHistoryIndex = index

        if (entry.type === "image") {
            const preview = displayImageForHistoryEntry(entry)
            if (preview.length === 0)
                return
            showSelectedHistoryImage(preview)
            return
        }

        clearPreviewImageDisplay()
        rawPreviewText = entry.content
    }

    function selectNetworkClipboardSlot() {
        currentPreviewFromLocalClipboard = false
        if (clipboardHistoryModel.count > 0) {
            if (serverContentUnavailable && isServerUnavailableEntry(clipboardHistoryModel.get(0))) {
                showServerUnavailablePlaceholder()
                return
            }
            selectHistoryEntry(0)
            positionHistoryAtStart()
            return
        }

        currentPreviewHistoryKey = ""
        currentPreviewHistoryIndex = -1
        clearPreviewImageDisplay()
        rawPreviewText = ""
    }

    function selectedHistoryIndex() {
        return currentPreviewHistoryIndex
    }

    function canSwipeHistoryPreview(direction) {
        if (clipboardHistoryModel.count <= 1 || currentPreviewHistoryIndex < 0)
            return false

        const targetIndex = currentPreviewHistoryIndex + direction
        if (targetIndex < 0 || targetIndex >= clipboardHistoryModel.count)
            return false
        if (isServerUnavailableEntry(clipboardHistoryModel.get(targetIndex)))
            return false

        return true
    }

    function swipeHistoryPreview(direction) {
        if (!canSwipeHistoryPreview(direction))
            return

        const targetIndex = currentPreviewHistoryIndex + direction
        selectHistoryEntry(targetIndex)
        Qt.callLater(function() {
            historyListView.positionViewAtIndex(targetIndex, ListView.Contain)
        })
    }

    function canDeleteHistoryEntry(index) {
        return index > 0
            && index < clipboardHistoryModel.count
            && !isServerUnavailableEntry(clipboardHistoryModel.get(index))
    }

    function requestDeleteSelectedHistoryEntry() {
        if (!canDeleteHistoryEntry(currentPreviewHistoryIndex))
            return

        pendingDeleteHistoryIndex = currentPreviewHistoryIndex
        deleteHistoryEntryDialog.open()
    }

    function deleteHistoryEntry(index) {
        if (!canDeleteHistoryEntry(index))
            return

        clipboardHistoryModel.remove(index)
        saveClipboardHistory()
        pendingDeleteHistoryIndex = -1

        if (clipboardHistoryModel.count === 0) {
            currentPreviewHistoryIndex = -1
            currentPreviewHistoryKey = ""
            currentPreviewFromLocalClipboard = false
            clearPreviewImageDisplay()
            rawPreviewText = ""
            return
        }

        const nextIndex = Math.max(0, Math.min(index, clipboardHistoryModel.count - 1))
        selectHistoryEntry(nextIndex)
    }

    function historyEntrySnapshot(entry) {
        return {
            "type": entry.type,
            "content": entry.content || "",
            "key": entry.key,
            "imageId": entry.imageId || "",
            "thumbnail": entry.thumbnail || "",
            "preview": entry.preview || "",
            "createdAt": entry.createdAt || Qt.formatTime(new Date(), "HH:mm")
        }
    }

    function promoteHistoryEntry(index) {
        const entries = []
        for (let i = 0; i < clipboardHistoryModel.count; ++i)
            entries.push(historyEntrySnapshot(clipboardHistoryModel.get(i)))

        const promotedEntry = entries[index]
        if (index > 0) {
            entries.splice(index, 1)
            entries.unshift(promotedEntry)
            clipboardHistoryModel.clear()
            for (let j = 0; j < entries.length; ++j)
                clipboardHistoryModel.append(entries[j])
        }

        return promotedEntry
    }

    function positionHistoryAtStart() {
        Qt.callLater(function() {
            historyListView.contentX = 0
            historyListView.positionViewAtBeginning()
            historyListView.positionViewAtIndex(0, ListView.Beginning)
        })
    }

    function setHistoryEntryCurrent(index) {
        if (index < 0 || index >= clipboardHistoryModel.count)
            return
        if (serverContentUnavailable || isServerUnavailableEntry(clipboardHistoryModel.get(index)))
            return

        const promotedEntry = promoteHistoryEntry(index)
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryKey = promotedEntry.key
        currentPreviewHistoryIndex = 0
        saveClipboardHistory()
        positionHistoryAtStart()

        if (promotedEntry.type === "image") {
            const content = promotedEntry.content.length > 0 ? promotedEntry.content : localClipboard.loadHistoryImageBase64(promotedEntry.imageId)
            if (content.length === 0)
                return
            const fingerprint = localClipboard.imageFingerprintFromBase64(content)
            localPublishGuardUntil = networkClipboard.serverActive ? Date.now() + 3000 : 0
            observedLocalImageFingerprint = fingerprint
            observedLocalClipboardText = ""
            lastAutoSentText = ""
            showPreviewImage(content)

            if (localClipboard.setImageBase64(content))
                observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount

            if (networkClipboard.serverActive
                    && !imageSendInFlight
                && fingerprint !== recentNetworkImageFingerprint
                && fingerprint !== lastAutoSentImageFingerprint) {
                pendingAutoSendImageFingerprint = fingerprint
                imageSendInFlight = true
                networkClipboard.sendImage(content, fingerprint, deviceName())
            }
            return
        }

        const text = promotedEntry.content
        localPublishGuardUntil = networkClipboard.serverActive ? Date.now() + 3000 : 0
        observedLocalClipboardText = text
        observedLocalImageFingerprint = ""
        lastAutoSentImageFingerprint = ""
        recentNetworkImageFingerprint = ""
        clearPreviewImageDisplay()
        rawPreviewText = text
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
        if (!force && Date.now() < localClipboardSyncPausedUntil)
            return

        if (!force && isBrowsingHistoryEntry())
            return

        if (Date.now() < localPublishGuardUntil)
            return

        if (appInForeground() && networkClipboard.serverActive) {
            if (force) {
                debugLog("refresh latest force",
                    "retries=" + initialServerReadRetries
                    + " serverActive=" + networkClipboard.serverActive)
                serverContentUnavailable = false
                removeServerUnavailablePlaceholder()
                forceNextNetworkText = true
                waitingForServerText = true
                serverReadGuardTimer.restart()
            }
            if (force)
                networkClipboard.forcePollLatest()
            else
                networkClipboard.pollLatest()
        } else if (force) {
            debugLog("refresh latest skipped",
                "foreground=" + appInForeground()
                + " serverActive=" + networkClipboard.serverActive)
        }
    }

    function scheduleInitialServerRead() {
        initialServerReadPending = true
        initialServerReadRetries = 12
        debugLog("initial read scheduled",
            "serverActive=" + networkClipboard.serverActive
            + " index=" + currentPreviewHistoryIndex
            + " key=" + currentPreviewHistoryKey)
        initialServerReadTimer.restart()
    }

    function scheduleInitialServerReadIfActive() {
        if (!networkClipboard.serverActive || !appInForeground() || isBrowsingHistoryEntry())
            return
        scheduleInitialServerRead()
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

    function usePlainPreviewText(text) {
        return text.length > 20000
    }

    function previewDisplayText(text) {
        if (!usePlainPreviewText(text))
            return richClipboardText(text)

        const maxPreviewLength = 80000
        if (text.length <= maxPreviewLength)
            return text

        return text.slice(0, maxPreviewLength)
            + "\n\n[Anzeige gekuerzt: " + text.length + " Zeichen]"
    }

    onActiveChanged: {
        if (active)
            scheduleClipboardSync()
    }

    Component.onCompleted: {
        debugLog("component completed",
            "serverActive=" + networkClipboard.serverActive
            + " status=" + networkClipboard.status)
        scheduleInitialServerRead()
        restoreClipboardHistory()
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
        id: selectedImageLoadTimer
        interval: 1
        repeat: false
        onTriggered: {
            showPreviewImage(pendingPreviewImageBase64)
            pendingPreviewImageBase64 = ""
        }
    }

    Timer {
        id: serverReadGuardTimer
        interval: 1500
        repeat: false
        onTriggered: {
            waitingForServerText = false
            if (initialServerReadPending && initialServerReadRetries > 0) {
                --initialServerReadRetries
                initialServerReadTimer.restart()
                return
            }
            initialServerReadPending = false
            if (!networkClipboard.serverActive)
                showServerUnavailablePlaceholder()
            if (!networkClipboard.serverActive)
                scheduleClipboardSync()
        }
    }

    Timer {
        id: initialServerReadTimer
        interval: 500
        repeat: false
        onTriggered: {
            if (!initialServerReadPending)
                return

            if (isBrowsingHistoryEntry()) {
                debugLog("initial read cancelled browsing history",
                    "index=" + currentPreviewHistoryIndex)
                initialServerReadPending = false
                waitingForServerText = false
                return
            }

            if (networkClipboard.serverActive) {
                debugLog("initial read timer active server",
                    "retries=" + initialServerReadRetries)
                refreshNetworkClipboard(true)
                return
            }

            if (initialServerReadRetries > 0) {
                --initialServerReadRetries
                debugLog("initial read retry waiting for server",
                    "remaining=" + initialServerReadRetries)
                restart()
                return
            }

            debugLog("initial read failed no server")
            initialServerReadPending = false
            showServerUnavailablePlaceholder()
            scheduleClipboardSync()
        }
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

            if (!localClipboard.setImageBase64(base64)) {
                debugLog("deferred image pasteboard set failed",
                    "fingerprint=" + fingerprint + " base64Len=" + base64.length)
                return
            }

            debugLog("deferred image pasteboard set ok",
                "fingerprint=" + fingerprint + " base64Len=" + base64.length)
            observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount
            observedLocalImageFingerprint = fingerprint
            recentNetworkImageFingerprint = fingerprint
            lastAutoSentImageFingerprint = fingerprint
        }
    }

    Timer {
        id: cameraImageReadyTimer
        interval: 700
        repeat: false
        onTriggered: {
            if (pendingCameraImageBase64.length === 0) {
                cameraImageLoading = false
                return
            }

            const base64 = pendingCameraImageBase64
            pendingCameraImageBase64 = ""
            cameraImageLoading = false
            applyPhotoContent(base64)
        }
    }

    Connections {
        target: Qt.application
        function onStateChanged() {
            if (appInForeground()) {
                const pasteboardChangeCount = Qt.platform.os === "ios"
                    ? localClipboard.pasteboardChangeCount()
                    : -1
                const hasNewPasteboardContent = observedPasteboardChangeCount >= 0
                    && pasteboardChangeCount >= 0
                    && pasteboardChangeCount !== observedPasteboardChangeCount
                scheduleClipboardSync()
                if (!hasNewPasteboardContent)
                    refreshNetworkClipboard(false)
            }
        }
    }

    Connections {
        target: localClipboard
        function onClipboardChanged() {
            scheduleClipboardSync()
        }
        function onRecentPhotosLoaded(photos) {
            recentPhotos = photos
            photoGalleryLoading = false
        }
        function onPhotoContentLoaded(assetId, base64) {
            if (assetId !== pendingSelectedPhotoId)
                return

            selectedPhotoApplying = false
            pendingSelectedPhotoId = ""
            debugLog("selected photo content loaded",
                "assetId=" + assetId + " base64Len=" + base64.length)
            if (base64.length > 0)
                applyPhotoContent(base64)
            else
                photoApplyError = "Originalbild konnte nicht geladen werden."
        }
        function onCameraImageProcessingStarted() {
            cameraImageLoading = true
            cameraImageLoadingStartedAt = Date.now()
            pendingCameraImageBase64 = ""
            cameraApplyError = ""
            debugLog("camera image processing started")
        }
        function onCameraImageCaptured(base64) {
            cameraApplyError = ""
            debugLog("camera image received", "base64Len=" + base64.length)
            const remainingMs = Math.max(0, 700 - (Date.now() - cameraImageLoadingStartedAt))
            if (remainingMs > 0) {
                pendingCameraImageBase64 = base64
                cameraImageReadyTimer.interval = remainingMs
                cameraImageReadyTimer.restart()
            } else {
                cameraImageLoading = false
                applyPhotoContent(base64)
            }
        }
        function onCameraCaptureFailed(message) {
            cameraImageLoading = false
            pendingCameraImageBase64 = ""
            if (message.length > 0)
                cameraApplyError = message
            debugLog("camera capture failed", message)
        }
    }

    Connections {
        target: networkClipboard
        function onLatestReceived(text) {
            localPublishGuardUntil = 0
            const forceUpdate = forceNextNetworkText
            debugLog("latest text received",
                debugPreviewText(text)
                + " force=" + forceUpdate
                + " browsing=" + isBrowsingHistoryEntry())
            initialServerReadPending = false
            initialServerReadTimer.stop()
            if (!forceUpdate && isBrowsingHistoryEntry()) {
                addHistoryEntry("text", text)
                currentPreviewHistoryIndex = findHistoryIndexPreferIndex(currentPreviewHistoryKey, currentPreviewHistoryIndex)
                return
            }

            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()

            if (!forceUpdate && rawPreviewText === text && rawPreviewImageBase64.length === 0)
                return

            lastAutoSentText = text
            lastAutoSentImageFingerprint = ""
            observedLocalClipboardText = text
            localClipboardSyncPausedUntil = Date.now() + 2500
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
            const forceUpdate = forceNextNetworkText
            debugLog("latest image received",
                "base64Len=" + base64.length
                + " force=" + forceUpdate
                + " browsing=" + isBrowsingHistoryEntry())
            initialServerReadPending = false
            initialServerReadTimer.stop()
            if (!forceUpdate && isBrowsingHistoryEntry()) {
                addHistoryEntry("image", base64)
                currentPreviewHistoryIndex = findHistoryIndexPreferIndex(currentPreviewHistoryKey, currentPreviewHistoryIndex)
                return
            }

            const incomingFingerprint = localClipboard.imageFingerprintFromBase64(base64)
            if (shouldIgnoreIncomingImageDuringLocalPublish(incomingFingerprint)) {
                debugLog("latest image ignored during local publish",
                    "fingerprint=" + incomingFingerprint
                    + " guardMs=" + Math.max(0, localPublishGuardUntil - Date.now())
                    + " imageSendInFlight=" + imageSendInFlight)
                return
            }

            localPublishGuardUntil = 0
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()
            pendingAutoSendImageFingerprint = ""
            imageSendInFlight = false

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
                localClipboardSyncPausedUntil = Date.now() + 3500
                showNetworkPreviewImage(base64)
                return
            }

            observedLocalClipboardText = ""
            lastAutoSentText = ""
            localClipboardSyncPausedUntil = Date.now() + 3500
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
            localPublishGuardUntil = Date.now() + 6000
            lastAutoSentImageFingerprint = fingerprint
            recentNetworkImageFingerprint = fingerprint
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

        function onLatestReadFailed() {
            debugLog("latest read failed",
                "serverActive=" + networkClipboard.serverActive
                + " retries=" + initialServerReadRetries
                + " status=" + networkClipboard.status)
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()
            initialServerReadTimer.stop()
            if (networkClipboard.serverActive) {
                serverContentUnavailable = false
                removeServerUnavailablePlaceholder()
                if (initialServerReadPending && initialServerReadRetries > 0) {
                    --initialServerReadRetries
                    initialServerReadTimer.restart()
                    return
                }

                initialServerReadPending = false
                markCurrentLocalClipboardAsSeen()
                return
            }

            if (initialServerReadPending && initialServerReadRetries > 0) {
                --initialServerReadRetries
                initialServerReadTimer.restart()
                return
            }

            initialServerReadPending = false
            if (isBrowsingHistoryEntry())
                return
            showServerUnavailablePlaceholder()
            if (!networkClipboard.serverActive)
                scheduleClipboardSync()
        }

        function onLatestReadEmpty() {
            debugLog("latest read empty",
                "serverActive=" + networkClipboard.serverActive
                + " retries=" + initialServerReadRetries
                + " status=" + networkClipboard.status)
            forceNextNetworkText = false
            waitingForServerText = false
            serverReadGuardTimer.stop()
            initialServerReadTimer.stop()
            initialServerReadPending = false
            markCurrentLocalClipboardAsSeen()
            if (isBrowsingHistoryEntry())
                return
            showServerUnavailablePlaceholder("Netz-Zwischenablage leer")
        }

        function onServerActiveChanged() {
            debugLog("server active changed",
                "active=" + networkClipboard.serverActive
                + " status=" + networkClipboard.status)
            if (networkClipboard.serverActive) {
                serverContentUnavailable = false
                removeServerUnavailablePlaceholder()
                observedPasteboardChangeCount = Qt.platform.os === "ios" ? localClipboard.pasteboardChangeCount() : observedPasteboardChangeCount
                lastAutoSentText = observedLocalClipboardText
                moveCurrentLocalPreviewToHistory()
                if (currentPreviewHistoryIndex > 0 && Date.now() >= localClipboardSyncPausedUntil)
                    selectNetworkClipboardSlot()
                localPublishGuardUntil = 0
                scheduleInitialServerRead()
                clipboardSyncRetries = 0
                clipboardSyncTimer.stop()
            } else {
                lastAutoSentText = ""
                lastAutoSentImageFingerprint = ""
                forceNextNetworkText = false
                pendingAutoSendImageFingerprint = ""
                recentNetworkImageFingerprint = ""
                imageSendInFlight = false
                initialServerReadPending = false
                initialServerReadTimer.stop()
                if (isBrowsingHistoryEntry())
                    ensureServerUnavailableHistoryEntry()
                else
                    showServerUnavailablePlaceholder()
                scheduleClipboardSync()
            }
        }

        function onServersChanged() {
            debugLog("servers changed",
                "active=" + networkClipboard.serverActive
                + " count=" + networkClipboard.servers.length)
            scheduleInitialServerReadIfActive()
        }

        function onServerUrlChanged() {
            debugLog("server url changed",
                "active=" + networkClipboard.serverActive)
            scheduleInitialServerReadIfActive()
        }

        function onTokenChanged() {
            debugLog("token changed",
                "active=" + networkClipboard.serverActive
                + " tokenLen=" + networkClipboard.token.length)
            scheduleInitialServerReadIfActive()
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

    Settings {
        id: clipboardHistorySettings
        category: "clipboardHistory"
        property string entriesJson: "[]"
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

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    CheckBox {
                        text: "Manuelle Serverwahl"
                        checked: networkClipboard.manualServerSelection
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        onToggled: networkClipboard.manualServerSelection = checked
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    ToolButton {
                        id: addServerButton
                        text: "+"
                        padding: 0
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                        ToolTip.visible: hovered
                        ToolTip.text: "Server hinzufügen"
                        contentItem: Text {
                            text: addServerButton.text
                            color: "#111827"
                            font.pixelSize: 22
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: width / 2
                            color: addServerButton.pressed ? "#d1d5db" : "#e5e7eb"
                            border.color: "#9ca3af"
                            border.width: 1
                        }
                        onClicked: {
                            debugLog("add server button clicked")
                            addContentActionsVisible = !addContentActionsVisible
                        }
                    }

                    Popup {
                        id: addContentPopup
                        parent: Overlay.overlay
                        x: Math.max(12, Math.min(root.width - width - 12,
                            addServerButton.mapToItem(Overlay.overlay, 0, 0).x + addServerButton.width - width))
                        y: addServerButton.mapToItem(Overlay.overlay, 0, 0).y + addServerButton.height + 8
                        padding: 10
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                        background: Rectangle {
                            radius: 18
                            color: "#ffffff"
                            border.color: "#d1d5db"
                            border.width: 1
                        }

                        contentItem: RowLayout {
                            spacing: 14

                            ToolButton {
                                Layout.preferredWidth: 74
                                Layout.preferredHeight: 78
                                padding: 0
                                ToolTip.visible: hovered
                                ToolTip.text: "Fotos"
                                contentItem: ColumnLayout {
                                    spacing: 6
                                    Rectangle {
                                        Layout.alignment: Qt.AlignHCenter
                                        width: 44
                                        height: 44
                                        radius: 22
                                        color: "#7c3aed"
                                        Canvas {
                                            anchors.centerIn: parent
                                            width: 24
                                            height: 24
                                            onPaint: {
                                                const ctx = getContext("2d")
                                                ctx.clearRect(0, 0, width, height)
                                                ctx.strokeStyle = "#ffffff"
                                                ctx.fillStyle = "#ffffff"
                                                ctx.lineWidth = 2
                                                ctx.strokeRect(4, 5, 16, 14)
                                                ctx.beginPath()
                                                ctx.arc(15.5, 9.5, 2, 0, Math.PI * 2)
                                                ctx.fill()
                                                ctx.beginPath()
                                                ctx.moveTo(5, 18)
                                                ctx.lineTo(10, 13)
                                                ctx.lineTo(13, 16)
                                                ctx.lineTo(16, 12)
                                                ctx.lineTo(20, 18)
                                                ctx.stroke()
                                            }
                                        }
                                    }
                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "Fotos"
                                        font.pixelSize: 12
                                        color: "#111827"
                                    }
                                }
                                background: Rectangle {
                                    radius: 10
                                    color: parent.pressed ? "#f3f4f6" : "transparent"
                                }
                                onClicked: {
                                    addContentPopup.close()
                                    debugLog("photos action clicked")
                                }
                            }

                            ToolButton {
                                Layout.preferredWidth: 74
                                Layout.preferredHeight: 78
                                padding: 0
                                ToolTip.visible: hovered
                                ToolTip.text: "Kamera"
                                contentItem: ColumnLayout {
                                    spacing: 6
                                    Rectangle {
                                        Layout.alignment: Qt.AlignHCenter
                                        width: 44
                                        height: 44
                                        radius: 22
                                        color: "#ec4899"
                                        Canvas {
                                            anchors.centerIn: parent
                                            width: 24
                                            height: 24
                                            onPaint: {
                                                const ctx = getContext("2d")
                                                ctx.clearRect(0, 0, width, height)
                                                ctx.strokeStyle = "#ffffff"
                                                ctx.lineWidth = 2
                                                ctx.beginPath()
                                                ctx.rect(4, 8, 16, 11)
                                                ctx.stroke()
                                                ctx.beginPath()
                                                ctx.moveTo(8, 8)
                                                ctx.lineTo(10, 5)
                                                ctx.lineTo(14, 5)
                                                ctx.lineTo(16, 8)
                                                ctx.stroke()
                                                ctx.beginPath()
                                                ctx.arc(12, 13.5, 3.2, 0, Math.PI * 2)
                                                ctx.stroke()
                                            }
                                        }
                                    }
                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "Kamera"
                                        font.pixelSize: 12
                                        color: "#111827"
                                    }
                                }
                                background: Rectangle {
                                    radius: 10
                                    color: parent.pressed ? "#f3f4f6" : "transparent"
                                }
                                onClicked: {
                                    addContentPopup.close()
                                    debugLog("camera action clicked")
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    visible: false
                    Layout.fillWidth: true
                    Layout.columnSpan: serverControls.columns
                    Layout.preferredHeight: 0
                    radius: 16
                    color: "#ffffff"
                    border.color: "#d1d5db"
                    border.width: 1

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 18

                        ToolButton {
                            Layout.preferredWidth: 82
                            Layout.preferredHeight: 72
                            padding: 0
                            contentItem: ColumnLayout {
                                spacing: 5
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "\u25a7"
                                    color: "#ffffff"
                                    font.pixelSize: 24
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 44
                                        height: 44
                                        radius: 22
                                        color: "#7c3aed"
                                        z: -1
                                    }
                                }
                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "Fotos"
                                    font.pixelSize: 12
                                    color: "#111827"
                                }
                            }
                            background: Rectangle {
                                radius: 10
                                color: parent.pressed ? "#f3f4f6" : "transparent"
                            }
                            onClicked: {
                                addContentActionsVisible = false
                                debugLog("photos action clicked")
                            }
                        }

                        ToolButton {
                            Layout.preferredWidth: 82
                            Layout.preferredHeight: 72
                            padding: 0
                            contentItem: ColumnLayout {
                                spacing: 5
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "\u25c9"
                                    color: "#ffffff"
                                    font.pixelSize: 24
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 44
                                        height: 44
                                        radius: 22
                                        color: "#ec4899"
                                        z: -1
                                    }
                                }
                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "Kamera"
                                    font.pixelSize: 12
                                    color: "#111827"
                                }
                            }
                            background: Rectangle {
                                radius: 10
                                color: parent.pressed ? "#f3f4f6" : "transparent"
                            }
                            onClicked: {
                                addContentActionsVisible = false
                                debugLog("camera action clicked")
                            }
                        }
                    }
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

            property string fieldsetTitle: showingServerUnavailablePreview
                ? "Inhalt Netz-Zwischenablage"
                : showingInitialServerReadPreview
                ? "Inhalt Netz-Zwischenablage"
                : rawPreviewImageBase64.length > 0
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

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.topMargin: 14
                    anchors.bottomMargin: 8
                    spacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 8

                        Label {
                            text: showingServerUnavailablePreview
                                ? serverUnavailableText
                                : showingInitialServerReadPreview
                                ? "Serverinhalt wird gelesen"
                                : clipboardHistoryModel.count > 0
                                ? (currentPreviewHistoryIndex === 0
                                    ? "Aktueller Eintrag"
                                    : "Alter Eintrag")
                                : ""
                            color: "#6b7280"
                            Layout.fillWidth: true
                            visible: text.length > 0
                        }

                        Item {
                            visible: showingServerUnavailablePreview
                                || showingInitialServerReadPreview
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Rectangle {
                                anchors.centerIn: parent
                                width: Math.max(120, Math.min(parent.width - 24, 300))
                                height: Math.max(120, Math.min(parent.height - 24, 180))
                                color: showingServerUnavailablePreview ? "#fef2f2" : "#f8fafc"
                                border.color: showingServerUnavailablePreview ? "#fca5a5" : "#cbd5e1"
                                border.width: 1
                                radius: 4

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 18
                                    spacing: 10

                                    Rectangle {
                                        Layout.alignment: Qt.AlignHCenter
                                        width: 52
                                        height: 52
                                        radius: 26
                                        color: "#ffffff"
                                        border.color: showingServerUnavailablePreview ? "#ef4444" : "#64748b"
                                        border.width: 2

                                        Label {
                                            anchors.centerIn: parent
                                            text: showingServerUnavailablePreview ? "!" : "..."
                                            font.pixelSize: 30
                                            font.bold: true
                                            color: showingServerUnavailablePreview ? "#b91c1c" : "#475569"
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: showingServerUnavailablePreview
                                            ? serverUnavailableText
                                            : "Lade Netz-Zwischenablage..."
                                        font.pixelSize: 17
                                        font.bold: true
                                        color: showingServerUnavailablePreview ? "#991b1b" : "#334155"
                                        horizontalAlignment: Text.AlignHCenter
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }

                        ScrollView {
                            visible: !showingServerUnavailablePreview
                                && !showingInitialServerReadPreview
                                && rawPreviewImageBase64.length === 0
                                && !previewImageLoading
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            TextArea {
                                id: preview
                                readOnly: true
                                wrapMode: TextEdit.Wrap
                                selectByMouse: true
                                selectByKeyboard: true
                                persistentSelection: true
                                textFormat: usePlainPreviewText(rawPreviewText)
                                    ? TextEdit.PlainText
                                    : TextEdit.RichText
                                text: previewDisplayText(rawPreviewText)
                                onLinkActivated: function(link) { Qt.openUrlExternally(link) }
                                background: Rectangle {
                                    color: "transparent"
                                    border.width: 0
                                }

                                DragHandler {
                                    target: null
                                    grabPermissions: PointerHandler.CanTakeOverFromAnything
                                        | PointerHandler.ApprovesTakeOverByAnything
                                    xAxis.enabled: true
                                    yAxis.enabled: false
                                    property bool swipeHandled: false
                                    onActiveChanged: {
                                        if (active) {
                                            swipeHandled = false
                                            return
                                        }
                                    }

                                    onTranslationChanged: {
                                        if (!active || swipeHandled)
                                            return
                                        const dx = translation.x
                                        const threshold = Math.max(42, parent.width * 0.12)
                                        if (Math.abs(dx) < threshold)
                                            return

                                        swipeHandled = true
                                        swipeHistoryPreview(dx < 0 ? 1 : -1)
                                    }
                                }
                            }
                        }

                        Image {
                            id: imagePreview
                            property bool triedFileFallback: false
                            visible: !showingServerUnavailablePreview
                                && !showingInitialServerReadPreview
                                && rawPreviewImageBase64.length > 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            source: rawPreviewImageBase64.length > 0
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
                                debugLog("preview image status",
                                    "status=" + status
                                    + " base64Len=" + rawPreviewImageBase64.length
                                    + " source=" + (rawPreviewImageSource.length > 0 ? "file" : "data")
                                    + "painted=" + paintedWidth + "x" + paintedHeight
                                    + "implicit=" + implicitWidth + "x" + implicitHeight)
                                if (status === Image.Error
                                        && !triedFileFallback
                                        && rawPreviewImageBase64.length > 0) {
                                    triedFileFallback = true
                                    rawPreviewImageSource = localClipboard.setPreviewImageBase64(rawPreviewImageBase64)
                                    debugLog("preview image fallback requested",
                                        "fallbackSourceLen=" + rawPreviewImageSource.length
                                        + " base64Len=" + rawPreviewImageBase64.length)
                                } else if (status === Image.Error) {
                                    debugLog("preview image failed",
                                        "fallbackTried=" + triedFileFallback
                                        + " source=" + (rawPreviewImageSource.length > 0 ? rawPreviewImageSource : "data-url"))
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                property real pressX: 0
                                property real pressY: 0

                                onPressed: function(mouse) {
                                    pressX = mouse.x
                                    pressY = mouse.y
                                }
                                onReleased: function(mouse) {
                                    const dx = mouse.x - pressX
                                    const dy = mouse.y - pressY
                                    const threshold = Math.max(42, width * 0.12)
                                    if (Math.abs(dx) < threshold || Math.abs(dx) < Math.abs(dy) * 1.25)
                                        return

                                    swipeHistoryPreview(dx < 0 ? 1 : -1)
                                }
                            }
                        }

                        BusyIndicator {
                            visible: previewImageLoading
                            running: visible
                            Layout.alignment: Qt.AlignHCenter
                        }

                        RowLayout {
                            visible: canDeleteHistoryEntry(currentPreviewHistoryIndex)
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignRight
                            spacing: 8

                            ToolButton {
                                id: deleteHistoryEntryButton
                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 36
                                padding: 0
                                ToolTip.visible: hovered
                                ToolTip.text: "Eintrag löschen"
                                contentItem: Item {
                                    Canvas {
                                        anchors.centerIn: parent
                                        width: 20
                                        height: 20
                                        onPaint: {
                                            const ctx = getContext("2d")
                                            ctx.clearRect(0, 0, width, height)
                                            ctx.strokeStyle = "#ffffff"
                                            ctx.lineWidth = 2
                                            ctx.lineCap = "round"
                                            ctx.lineJoin = "round"
                                            ctx.beginPath()
                                            ctx.moveTo(5, 6)
                                            ctx.lineTo(15, 6)
                                            ctx.moveTo(8, 6)
                                            ctx.lineTo(8, 17)
                                            ctx.moveTo(12, 6)
                                            ctx.lineTo(12, 17)
                                            ctx.moveTo(6, 6)
                                            ctx.lineTo(7, 18)
                                            ctx.lineTo(13, 18)
                                            ctx.lineTo(14, 6)
                                            ctx.moveTo(4, 4)
                                            ctx.lineTo(16, 4)
                                            ctx.moveTo(8, 4)
                                            ctx.lineTo(9, 2)
                                            ctx.lineTo(11, 2)
                                            ctx.lineTo(12, 4)
                                            ctx.stroke()
                                        }
                                    }
                                }
                                background: Rectangle {
                                    radius: width / 2
                                    color: deleteHistoryEntryButton.pressed ? "#991b1b" : "#dc2626"
                                }
                                onClicked: requestDeleteSelectedHistoryEntry()
                            }
                        }

                    }

                    Item {
                        property int historyScrollbarGap: 5
                        property int historyScrollbarHeight: 12
                        property int historySpacing: 6
                        property int historyArrowSpace: 72
                        property real historyViewportWidth: Math.max(0, width - historyArrowSpace)
                        property bool historyHasOverflow: historyContentWidthForWidth(historyViewportWidth, historySpacing)
                            > historyViewportWidth + 1
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(102, historyDesiredTileWidth(historyListView)
                            + historyScrollbarGap * 2 + historyScrollbarHeight)

                        Label {
                            visible: clipboardHistoryModel.count === 0
                            anchors.centerIn: parent
                            horizontalAlignment: Text.AlignHCenter
                            text: "Kein lokaler Verlauf"
                            color: "#6b7280"
                        }

                        ListView {
                            id: historyListView
                            visible: clipboardHistoryModel.count > 0
                            anchors.left: parent.left
                            anchors.leftMargin: parent.historyHasOverflow ? 36 : 0
                            anchors.right: parent.right
                            anchors.rightMargin: parent.historyHasOverflow ? 36 : 0
                            anchors.top: parent.top
                            height: Math.max(72, parent.height
                                - parent.historyScrollbarGap * 2
                                - parent.historyScrollbarHeight)
                            clip: true
                            currentIndex: currentPreviewHistoryIndex
                            orientation: ListView.Horizontal
                            spacing: parent.historySpacing
                            boundsBehavior: Flickable.StopAtBounds
                            snapMode: ListView.SnapToItem
                            model: clipboardHistoryModel
                            onContentXChanged: clampHistoryContentX(historyListView)

                            delegate: Item {
                                width: historyTileWidth(historyListView)
                                height: width

                                Rectangle {
                                    anchors.fill: parent
                                    color: model.type === "server-unavailable"
                                        ? "#fef2f2"
                                        : (index === 0 ? "#eef2ff" : "transparent")
                                    border.color: model.type === "server-unavailable"
                                        ? "#fca5a5"
                                        : (index === 0 ? "#c7d2fe" : "#e5e7eb")
                                    border.width: 1
                                    radius: 2
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    color: "transparent"
                                    border.color: index === currentPreviewHistoryIndex
                                        && !(serverContentUnavailable && index === 0)
                                        ? (index === 0 ? "#16a34a" : "#111827")
                                        : "transparent"
                                    border.width: index === currentPreviewHistoryIndex
                                        && !(serverContentUnavailable && index === 0)
                                        ? (index === 0 ? 3 : 2)
                                        : 0
                                    radius: 2
                                }

                                Rectangle {
                                    id: historyEntryThumb
                                    anchors.top: model.type === "image" ? undefined : parent.top
                                    anchors.topMargin: model.type === "image" ? 0 : 6
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.verticalCenter: model.type === "image" ? parent.verticalCenter : undefined
                                    width: model.type === "image"
                                        ? Math.min(parent.width - 14, parent.height - 14)
                                        : Math.min(38, parent.width - 6)
                                    height: width
                                    color: model.type === "image"
                                        ? "transparent"
                                        : (model.type === "server-unavailable" ? "#ffffff" : "#f3f4f6")
                                    border.color: model.type === "server-unavailable" ? "#ef4444" : "#d1d5db"
                                    border.width: 1

                                    Image {
                                        visible: model.type === "image"
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        source: visible && model.thumbnail
                                            ? "data:image/png;base64," + model.thumbnail
                                            : ""
                                        sourceSize.width: historyEntryThumb.width
                                        sourceSize.height: historyEntryThumb.height
                                        fillMode: Image.PreserveAspectFit
                                        cache: true
                                        asynchronous: true
                                    }

                                    Label {
                                        visible: model.type !== "image"
                                        anchors.centerIn: parent
                                        text: historyEntryIconLabel(model.type, model.content)
                                        font.pixelSize: 11
                                        font.bold: model.type === "server-unavailable"
                                        color: model.type === "server-unavailable" ? "#b91c1c" : "#6b7280"
                                    }
                                }

                                Text {
                                    visible: model.type !== "image"
                                    anchors.left: parent.left
                                    anchors.leftMargin: 4
                                    anchors.right: parent.right
                                    anchors.rightMargin: 4
                                    anchors.top: historyEntryThumb.bottom
                                    anchors.topMargin: 4
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 4
                                    font.pixelSize: 10
                                    horizontalAlignment: Text.AlignHCenter
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    text: historyTileText(model.type, model.content, index)
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !(serverContentUnavailable && index === 0)
                                    onClicked: function(mouse) {
                                        selectHistoryEntry(index)
                                        mouse.accepted = true
                                    }
                                }

                                Rectangle {
                                    visible: networkClipboard.serverActive
                                        && !serverContentUnavailable
                                        && index === currentPreviewHistoryIndex
                                        && index > 0
                                    anchors.right: parent.right
                                    anchors.rightMargin: 5
                                    anchors.top: parent.top
                                    anchors.topMargin: 5
                                    width: 30
                                    height: 30
                                    radius: 15
                                    color: "#111827"
                                    border.color: "#ffffff"
                                    border.width: 1
                                    z: 2

                                    Label {
                                        anchors.centerIn: parent
                                        text: "\u2713"
                                        color: "#ffffff"
                                        font.pixelSize: 18
                                        font.bold: true
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        preventStealing: true
                                        onClicked: function(mouse) {
                                            setHistoryEntryCurrent(index)
                                            mouse.accepted = true
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            id: historyScrollbarTrack
                            visible: historyListView.visible && parent.historyHasOverflow
                            anchors.left: historyListView.left
                            anchors.right: historyListView.right
                            anchors.top: historyListView.bottom
                            anchors.topMargin: parent.historyScrollbarGap
                            height: parent.historyScrollbarHeight
                            radius: height / 2
                            color: "#e5e7eb"
                            z: 2

                            Rectangle {
                                height: parent.height
                                radius: height / 2
                                color: "#6b7280"
                                width: Math.max(36, parent.width * historyListView.width
                                    / Math.max(historyContentWidth(historyListView), historyListView.width))
                                x: historyMaxContentX(historyListView) <= 0
                                    ? 0
                                    : (parent.width - width) * historyListView.contentX
                                        / historyMaxContentX(historyListView)
                            }
                        }

                        MouseArea {
                            id: historyScrollbarTouchArea
                            property real pressContentX: 0
                            property real pressX: 0
                            visible: historyListView.visible && parent.historyHasOverflow
                            anchors.left: historyListView.left
                            anchors.right: historyListView.right
                            anchors.top: historyListView.bottom
                            anchors.topMargin: 0
                            height: parent.historyScrollbarGap + parent.historyScrollbarHeight + parent.historyScrollbarGap
                            z: 3

                            onPressed: function(mouse) {
                                pressX = mouse.x
                                pressContentX = historyListView.contentX
                                mouse.accepted = true
                            }

                            onPositionChanged: function(mouse) {
                                const maxContentX = historyMaxContentX(historyListView)
                                if (maxContentX <= 0)
                                    return

                                const delta = mouse.x - pressX
                                const target = pressContentX + (delta / Math.max(1, width)) * maxContentX
                                historyListView.contentX = Math.max(0, Math.min(maxContentX, target))
                                mouse.accepted = true
                            }
                        }

                        Rectangle {
                            visible: historyListView.visible
                                     && historyListView.contentX > 1
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: historyListView.bottom
                            width: 34
                            color: "#f9fafb"
                            opacity: 0.92

                            Label {
                                anchors.centerIn: parent
                                text: "<"
                                font.pixelSize: 22
                                color: "#374151"
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: scrollHistoryBy(-1)
                            }
                        }

                        Rectangle {
                            visible: historyListView.visible
                                     && parent.historyHasOverflow
                                     && historyListView.contentX < historyMaxContentX(historyListView) - 1
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: historyListView.bottom
                            width: 34
                            color: "#f9fafb"
                            opacity: 0.92

                            Label {
                                anchors.centerIn: parent
                                text: ">"
                                font.pixelSize: 22
                                color: "#374151"
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: scrollHistoryBy(1)
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

        Label {
            visible: cameraImageLoading
            text: "Bild wird geladen ..."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#6b7280"
        }

        Label {
            visible: cameraApplyError.length > 0
            text: cameraApplyError
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#b91c1c"
        }
    }

    Item {
        id: photoGalleryOverlay
        anchors.fill: parent
        visible: photoGalleryVisible
        z: 120

        Rectangle {
            anchors.fill: parent
            color: "#f9fafb"
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                ToolButton {
                    text: "<"
                    Layout.preferredWidth: 42
                    Layout.preferredHeight: 36
                    contentItem: Text {
                        text: parent.text
                        color: "#111827"
                        font.pixelSize: 22
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 18
                        color: parent.pressed ? "#d1d5db" : "#e5e7eb"
                    }
                    onClicked: photoGalleryVisible = false
                }

                Label {
                    text: "Fotos"
                    font.pixelSize: 22
                    font.bold: true
                    color: "#111827"
                    Layout.fillWidth: true
                }

                Button {
                    text: selectedPhotoApplying ? "Lade ..." : "Übernehmen"
                    enabled: selectedPhotoIndex >= 0 && !selectedPhotoApplying
                    Layout.preferredHeight: 36
                    background: Rectangle {
                        radius: 18
                        color: parent.enabled
                            ? (parent.pressed ? "#15803d" : "#16a34a")
                            : "#d1d5db"
                    }
                    contentItem: Text {
                        text: parent.text
                        color: parent.enabled ? "#ffffff" : "#6b7280"
                        font.pixelSize: 14
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: applySelectedPhoto()
                }
            }

            Label {
                visible: photoGalleryLoading
                Layout.fillWidth: true
                text: "Fotos werden geladen ..."
                color: "#6b7280"
                horizontalAlignment: Text.AlignHCenter
            }

            Label {
                visible: photoApplyError.length > 0
                Layout.fillWidth: true
                text: photoApplyError
                color: "#b91c1c"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Label {
                visible: !photoGalleryLoading && recentPhotos.length === 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                text: "Keine Fotos verfügbar"
                color: "#6b7280"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            GridView {
                visible: !photoGalleryLoading && recentPhotos.length > 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                cellWidth: Math.max(88, Math.floor(width / (root.compactLayout ? 3 : 4)))
                cellHeight: cellWidth
                model: recentPhotos
                delegate: Rectangle {
                    width: GridView.view.cellWidth - 6
                    height: width
                    radius: 4
                    color: selectedPhotoIndex === index ? "#dcfce7" : "#e5e7eb"
                    border.color: selectedPhotoIndex === index ? "#16a34a" : "transparent"
                    border.width: selectedPhotoIndex === index ? 4 : 0
                    clip: true

                    Image {
                        anchors.fill: parent
                        anchors.margins: selectedPhotoIndex === index ? 4 : 0
                        source: modelData.thumbnail ? "data:image/png;base64," + modelData.thumbnail : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                    }

                    Rectangle {
                        visible: selectedPhotoIndex === index
                        width: 26
                        height: 26
                        radius: 13
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 6
                        color: "#16a34a"

                        Text {
                            anchors.centerIn: parent
                            text: "✓"
                            color: "#ffffff"
                            font.pixelSize: 17
                            font.bold: true
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: selectedPhotoIndex = index
                    }
                }
            }
        }
    }

    Item {
        id: addContentOverlay
        anchors.fill: parent
        visible: addContentActionsVisible
        z: 100
        property point buttonPos: visible ? addServerButton.mapToItem(addContentOverlay, 0, 0) : Qt.point(0, 0)

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.14
        }

        MouseArea {
            anchors.fill: parent
            onClicked: addContentActionsVisible = false
        }

        Rectangle {
            id: addContentActionPanel
            width: 190
            height: 94
            radius: 18
            color: "#ffffff"
            border.color: "#d1d5db"
            border.width: 1
            x: Math.max(12, Math.min(addContentOverlay.width - width - 12,
                addContentOverlay.buttonPos.x + addServerButton.width - width))
            y: Math.max(12, Math.min(addContentOverlay.height - height - 12,
                addContentOverlay.buttonPos.y + addServerButton.height + 8))

            MouseArea {
                anchors.fill: parent
                onClicked: mouse.accepted = true
            }

            RowLayout {
                anchors.centerIn: parent
                spacing: 18

                ToolButton {
                    Layout.preferredWidth: 82
                    Layout.preferredHeight: 76
                    padding: 0
                    contentItem: ColumnLayout {
                        spacing: 5
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: "\u25a7"
                            color: "#ffffff"
                            font.pixelSize: 24
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter

                            Rectangle {
                                anchors.centerIn: parent
                                width: 46
                                height: 46
                                radius: 23
                                color: "#7c3aed"
                                z: -1
                            }
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "Fotos"
                            font.pixelSize: 12
                            color: "#111827"
                        }
                    }
                    background: Rectangle {
                        radius: 10
                        color: parent.pressed ? "#f3f4f6" : "transparent"
                    }
                    onClicked: {
                        addContentActionsVisible = false
                        debugLog("photos action clicked")
                        openPhotoGallery()
                    }
                }

                ToolButton {
                    Layout.preferredWidth: 82
                    Layout.preferredHeight: 76
                    padding: 0
                    contentItem: ColumnLayout {
                        spacing: 5
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: "\u25c9"
                            color: "#ffffff"
                            font.pixelSize: 24
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter

                            Rectangle {
                                anchors.centerIn: parent
                                width: 46
                                height: 46
                                radius: 23
                                color: "#ec4899"
                                z: -1
                            }
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "Kamera"
                            font.pixelSize: 12
                            color: "#111827"
                        }
                    }
                    background: Rectangle {
                        radius: 10
                        color: parent.pressed ? "#f3f4f6" : "transparent"
                    }
                    onClicked: {
                        addContentActionsVisible = false
                        debugLog("camera action clicked")
                        openCameraCapture()
                    }
                }
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: cameraImageLoading
        z: 110

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.18
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 240)
            height: 82
            radius: 12
            color: "#ffffff"
            border.color: "#d1d5db"

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 8

                BusyIndicator {
                    running: cameraImageLoading
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                }

                Label {
                    text: "Bild wird geladen ..."
                    color: "#111827"
                    font.pixelSize: 14
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    Layout.alignment: Qt.AlignHCenter
                }
            }
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

    Dialog {
        id: deleteHistoryEntryDialog
        title: "Eintrag löschen"
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        anchors.centerIn: parent
        onAccepted: deleteHistoryEntry(pendingDeleteHistoryIndex)
        onRejected: pendingDeleteHistoryIndex = -1

        Label {
            width: Math.min(deleteHistoryEntryDialog.availableWidth, 340)
            wrapMode: Text.WordWrap
            text: "Soll dieser Eintrag wirklich aus dem Verlauf gelöscht werden?"
        }
    }
}
