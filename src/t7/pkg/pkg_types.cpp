// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pkg_types.h"

namespace T7::Pkg {

std::string_view GetEntryNameByType(u32 type) {
    switch (type) {
    case 0x001:
        return "digests";
    case 0x010:
        return "entry_keys";
    case 0x020:
        return "image_key";
    case 0x080:
        return "general_digests";
    case 0x100:
        return "metas";
    case 0x200:
        return "entry_names";
    case 0x400:
        return "license.dat";
    case 0x401:
        return "license.info";
    case 0x402:
        return "nptitle.dat";
    case 0x403:
        return "npbind.dat";
    case 0x404:
        return "selfinfo.dat";
    case 0x406:
        return "imageinfo.dat";
    case 0x407:
        return "target-deltainfo.dat";
    case 0x408:
        return "origin-deltainfo.dat";
    case 0x409:
        return "psreserved.dat";
    case 0x1000:
        return "param.sfo";
    case 0x1001:
        return "playgo-chunk.dat";
    case 0x1002:
        return "playgo-chunk.sha";
    case 0x1003:
        return "playgo-manifest.xml";
    case 0x1004:
        return "pronunciation.xml";
    case 0x1005:
        return "pronunciation.sig";
    case 0x1006:
        return "pic1.png";
    case 0x1007:
        return "pubtoolinfo.dat";
    case 0x1008:
        return "app/playgo-chunk.dat";
    case 0x1009:
        return "app/playgo-chunk.sha";
    case 0x100A:
        return "app/playgo-manifest.xml";
    case 0x100B:
        return "shareparam.json";
    case 0x100C:
        return "shareoverlayimage.png";
    case 0x100D:
        return "save_data.png";
    case 0x100E:
        return "shareprivacyguardimage.png";
    default:
        return "";
    }
}

} // namespace T7::Pkg
