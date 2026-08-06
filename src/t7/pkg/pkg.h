// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/key_manager.h"
#include "pkg_crypto.h"
#include "pkg_types.h"

namespace T7::Pkg {

class PKGInstaller {
public:
    PKGInstaller();
    ~PKGInstaller();

    struct ValidationResult {
        bool success = false;
        std::string error;
        std::string title_id;
        std::string content_id;
        std::string category;
        std::string app_ver;
        u64 pkg_size = 0;
    };

    using ProgressCallback = std::function<bool(int current, int total, const std::string& message)>;

    ValidationResult ValidatePackage(const std::filesystem::path& pkg_path);

    bool Extract(const std::filesystem::path& pkg_path,
                const std::filesystem::path& extract_path,
                const ProgressCallback& progress_callback,
                std::string& error);

private:
    bool ReadHeader(const std::filesystem::path& pkg_path, std::string& error);
    bool ExtractSceMetadata(const std::filesystem::path& pkg_path,
                           const std::filesystem::path& extract_path,
                           std::string& error);
    bool ExtractPfsImage(const std::filesystem::path& pkg_path,
                        const std::filesystem::path& extract_path,
                        const ProgressCallback& progress_callback,
                        std::string& error);
    bool ValidatePaths() const;
    u32 GetPFSCOffset(std::span<const u8> pfs_image) const;

    PKGHeader m_header{};
    std::vector<u8> m_sfo_data;
    std::array<u8, 32> m_dk3{};
    std::array<u8, 32> m_iv_key{};
    std::array<u8, 256> m_img_key{};
    std::array<u8, 32> m_ekpfs_key{};
    std::array<u8, 16> m_data_key{};
    std::array<u8, 16> m_tweak_key{};
    std::vector<PfsFileEntry> m_fs_table;
    std::vector<Inode> m_inode_buf;
    std::vector<u64> m_sector_map;
    std::unordered_map<int, std::filesystem::path> m_extract_paths;
    u64 m_pfsc_offset = 0;
    std::filesystem::path m_current_dir;
};

} // namespace T7::Pkg
