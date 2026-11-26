// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QQuickItem>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>

namespace Imv {

class ImvImage : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged FINAL)
    Q_PROPERTY(Mask mask READ mask WRITE setMask NOTIFY maskChanged FINAL)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged FINAL)
    Q_PROPERTY(bool animate READ animate WRITE setAnimate NOTIFY animateChanged FINAL)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged FINAL)
    Q_PROPERTY(double videoPosition READ videoPosition WRITE setVideoPosition NOTIFY
                   videoPositionChanged FINAL)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString errorStage READ errorStage NOTIFY errorChanged FINAL)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorChanged FINAL)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY errorChanged FINAL)
    Q_PROPERTY(QVariantMap sourceInfo READ sourceInfo NOTIFY sourceInfoChanged FINAL)

public:
    enum FillMode { Contain, Cover, Stretch };
    Q_ENUM(FillMode)
    enum Mask { NoMask, Circle, Rounded };
    Q_ENUM(Mask)
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)

    explicit ImvImage(QQuickItem *parent = nullptr);
    ~ImvImage() override;

    QUrl source() const;
    void setSource(const QUrl &source);
    Q_INVOKABLE void setBytes(const QByteArray &bytes);
    FillMode fillMode() const;
    void setFillMode(FillMode mode);
    Mask mask() const;
    void setMask(Mask mask);
    qreal radius() const;
    void setRadius(qreal radius);
    bool animate() const;
    void setAnimate(bool animate);
    bool playing() const;
    void setPlaying(bool playing);
    double videoPosition() const;
    void setVideoPosition(double position);
    Status status() const;
    QString errorStage() const;
    QString errorCode() const;
    QString errorDetail() const;
    QVariantMap sourceInfo() const;

signals:
    void sourceChanged();
    void fillModeChanged();
    void maskChanged();
    void radiusChanged();
    void animateChanged();
    void playingChanged();
    void videoPositionChanged();
    void statusChanged();
    void errorChanged();
    void sourceInfoChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &now, const QRectF &before) override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *) override;

private:
    struct State;
    std::unique_ptr<State> d;
    void resetRequest(bool discard);
    void syncPlayback();
    void setOutcome(Status status, QString stage = {}, QString code = {}, QString detail = {});
};

} // namespace Imv
