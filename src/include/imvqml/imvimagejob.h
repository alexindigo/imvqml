// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

namespace Imv {

class ImvImageJob : public QObject {
    Q_OBJECT
    QML_ELEMENT
public:
    explicit ImvImageJob(QObject *parent = nullptr);

    Q_INVOKABLE int sample(const QUrl &source, int maxWidth, int maxHeight);

signals:
    void finished(int id, const QVariantMap &result);

private:
    int m_lastId = 0;
};

} // namespace Imv
