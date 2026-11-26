// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "private/decode-wire.h"
#include <optional>

namespace Imv {
std::optional<DecodeReport> decodeVideo(int input, int output, const DecodeSpec &spec);
}
