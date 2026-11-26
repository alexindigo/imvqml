// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-wire.h"

namespace Imv {
bool initializeVips(const char *program);
DecodeReport decodeWithVips(int input, int output, const DecodeSpec &spec, qint64 length);
} // namespace Imv
