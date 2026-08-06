// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace T7::Pkg {

bool InflateData(const u8* compressed_data, size_t compressed_size,
                 u8* decompressed_data, size_t decompressed_size);

} // namespace T7::Pkg
