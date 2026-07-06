#include "ClipboardContentWindow.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSet>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString entryDedupeKey(const ClipboardEntry &entry)
{
    if (entry.type == QStringLiteral("image")) {
        const QByteArray imageData = QByteArray::fromBase64(entry.content.toLatin1());
        const QImage image = QImage::fromData(imageData, "PNG");
        if (!image.isNull()) {
            const QImage normalized = image.convertToFormat(QImage::Format_RGBA8888);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(QByteArray::number(normalized.width()));
            hash.addData("x");
            hash.addData(QByteArray::number(normalized.height()));
            for (int row = 0; row < normalized.height(); ++row) {
                hash.addData(QByteArrayView(
                    reinterpret_cast<const char *>(normalized.constScanLine(row)),
                    normalized.width() * 4));
            }
            return QStringLiteral("image\n") + QString::fromLatin1(hash.result().toHex());
        }

        return QStringLiteral("image\n")
            + QString::fromLatin1(QCryptographicHash::hash(imageData, QCryptographicHash::Sha256).toHex());
    }

    QString content = entry.content;
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    content.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return QStringLiteral("text\n") + content.trimmed();
}
}

ClipboardContentWindow::ClipboardContentWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Network Clipboard Content"));
    resize(760, 480);

    m_historyList = new QListWidget(this);
    m_historyList->setMaximumWidth(240);
    m_historyList->setMinimumWidth(190);
    connect(m_historyList, &QListWidget::currentRowChanged, this, &ClipboardContentWindow::selectEntry);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setText(QStringLiteral("Inhalt Netz-Zwischenablage"));
    m_titleLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));

    m_stateLabel = new QLabel(this);
    m_stateLabel->setStyleSheet(QStringLiteral("color: #666;"));

    m_textBrowser = new QTextBrowser(this);
    m_textBrowser->setOpenExternalLinks(false);
    m_textBrowser->setReadOnly(true);
    connect(m_textBrowser, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        QDesktopServices::openUrl(url);
    });

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setMinimumSize(240, 180);
    m_imageLabel->hide();

    m_makeCurrentButton = new QPushButton(QStringLiteral("Make Current"), this);
    m_makeCurrentButton->hide();
    connect(m_makeCurrentButton, &QPushButton::clicked, this, [this]() {
        if (m_selectedIndex > 0 && m_selectedIndex < m_entries.size())
            emit makeCurrentRequested(m_entries.at(m_selectedIndex));
    });

    auto *contentLayout = new QVBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(8);
    contentLayout->addWidget(m_titleLabel);
    contentLayout->addWidget(m_stateLabel);
    contentLayout->addWidget(m_textBrowser, 1);
    contentLayout->addWidget(m_imageLabel, 1);
    contentLayout->addWidget(m_makeCurrentButton, 0, Qt::AlignRight);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addWidget(m_historyList);
    layout->addLayout(contentLayout, 1);
}

void ClipboardContentWindow::setLoading()
{
    m_image = {};
    m_titleLabel->setText(QStringLiteral("Inhalt Netz-Zwischenablage"));
    m_stateLabel->clear();
    m_makeCurrentButton->hide();
    m_imageLabel->hide();
    m_textBrowser->show();
    m_textBrowser->setPlainText(QStringLiteral("Loading..."));
}

void ClipboardContentWindow::setMessage(const QString &message)
{
    m_image = {};
    m_titleLabel->setText(QStringLiteral("Inhalt Netz-Zwischenablage"));
    m_stateLabel->clear();
    m_makeCurrentButton->hide();
    m_imageLabel->hide();
    m_textBrowser->show();
    m_textBrowser->setPlainText(message);
}

void ClipboardContentWindow::setEntry(const ClipboardEntry &entry)
{
    if (entry.type == QStringLiteral("image")) {
        const QImage image = QImage::fromData(QByteArray::fromBase64(entry.content.toLatin1()), "PNG");
        if (image.isNull()) {
            setMessage(QStringLiteral("Network clipboard image is invalid."));
            return;
        }
        setImageContent(image);
        return;
    }

    setTextContent(entry.content);
}

void ClipboardContentWindow::setEntries(const QList<ClipboardEntry> &entries)
{
    m_entries.clear();
    QSet<QString> seen;
    for (const ClipboardEntry &entry : entries) {
        const QString key = entryDedupeKey(entry);
        if (seen.contains(key))
            continue;

        seen.insert(key);
        m_entries.append(entry);
        if (m_entries.size() >= 15)
            break;
    }

    m_historyList->clear();

    for (int index = 0; index < m_entries.size(); ++index)
        m_historyList->addItem(entryLabel(m_entries.at(index), index));

    if (m_entries.isEmpty()) {
        m_selectedIndex = -1;
        setMessage(QStringLiteral("No network clipboard entry available."));
        return;
    }

    m_historyList->setCurrentRow(0);
    selectEntry(0);
}

void ClipboardContentWindow::setTextContent(const QString &text)
{
    m_image = {};
    m_titleLabel->setText(QStringLiteral("Inhalt Netz-Zwischenablage"));
    m_imageLabel->hide();
    m_textBrowser->show();
    m_textBrowser->setHtml(richText(text));
}

void ClipboardContentWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateImagePixmap();
}

void ClipboardContentWindow::setImageContent(const QImage &image)
{
    m_image = image;
    m_titleLabel->setText(QStringLiteral("Bild Netz-Zwischenablage"));
    m_textBrowser->hide();
    m_imageLabel->show();
    updateImagePixmap();
}

void ClipboardContentWindow::selectEntry(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;

    m_selectedIndex = index;
    m_stateLabel->setText(index == 0 ? QStringLiteral("Aktueller Eintrag")
                                     : QStringLiteral("Alter Eintrag"));
    m_makeCurrentButton->setVisible(index > 0);
    m_makeCurrentButton->setEnabled(index > 0);
    setEntry(m_entries.at(index));
}

void ClipboardContentWindow::updateImagePixmap()
{
    if (m_image.isNull() || !m_imageLabel->isVisible())
        return;

    m_imageLabel->setPixmap(QPixmap::fromImage(m_image).scaled(
        m_imageLabel->size(),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
}

QString ClipboardContentWindow::richText(const QString &text)
{
    QString html = text.toHtmlEscaped();
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));

    static const QRegularExpression urlPattern(QStringLiteral(R"((https?://[^\s<]+))"));
    html.replace(urlPattern, QStringLiteral(R"(<a href="\1">\1</a>)"));
    return QStringLiteral("<div style=\"white-space: normal;\">%1</div>").arg(html);
}

QString ClipboardContentWindow::entryLabel(const ClipboardEntry &entry, int index)
{
    const QString prefix = index == 0 ? QStringLiteral("Aktuell") : QStringLiteral("Alt");
    const QString type = entry.type == QStringLiteral("image") ? QStringLiteral("Bild") : QStringLiteral("Text");
    const QString time = entry.timestamp > 0
        ? QDateTime::fromSecsSinceEpoch(entry.timestamp).toString(QStringLiteral("dd.MM. hh:mm"))
        : QStringLiteral("--");
    QString preview = entry.type == QStringLiteral("image") ? entry.deviceName : entry.content.simplified();
    if (preview.size() > 42)
        preview = preview.left(39) + QStringLiteral("...");
    if (preview.isEmpty())
        preview = entry.deviceName;
    return QStringLiteral("%1 - %2 - %3\n%4").arg(prefix, type, time, preview);
}
