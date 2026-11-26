// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QByteArray>
#include <QFile>

inline bool mappedFrom(const QByteArray &prefix, bool needsPlugin) {
    QFile maps("/proc/self/maps");
    if (!maps.open(QIODevice::ReadOnly))
        return false;
    bool library = false, plugin = false;
    for (;;) {
        const auto line = maps.readLine();
        if (line.isEmpty())
            break;
        const bool backing = line.contains("/libimvqml.so.");
        const bool module = line.contains("/libimvqmlplugin.so");
        if ((backing || module) && !line.contains(prefix + '/'))
            return false;
        library |= backing;
        plugin |= module;
    }
    return library && (!needsPlugin || plugin);
}
