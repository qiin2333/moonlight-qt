#pragma once

#include <atomic>
#include <memory>

#include <QMutex>
#include <QImage>
#include <QPoint>
#include <QRasterWindow>
#include <QThread>
#include <QVariantList>
#include <QVector>

#include "backend/nvcomputer.h"

class FileMappingProtocolAdapter;

class FileTransferWorker : public QObject
{
    Q_OBJECT

public:
    explicit FileTransferWorker(NvComputer computer);
    ~FileTransferWorker() override;

    void requestCancel();

public slots:
    void initialize();
    void browseRemote(const QString& mappingId, const QString& path);
    void upload(const QString& localPath,
                const QString& mappingId,
                const QString& remoteDirectory);
    void download(const QString& mappingId,
                  const QString& remotePath,
                  bool directory,
                  const QString& localDirectory);

signals:
    void remoteReady(const QVariantList& mappings, const QString& error);
    void remoteListed(const QString& mappingId,
                      const QString& path,
                      const QVariantList& entries,
                      const QString& error);
    void transferProgress(const QString& message, quint64 completed, quint64 total);
    void transferFinished(bool ok, const QString& message);

private:
    bool ensureConnected(QString& error);
    bool uploadItem(const QString& localPath,
                    const QString& mappingId,
                    const QString& remotePath,
                    QString& error);
    bool uploadFile(const QString& localPath,
                    const QString& mappingId,
                    const QString& remotePath,
                    QString& error);
    bool downloadItem(const QString& mappingId,
                      const QString& remotePath,
                      bool directory,
                      const QString& localPath,
                      QString& error);
    bool downloadFile(const QString& mappingId,
                      const QString& remotePath,
                      const QString& localPath,
                      QString& error);

    NvComputer m_Computer;
    std::unique_ptr<FileMappingProtocolAdapter> m_Client;
    std::atomic_bool m_Cancelled { false };
};

class FileTransferWindow : public QRasterWindow
{
    Q_OBJECT

public:
    explicit FileTransferWindow(NvComputer computer);
    ~FileTransferWindow() override;

    void showAndActivate();

signals:
    void initializeWorker();
    void requestRemoteList(const QString& mappingId, const QString& path);
    void requestUpload(const QString& localPath,
                       const QString& mappingId,
                       const QString& remoteDirectory);
    void requestDownload(const QString& mappingId,
                         const QString& remotePath,
                         bool directory,
                         const QString& localDirectory);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onRemoteReady(const QVariantList& mappings, const QString& error);
    void onRemoteListed(const QString& mappingId,
                        const QString& path,
                        const QVariantList& entries,
                        const QString& error);
    void onTransferProgress(const QString& message, quint64 completed, quint64 total);
    void onTransferFinished(bool ok, const QString& message);

private:
    struct Entry {
        QString name;
        QString path;
        QString mappingId;
        bool directory = false;
        bool drive = false;
        bool writable = false;
        quint64 size = 0;
        QImage icon;
    };

    QRect localPaneRect() const;
    QRect remotePaneRect() const;
    QRect localPathRect() const;
    QRect remotePathRect() const;
    QRect uploadButtonRect() const;
    QRect downloadButtonRect() const;
    QRect refreshButtonRect() const;
    QRect rowRect(bool local, int visibleRow) const;
    int rowAt(bool local, const QPoint& point) const;
    int visibleRowCount() const;

    void loadLocalRoot();
    void browseLocal(const QString& path);
    void refreshLocal();
    void refreshRemote();
    void openLocalSelection();
    void openRemoteSelection();
    void localUp();
    void remoteUp();
    void beginUpload();
    void beginDownload();
    bool resolveRemoteDropTarget(const QPoint& point,
                                 QString& mappingId,
                                 QString& remoteDirectory,
                                 QString& displayPath) const;
    bool resolveLocalDropTarget(const QPoint& point,
                                QString& localDirectory) const;
    void updateDrag(const QPoint& point);
    void finishDrag(const QPoint& point);
    void resetDrag();
    void clampScrollOffsets();
    void setStatus(const QString& status, bool error = false);

    QThread m_WorkerThread;
    FileTransferWorker* m_Worker = nullptr;
    QVector<Entry> m_LocalEntries;
    QVector<Entry> m_RemoteEntries;
    QVector<Entry> m_RemoteRoots;
    QString m_LocalPath;
    QString m_RemoteMappingId;
    QString m_RemoteMappingName;
    QString m_RemotePath;
    QString m_Status;
    QString m_DragHint;
    bool m_StatusError = false;
    bool m_Busy = true;
    bool m_RemoteWritable = false;
    bool m_ProgressVisible = false;
    int m_ProgressPercent = 0;
    int m_LocalSelection = -1;
    int m_RemoteSelection = -1;
    int m_LocalScroll = 0;
    int m_RemoteScroll = 0;
    bool m_DragSourceLocal = true;
    bool m_DragActive = false;
    bool m_DragTargetValid = false;
    int m_DragSourceIndex = -1;
    QPoint m_DragStart;
    QPoint m_DragPosition;
};
