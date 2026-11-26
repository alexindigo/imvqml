// SPDX-License-Identifier: LGPL-3.0-only
#include "playback.h"
#include "helper.h"

namespace Imv {

Playback::Playback(Asset asset, ImagePipeline::Done deliver)
    : m_base(std::move(asset)), m_deliver(std::move(deliver)),
      m_remaining(m_base->stream ? m_base->delay : m_base->animation.delay(0)) {
    m_clock.setSingleShot(true);
    m_clock.setTimerType(Qt::PreciseTimer);
    m_grace.setSingleShot(true);
    connect(&m_clock, &QTimer::timeout, this, &Playback::tick);
    connect(&m_grace, &QTimer::timeout, this, &Playback::retire);
}

void Playback::run(bool enabled) {
    if (m_complete || enabled == m_running)
        return;
    m_running = enabled;
    if (!enabled) {
        if (m_clock.isActive()) {
            m_remaining = qMax(0, m_clock.remainingTime());
            m_clock.stop();
        }
        if (m_helper)
            m_grace.start(GraceMs);
        return;
    }
    m_grace.stop();
    m_waiting = false;
    m_clock.start(m_remaining);
    QMetaObject::invokeMethod(this, &Playback::fill, Qt::QueuedConnection);
}

void Playback::retire() {
    m_helper.reset();
    m_ring.clear();
    m_inFlight = false;
}

void Playback::rewind() {
    m_clock.stop();
    // Disabling animation also pauses it; honor the helper grace if one exists.
    run(false);
    m_index = 0;
    m_cycle = 0;
    m_waiting = false;
    m_complete = false;
    m_remaining = m_base->stream ? m_base->delay : m_base->animation.delay(0);
    if (m_helper)
        retire(); // A reposition cannot reuse pending pages for the old playhead.
    const auto deliver = m_deliver;
    deliver(m_base, {});
}

bool Playback::next(int &index, qint64 &cycle) const {
    if (++index < m_base->info.frames)
        return true;
    index = 0;
    if (m_base->info.loop > 0)
        return ++cycle < m_base->info.loop;
    return true;
}

void Playback::tick() {
    if (!m_running)
        return;
    int index = m_index;
    qint64 cycle = m_cycle;
    if (!next(index, cycle)) {
        run(false);
        m_complete = true;
        return;
    }
    Asset frame;
    if (m_base->stream) {
        if (m_ring.isEmpty()) {
            m_remaining = 0;
            m_waiting = true;
            fill();
            return;
        }
        const auto ready = m_ring.takeFirst();
        Q_ASSERT(ready.index == index && ready.cycle == cycle);
        m_remaining = ready.delay;
        frame = std::make_shared<const ImageAsset>(
            ImageAsset{m_base->key + "/frame/" + QByteArray::number(index),
                       m_base->info,
                       ready.pixels,
                       {},
                       {},
                       {},
                       0,
                       true});
    } else {
        m_remaining = m_base->animation.delay(index);
        frame = index == 0 ? m_base
                           : std::make_shared<const ImageAsset>(
                                 ImageAsset{m_base->key + "/frame/" + QByteArray::number(index),
                                            m_base->info, m_base->animation.frame(index)});
    }
    m_index = index;
    m_cycle = cycle;
    m_waiting = false;
    m_clock.start(m_remaining);
    const QPointer guard(this);
    const auto deliver = m_deliver;
    deliver(std::move(frame), {});
    if (guard)
        fill();
}

void Playback::fill() {
    if (!m_running || !m_base->stream || m_inFlight || m_ring.size() >= Ahead)
        return;
    int index = m_ring.isEmpty() ? m_index : m_ring.back().index;
    qint64 cycle = m_ring.isEmpty() ? m_cycle : m_ring.back().cycle;
    if (!next(index, cycle))
        return;
    if (!m_helper) {
        m_helper = std::make_unique<Pool>([] { return processChild(decoderPath()); }, 1, 32);
        connect(m_helper.get(), &Pool::capacityReleased, this, &Playback::fill);
    }
    if (!m_helper->available())
        return;
    auto spec = m_base->spec;
    spec.mode = DecodeSpec::FrameAt;
    spec.page = index;
    const auto ticket = submitDecode(
        *m_helper, duplicateSource(*m_base->stream), spec, [this, index, cycle](Decoded value) {
            m_inFlight = false;
            if (!value.error && (value.source.size != m_base->info.size ||
                                 value.source.frames != m_base->info.frames ||
                                 value.source.loop != m_base->info.loop ||
                                 value.pixels.size() != m_base->pixels.size()))
                value.error = {"decode", "invalid-image", "Animation changed during playback"};
            if (value.error) {
                run(false);
                m_complete = true;
                const auto deliver = m_deliver;
                deliver({}, std::move(value.error));
                return;
            }
            m_ring.append({index, cycle, std::move(value.pixels), value.delay});
            if (m_running && m_waiting)
                tick();
            else
                fill();
        });
    if (ticket.status == Pool::Admission::Accepted) {
        m_inFlight = true;
    } else if (ticket.status != Pool::Admission::Busy) {
        run(false);
        m_complete = true;
        const auto deliver = m_deliver;
        deliver({}, {"admission", "output-failed", "Could not admit an animation frame"});
    }
}

} // namespace Imv
