// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "image-pipeline.h"
#include <QTimer>

namespace Imv {

class Playback : public QObject {
public:
    static constexpr int Ahead = 2;
    static constexpr int GraceMs = 400;
    Playback(Asset asset, ImagePipeline::Done deliver);
    void run(bool enabled);
    void rewind();
    int buffered() const { return m_ring.size(); }
    qint64 bufferedBytes() const;
    bool hasHelper() const { return bool(m_helper); }

private:
    struct Frame {
        int index;
        qint64 cycle;
        QImage pixels;
        int delay;
    };
    void tick();
    void fill();
    void retire();
    bool next(int &index, qint64 &cycle) const;

    Asset m_base;
    ImagePipeline::Done m_deliver;
    QTimer m_clock, m_grace;
    std::unique_ptr<Pool> m_helper;
    QList<Frame> m_ring;
    int m_index = 0;
    qint64 m_cycle = 0;
    int m_remaining = 0;
    bool m_running = false;
    bool m_waiting = false;
    qint64 m_inFlight = 0;
    bool m_complete = false;
};

} // namespace Imv
