// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>

namespace Imv {

class ImvImageJob : public QObject {
    Q_OBJECT
    QML_ELEMENT
public:
    explicit ImvImageJob(QObject *parent = nullptr);
    ~ImvImageJob() override;

    Q_INVOKABLE int sample(const QUrl &source, int maxWidth, int maxHeight,
                           double videoPosition = 0);
    Q_INVOKABLE int inspect(const QUrl &source);
    Q_INVOKABLE int render(const QUrl &source, int maxWidth, int maxHeight, const QUrl &destination,
                           const QString &format = QStringLiteral("png"), double videoPosition = 0);
    Q_INVOKABLE int retainOriginal(const QUrl &source, const QUrl &destinationDirectory);
    Q_INVOKABLE bool cancel(int id);

signals:
    void finished(int id, const QVariantMap &result);

private:
    struct State;
    std::unique_ptr<State> d;
};

} // namespace Imv
