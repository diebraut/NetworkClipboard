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
    property double localClipboardSyncPausedUntil: 0
    readonly property bool compactLayout: width < 520

    function deviceName() {
        return Qt.platform.os === "ios" ? "iPhone" : "Android"
    }

    function syncClipboardToPreview() {
        if (Date.now() < localClipboardSyncPausedUntil) {
            clipboardSyncRetries = 0
            return
        }

        if (isBrowsingHistoryEntry()) {
            clipboardSyncRetries = 0
            return
        }

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
        if (Date.now() < localClipboardSyncPausedUntil)
            return

        if (isBrowsingHistoryEntry())
            return

        clipboardSyncRetries = Qt.platform.os === "ios" ? 20 : 12
        clipboardSyncTimer.interval = Qt.platform.os === "ios" ? 700 : 350
        clipboardSyncTimer.restart()
    }

    function isBrowsingHistoryEntry() {
        return currentPreviewHistoryIndex > 0
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

    function showPreviewImage(base64) {
        rawPreviewText = ""
        if (rawPreviewImageBase64 === base64 && rawPreviewImageBase64.length > 0)
            return

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

    function historyKey(type, content) {
        return type + ":" + content
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

        if (entry.type === "image") {
            const imageData = imageDataForHistoryEntry(entry)
            if (imageData.length > 0) {
                const fingerprint = localClipboard.imageFingerprintFromBase64(imageData)
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
        const containerWidth = clipboardBox && clipboardBox.width > 0
            ? clipboardBox.width
            : (view ? view.width : 0)
        const widthFromGroupBox = Math.round(containerWidth * 0.07)
        return Math.max(64, widthFromGroupBox)
    }

    function historyVisibleTileCount(view) {
        if (!view || view.width <= 0)
            return 1

        return Math.max(1, Math.floor((view.width + view.spacing)
            / (historyDesiredTileWidth(view) + view.spacing)))
    }

    function historyTileWidth(view) {
        const count = historyVisibleTileCount(view)
        if (!view || view.width <= 0)
            return 64

        return Math.max(64, Math.floor((view.width - view.spacing * (count - 1)) / count))
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
            if (!isValidHistoryEntry(entry))
                continue
            const dedupeKey = historyDedupeKey(entry)
            if (dedupeKey.length === 0 || seen[dedupeKey])
                continue
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
        let changed = false

        for (let i = 0; i < clipboardHistoryModel.count; ++i) {
            const entry = clipboardHistoryModel.get(i)
            if (!isValidHistoryEntry(entry)) {
                changed = true
                continue
            }

            const dedupeKey = historyDedupeKey(entry)
            if (dedupeKey.length === 0 || seen[dedupeKey]) {
                changed = true
                continue
            }

            seen[dedupeKey] = true
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
        currentPreviewHistoryIndex = selectedKey.length > 0 ? findHistoryIndex(selectedKey) : -1
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

        rawPreviewText = entry.content
        rawPreviewImageBase64 = ""
        pendingPreviewImageBase64 = ""
        previewImageLoading = false
        rawPreviewImageSource = ""
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
            showRestoredHistoryEntry(0)
            saveClipboardHistory()
        }
    }

    function addHistoryEntry(type, content) {
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
        for (let i = 0; i < clipboardHistoryModel.count; ++i) {
            const existing = clipboardHistoryModel.get(i)
            const sameImage = type === "image"
                && existing.type === "image"
                && (existing.imageId === imageId || historyDedupeKey(existing) === key)
            if (existing.key === key || sameImage) {
                if (type === "image") {
                    if ((!existing.imageId || existing.imageId.length === 0) && imageId.length > 0)
                        clipboardHistoryModel.setProperty(i, "imageId", imageId)
                    if (!existing.thumbnail || existing.thumbnail.length === 0)
                        clipboardHistoryModel.setProperty(i, "thumbnail", thumbnail)
                }
                clipboardHistoryModel.move(i, 0, 1)
                cleanClipboardHistoryModel()
                saveClipboardHistory()
                return 0
            }
        }

        clipboardHistoryModel.insert(0, {
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
        return 0
    }

    function historyEntryTypeLabel(type, content) {
        if (type === "image")
            return "Bild"
        if (content.toLowerCase().startsWith("http://") || content.toLowerCase().startsWith("https://"))
            return "URL"
        return "Text"
    }

    function historyEntryIconLabel(type, content) {
        if (type === "image")
            return ""
        return historyEntryTypeLabel(type, content) === "URL" ? "URL" : "TXT"
    }

    function historyEntryPreview(type, content, createdAt) {
        if (type === "image")
            return "<span style=\"font-weight:600;\">Bild</span><br><span style=\"color:#6b7280;\">" + createdAt + "</span>"

        let preview = content.replace(/\s+/g, " ").trim()
        if (preview.length > 42)
            preview = preview.slice(0, 39) + "..."
        return "<span style=\"font-weight:600;\">" + historyEntryTypeLabel(type, content)
            + "</span><br><span style=\"color:#6b7280;\">" + escapeHtml(preview) + "</span>"
    }

    function historyTileText(type, content, index) {
        if (type === "image")
            return (index === 0 ? "Aktuell" : "Alt") + "\nBild"

        let preview = content.replace(/\s+/g, " ").trim()
        if (preview.length === 0)
            preview = historyEntryTypeLabel(type, content)
        if (preview.length > 22)
            preview = preview.slice(0, 21) + "..."
        return preview
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
        currentPreviewHistoryIndex = -1
    }

    function showLocalPreviewText(text) {
        rawPreviewText = text
        rawPreviewImageBase64 = ""
        rawPreviewImageSource = ""
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryIndex = networkClipboard.serverActive ? addHistoryEntry("text", text) : -1
        const normalizedText = normalizedHistoryContent("text", text)
        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0
            ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key
            : (normalizedText.length > 0 ? historyKey("text", normalizedText) : "")
    }

    function showLocalPreviewImage(base64, fingerprint) {
        currentPreviewFromLocalClipboard = true
        currentPreviewHistoryIndex = networkClipboard.serverActive ? addHistoryEntry("image", base64) : -1
        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0 ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key : historyKey("image", fingerprint)
        showPreviewImage(currentPreviewHistoryIndex >= 0
            ? displayImageForHistoryEntry(clipboardHistoryModel.get(currentPreviewHistoryIndex))
            : previewFromFullImage(base64))
    }

    function showNetworkPreviewText(text) {
        rawPreviewText = text
        rawPreviewImageBase64 = ""
        rawPreviewImageSource = ""
        currentPreviewFromLocalClipboard = false
        currentPreviewHistoryIndex = addHistoryEntry("text", text)
        const normalizedText = normalizedHistoryContent("text", text)
        currentPreviewHistoryKey = currentPreviewHistoryIndex >= 0
            ? clipboardHistoryModel.get(currentPreviewHistoryIndex).key
            : (normalizedText.length > 0 ? historyKey("text", normalizedText) : "")
    }

    function showNetworkPreviewImage(base64) {
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

        rawPreviewText = entry.content
        rawPreviewImageBase64 = ""
        pendingPreviewImageBase64 = ""
        previewImageLoading = false
        rawPreviewImageSource = ""
    }

    function selectedHistoryIndex() {
        return currentPreviewHistoryIndex
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
        if (!force && Date.now() < localClipboardSyncPausedUntil)
            return

        if (!force && isBrowsingHistoryEntry())
            return

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
        restoreClipboardHistory()
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
            if (!forceUpdate && isBrowsingHistoryEntry()) {
                addHistoryEntry("text", text)
                currentPreviewHistoryIndex = findHistoryIndex(currentPreviewHistoryKey)
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
            const forceUpdate = forceNextNetworkText
            if (!forceUpdate && isBrowsingHistoryEntry()) {
                addHistoryEntry("image", base64)
                currentPreviewHistoryIndex = findHistoryIndex(currentPreviewHistoryKey)
                return
            }

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
                            text: clipboardHistoryModel.count > 0
                                ? (currentPreviewHistoryKey.length > 0 && clipboardHistoryModel.count > 0
                                    && clipboardHistoryModel.get(0).key === currentPreviewHistoryKey
                                    ? "Aktueller Eintrag"
                                    : "Alter Eintrag")
                                : ""
                            color: "#6b7280"
                            Layout.fillWidth: true
                            visible: text.length > 0
                        }

                        ScrollView {
                            visible: rawPreviewImageBase64.length === 0 && !previewImageLoading
                            Layout.fillWidth: true
                            Layout.fillHeight: true

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
                            Layout.fillWidth: true
                            Layout.fillHeight: true
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

                        BusyIndicator {
                            visible: previewImageLoading
                            running: visible
                            Layout.alignment: Qt.AlignHCenter
                        }

                    }

                    Item {
                        property int historyScrollbarGap: 5
                        property int historyScrollbarHeight: 12
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
                            anchors.leftMargin: historyCanScroll(historyListView) ? 36 : 0
                            anchors.right: parent.right
                            anchors.rightMargin: historyCanScroll(historyListView) ? 36 : 0
                            anchors.top: parent.top
                            height: Math.max(72, parent.height
                                - parent.historyScrollbarGap * 2
                                - parent.historyScrollbarHeight)
                            clip: true
                            currentIndex: currentPreviewHistoryIndex
                            orientation: ListView.Horizontal
                            spacing: 6
                            boundsBehavior: Flickable.StopAtBounds
                            snapMode: ListView.SnapToItem
                            model: clipboardHistoryModel
                            onContentXChanged: clampHistoryContentX(historyListView)

                            delegate: Item {
                                width: historyTileWidth(historyListView)
                                height: width

                                Rectangle {
                                    anchors.fill: parent
                                    color: index === 0 ? "#eef2ff" : "transparent"
                                    border.color: index === 0 ? "#c7d2fe" : "#e5e7eb"
                                    border.width: 1
                                    radius: 2
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    color: "transparent"
                                    border.color: model.key === currentPreviewHistoryKey
                                        ? (index === 0 ? "#16a34a" : "#111827")
                                        : "transparent"
                                    border.width: model.key === currentPreviewHistoryKey
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
                                    color: model.type === "image" ? "transparent" : "#f3f4f6"
                                    border.color: "#d1d5db"
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
                                        color: "#6b7280"
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
                                    onClicked: function(mouse) {
                                        selectHistoryEntry(index)
                                        mouse.accepted = true
                                    }
                                }

                                Rectangle {
                                    visible: index === currentPreviewHistoryIndex && index > 0
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
                            visible: historyListView.visible && historyCanScroll(historyListView)
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
                            visible: historyListView.visible && historyCanScroll(historyListView)
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
                                     && historyListView.contentWidth > historyListView.width + 1
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
