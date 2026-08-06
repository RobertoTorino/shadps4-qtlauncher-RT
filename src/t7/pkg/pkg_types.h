// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <string_view>
#include "common/endian.h"
#include "common/types.h"

namespace T7::Pkg {

struct PKGHeader {
    u32_be magic;                   // 0x7F434E54
    u32_be pkg_type;
    u32_be pkg_0x8;
    u32_be pkg_file_count;
    u32_be pkg_table_entry_count;
    u16_be pkg_sc_entry_count;
    u16_be pkg_table_entry_count_2;
    u32_be pkg_table_entry_offset;
    u32_be pkg_sc_entry_data_size;
    u64_be pkg_body_offset;
    u64_be pkg_body_size;
    u64_be pkg_content_offset;
    u64_be pkg_content_size;
    u8 pkg_content_id[0x24];
    u8 pkg_padding[0xC];
    u32_be pkg_drm_type;
    u32_be pkg_content_type;
    u32_be pkg_content_flags;
    u32_be pkg_promote_size;
    u32_be pkg_version_date;
    u32_be pkg_version_hash;
    u32_be pkg_0x088;
    u32_be pkg_0x08C;
    u32_be pkg_0x090;
    u32_be pkg_0x094;
    u32_be pkg_iro_tag;
    u32_be pkg_drm_type_version;
    u8 pkg_zeroes_1[0x60];
    u8 digest_entries1[0x20];
    u8 digest_entries2[0x20];
    u8 digest_table_digest[0x20];
    u8 digest_body_digest[0x20];
    u8 pkg_zeroes_2[0x280];
    u32_be pkg_0x400;
    u32_be pfs_image_count;
    u64_be pfs_image_flags;
    u64_be pfs_image_offset;
    u64_be pfs_image_size;
    u64_be mount_image_offset;
    u64_be mount_image_size;
    u64_be pkg_size;
    u32_be pfs_signed_size;
    u32_be pfs_cache_size;
    u8 pfs_image_digest[0x20];
    u8 pfs_signed_digest[0x20];
    u64_be pfs_split_size_nth_0;
    u64_be pfs_split_size_nth_1;
    u8 pkg_zeroes_3[0xB50];
    u8 pkg_digest[0x20];
};
static_assert(sizeof(PKGHeader) == 0x1000);

struct PKGEntry {
    u32_be id;
    u32_be filename_offset;
    u32_be flags1;
    u32_be flags2;
    u32_be offset;
    u32_be size;
    u64_be padding;
};
static_assert(sizeof(PKGEntry) == 32);

enum class PKGContentFlag : u32 {
    FIRST_PATCH = 0x100000,
    PATCHGO = 0x200000,
    REMASTER = 0x400000,
    PS_CLOUD = 0x800000,
    GD_AC = 0x2000000,
    NON_GAME = 0x4000000,
    UNKNOWN_0x8000000 = 0x8000000,
    SUBSEQUENT_PATCH = 0x40000000,
    DELTA_PATCH = 0x41000000,
    CUMULATIVE_PATCH = 0x60000000
};

#define PFS_FILE 2
#define PFS_DIR 3
#define PFS_CURRENT_DIR 4
#define PFS_PARENT_DIR 5

enum PfsMode : u16 {
    None = 0,
    Signed = 0x1,
    Is64Bit = 0x2,
    Encrypted = 0x4,
    UnknownFlagAlwaysSet = 0x8
};

struct PFSCHdr {
    s32 magic;
    s32 unk4;
    s32 unk8;
    s32 block_sz;
    s64 block_sz2;
    s64 block_offsets;
    u64 data_start;
    s64 data_length;
};

struct Inode {
    u16 Mode;
    u16 Nlink;
    u32 Flags;
    s64 Size;
    s64 SizeCompressed;
    s64 Time1_sec;
    s64 Time2_sec;
    s64 Time3_sec;
    s64 Time4_sec;
    u32 Time1_nsec;
    u32 Time2_nsec;
    u32 Time3_nsec;
    u32 Time4_nsec;
    u32 Uid;
    u32 Gid;
    u64 unk1;
    u64 unk2;
    u32 Blocks;
    u32 loc;
};

struct Dirent {
    u32 ino;
    u32 type;
    u32 namelen;
    u32 entsize;
    char name[256];
};

struct PfsFileEntry {
    std::string name;
    u32 inode;
    u32 type;
};

std::string_view GetEntryNameByType(u32 type);

} // namespace T7::Pkg
