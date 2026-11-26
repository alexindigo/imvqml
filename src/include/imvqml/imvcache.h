// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>

namespace Imv {

class ImvCache : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QUrl directory READ directory WRITE setDirectory NOTIFY policyChanged FINAL)
    Q_PROPERTY(qint64 byteBudget READ byteBudget WRITE setByteBudget NOTIFY policyChanged FINAL)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY policyChanged FINAL)

public:
    explicit ImvCache(QObject *parent = nullptr);
    ~ImvCache() override;
    QUrl directory() const;
    void setDirectory(const QUrl &directory);
    qint64 byteBudget() const;
    void setByteBudget(qint64 bytes);
    bool enabled() const;
    void setEnabled(bool enabled);

    Q_INVOKABLE int clear();
    Q_INVOKABLE int invalidate(const QUrl &source);
    Q_INVOKABLE int exportThumbnail(const QUrl &source, int size = 128);

signals:
    void policyChanged();
    void cleared(int ticket, const QVariantMap &result);
    void thumbnailFinished(int id, const QVariantMap &result);

private:
    struct State;
    std::unique_ptr<State> d;
    int evict(const QUrl *source);
    void finish(int id, const QVariantMap &result);
};
} // namespace Imv
