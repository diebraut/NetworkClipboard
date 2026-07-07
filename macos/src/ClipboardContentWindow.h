#pragma once

#include "ClipboardEntry.h"

#include <QImage>
#include <QList>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QTextBrowser;

class ClipboardContentWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ClipboardContentWindow(QWidget *parent = nullptr);

    void setLoading();
    void setMessage(const QString &message);
    void setEntry(const ClipboardEntry &entry);
    void setEntries(const QList<ClipboardEntry> &entries);
    void setTextContent(const QString &text);
    void setImageContent(const QImage &image);

signals:
    void makeCurrentRequested(const ClipboardEntry &entry);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void selectEntry(int index);
    void updateImagePixmap();
    static QString entryLabel(const ClipboardEntry &entry, int index);
    static QString richText(const QString &text);

    QListWidget *m_historyList = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_stateLabel = nullptr;
    QTextBrowser *m_textBrowser = nullptr;
    QLabel *m_imageLabel = nullptr;
    QPushButton *m_makeCurrentButton = nullptr;
    QList<ClipboardEntry> m_entries;
    int m_selectedIndex = -1;
    QImage m_image;
};
