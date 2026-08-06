// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pkg_zlib.h"

#include <limits>
#include <zlib.h>

namespace T7::Pkg {

bool InflateData(const u8* compressed_data, size_t compressed_size,
                 u8* decompressed_data, size_t decompressed_size) {
    if (compressed_size > std::numeric_limits<uInt>::max() ||
        decompressed_size > std::numeric_limits<uInt>::max()) {
        return false;
    }

    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(compressed_data);
    stream.avail_in = static_cast<uInt>(compressed_size);
    stream.next_out = decompressed_data;
    stream.avail_out = static_cast<uInt>(decompressed_size);
    if (inflateInit(&stream) != Z_OK) {
        return false;
    }
    const int result = inflate(&stream, Z_FINISH);
    const bool success = result == Z_STREAM_END && stream.total_out == decompressed_size;
    inflateEnd(&stream);
    return success;
}

} // namespace T7::Pkg
