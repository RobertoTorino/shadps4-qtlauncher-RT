// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pkg.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "common/io_file.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "core/file_format/psf.h"
#include "pkg_zlib.h"

namespace T7::Pkg {

namespace {

constexpr u32 PKG_MAGIC = 0x7F434E54;
constexpr u32 PFSC_MAGIC = 0x43534650;
constexpr size_t BLOCK_SIZE = 0x10000;

bool ReadEntry(Common::FS::IOFile& file, PKGEntry& entry) {
    return file.Read(entry.id) == 1 && file.Read(entry.filename_offset) == 1 &&
           file.Read(entry.flags1) == 1 && file.Read(entry.flags2) == 1 &&
           file.Read(entry.offset) == 1 && file.Read(entry.size) == 1 &&
           file.Seek(8, Common::FS::SeekOrigin::CurrentPosition);
}

bool DecompressPFSC(std::span<const u8> compressed_data, std::span<u8> decompressed_data) {
    if (!InflateData(compressed_data.data(), compressed_data.size(),
                     decompressed_data.data(), decompressed_data.size())) {
        LOG_ERROR(Loader, "Failed to decompress PFSC data");
        return false;
    }
    return true;
}

} // namespace

PKGInstaller::PKGInstaller() = default;
PKGInstaller::~PKGInstaller() = default;

PKGInstaller::ValidationResult PKGInstaller::ValidatePackage(const std::filesystem::path& pkg_path) {
    ValidationResult result;

    if (!std::filesystem::exists(pkg_path)) {
        result.error = "PKG file does not exist";
        return result;
    }

    Common::FS::IOFile file(pkg_path, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        result.error = "Failed to open PKG file";
        return result;
    }

    PKGHeader header{};
    if (file.GetSize() < sizeof(header) || !file.Read(header)) {
        result.error = "PKG header is truncated";
        return result;
    }

    if (header.magic != PKG_MAGIC) {
        result.error = "Invalid PKG magic";
        return result;
    }

    const u64 file_size = file.GetSize();
    result.pkg_size = file_size;

    if (header.pkg_size > file_size) {
        result.error = "PKG file size mismatch";
        return result;
    }

    if (header.pkg_content_offset > header.pkg_size ||
        header.pkg_content_size > header.pkg_size - header.pkg_content_offset) {
        result.error = "Invalid content size/offset";
        return result;
    }

    // Extract title ID from content ID
    if (header.pkg_content_id[0] == 0) {
        result.error = "Missing content ID";
        return result;
    }

    const auto* content_id_begin = reinterpret_cast<const char*>(header.pkg_content_id);
    const auto* content_id_end = std::find(content_id_begin, content_id_begin + 0x24, '\0');
    std::string content_id(content_id_begin, content_id_end);
    result.content_id = content_id;

    if (content_id.size() >= 16) {
        result.title_id = content_id.substr(7, 9);
    } else {
        result.error = "Invalid content ID format";
        return result;
    }
    if (!std::all_of(result.title_id.begin(), result.title_id.end(), [](unsigned char character) {
            return std::isalnum(character) != 0;
        })) {
        result.error = "Invalid title ID in content ID";
        return result;
    }

    // Read SFO
    const u64 table_offset = header.pkg_table_entry_offset;
    const u32 n_entries = header.pkg_table_entry_count;
    if (n_entries > 100000 || table_offset > file_size ||
        static_cast<u64>(n_entries) * sizeof(PKGEntry) > file_size - table_offset ||
        !file.Seek(table_offset)) {
        result.error = "Invalid PKG entry table";
        return result;
    }

    for (u32 i = 0; i < n_entries; i++) {
        PKGEntry entry{};
        if (!ReadEntry(file, entry)) {
            result.error = "PKG entry table is truncated";
            return result;
        }

        if (entry.offset > file_size || entry.size > file_size - entry.offset) {
            result.error = "PKG entry extends beyond the package";
            return result;
        }

        if (entry.id == 0x1000) { // param.sfo
            auto current_pos = file.Tell();
            if (!file.Seek(entry.offset)) {
                result.error = "Failed to seek to param.sfo";
                return result;
            }

            if (entry.size > 10 * 1024 * 1024) { // Sanity check: 10MB max
                result.error = "param.sfo size is suspiciously large";
                return result;
            }

            std::vector<u8> sfo_data(entry.size);
            if (file.ReadRaw<u8>(sfo_data.data(), entry.size) != entry.size) {
                result.error = "param.sfo is truncated";
                return result;
            }

            PSF psf;
            if (!psf.Open(sfo_data)) {
                result.error = "Failed to parse param.sfo";
                return result;
            }

            if (auto category = psf.GetString("CATEGORY")) {
                result.category = std::string{*category};
            } else {
                result.error = "Missing CATEGORY in param.sfo";
                return result;
            }

            if (auto app_ver = psf.GetString("APP_VER")) {
                result.app_ver = std::string{*app_ver};
            }

            file.Seek(current_pos);
            break;
        }
    }

    if (result.category.empty()) {
        result.error = "Could not determine package category";
        return result;
    }

    result.success = true;
    return result;
}

bool PKGInstaller::ReadHeader(const std::filesystem::path& pkg_path, std::string& error) {
    Common::FS::IOFile file(pkg_path, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        error = "Failed to open PKG file";
        return false;
    }

    if (file.GetSize() < sizeof(m_header) || !file.Read(m_header)) {
        error = "PKG header is truncated";
        return false;
    }

    if (m_header.magic != PKG_MAGIC) {
        error = "Invalid PKG magic";
        return false;
    }

    return true;
}

bool PKGInstaller::ValidatePaths() const {
    for (const auto& [inode, path] : m_extract_paths) {
        const auto path_str = path.u8string();
        if (path_str.find(u8"..") != std::string::npos ||
            path_str.find(u8"\\..") != std::string::npos ||
            path_str.find(u8"/..") != std::string::npos) {
            LOG_ERROR(Loader, "Path traversal detected: {}", path.string());
            return false;
        }
    }
    return true;
}

u32 PKGInstaller::GetPFSCOffset(std::span<const u8> pfs_image) const {
    for (u32 i = 0x20000; i < pfs_image.size() && i < 0x200000; i += 0x10000) {
        if (i + 4 > pfs_image.size()) break;
        u32 value;
        std::memcpy(&value, &pfs_image[i], sizeof(u32));
        if (value == PFSC_MAGIC) {
            return i;
        }
    }
    return static_cast<u32>(-1);
}

bool PKGInstaller::ExtractSceMetadata(const std::filesystem::path& pkg_path,
                                      const std::filesystem::path& extract_path,
                                      std::string& error) {
    Common::FS::IOFile file(pkg_path, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        error = "Failed to open PKG file";
        return false;
    }

    auto keyManager = KeyManager::GetInstance();
    if (!keyManager || !keyManager->HasKeys()) {
        error = "Cryptographic keys are missing. Configure them in Settings > Keys before "
            "installing packages.";
        return false;
    }

    Crypto crypto(keyManager->GetAllKeys());
    std::string crypto_error;
    if (!crypto.Validate(crypto_error)) {
        error = "Cryptographic key validation failed: " + crypto_error;
        return false;
    }

    const u64 file_size = file.GetSize();
    const u32 offset = m_header.pkg_table_entry_offset;
    const u32 n_entries = m_header.pkg_table_entry_count;

    if (n_entries > 100000 || offset > file_size ||
        static_cast<u64>(n_entries) * sizeof(PKGEntry) > file_size - offset ||
        !file.Seek(offset)) {
        error = "Invalid PKG entry table";
        return false;
    }

    std::array<u8, 64> concatenated_ivkey_dk3;
    std::array<u8, 256> img_key_data;

    for (u32 i = 0; i < n_entries; i++) {
        PKGEntry entry{};
        if (!ReadEntry(file, entry)) {
            error = "PKG entry table is truncated";
            return false;
        }

        if (entry.offset > file_size || entry.size > file_size - entry.offset) {
            error = "PKG entry extends beyond the package";
            return false;
        }

        auto current_pos = file.Tell();
        const auto name = GetEntryNameByType(entry.id);

        if (entry.id == 0x10) { // ENTRY_KEYS
            if (!file.Seek(entry.offset) || entry.size < 32 + 7 * 32 + 7 * 256) {
                error = "Invalid ENTRY_KEYS entry";
                return false;
            }

            std::array<u8, 32> seed_digest;
            std::array<std::array<u8, 32>, 7> digest1;
            std::array<std::array<u8, 256>, 7> key1;

            if (file.Read(seed_digest) != seed_digest.size()) {
                error = "ENTRY_KEYS is truncated";
                return false;
            }
            for (int j = 0; j < 7; j++) {
                if (file.Read(digest1[j]) != digest1[j].size()) {
                    error = "ENTRY_KEYS digest table is truncated";
                    return false;
                }
            }
            for (int j = 0; j < 7; j++) {
                if (file.Read(key1[j]) != key1[j].size()) {
                    error = "ENTRY_KEYS key table is truncated";
                    return false;
                }
            }

            if (!crypto.DecryptDerivedKey3(key1[3], m_dk3, crypto_error)) {
                error = "Failed to decrypt DK3: " + crypto_error;
                return false;
            }
        } else if (entry.id == 0x20) { // IMAGE_KEY
            if (!file.Seek(entry.offset) || entry.size < 256) {
                error = "Invalid IMAGE_KEY entry";
                return false;
            }

            if (file.Read(img_key_data) != img_key_data.size()) {
                error = "IMAGE_KEY entry is truncated";
                return false;
            }

            std::memcpy(concatenated_ivkey_dk3.data(), &entry, sizeof(entry));
            std::memcpy(concatenated_ivkey_dk3.data() + sizeof(entry), m_dk3.data(), m_dk3.size());

            crypto.HashIvKey(concatenated_ivkey_dk3, m_iv_key);

            if (!crypto.DecryptCbc(m_iv_key, img_key_data, m_img_key, crypto_error)) {
                error = "Failed to decrypt image key: " + crypto_error;
                return false;
            }

            if (!crypto.DecryptImageKey(m_img_key, m_ekpfs_key, crypto_error)) {
                error = "Failed to decrypt EKPFS key: " + crypto_error;
                return false;
            }
        }

        // Extract metadata files
        if (!name.empty()) {
            const auto filepath = extract_path / "sce_sys" / name;
            std::filesystem::create_directories(filepath.parent_path());

            if (!file.Seek(entry.offset)) {
                error = std::string("Failed to seek to ") + std::string(name);
                return false;
            }

            if (entry.size > 100 * 1024 * 1024) { // Sanity check
                error = std::string("Entry ") + std::string(name) + " size is too large";
                return false;
            }

            std::vector<u8> data(entry.size);
            if (file.ReadRaw<u8>(data.data(), entry.size) != entry.size) {
                error = std::string(name) + " is truncated";
                return false;
            }

            // Decrypt NP entries
            if (entry.id >= 0x400 && entry.id <= 0x403) {
                std::array<u8, 64> ivkey_concat;
                std::memcpy(ivkey_concat.data(), &entry, sizeof(entry));
                std::memcpy(ivkey_concat.data() + sizeof(entry), m_dk3.data(), m_dk3.size());

                std::array<u8, 32> ivkey;
                crypto.HashIvKey(ivkey_concat, ivkey);

                std::vector<u8> decrypted(entry.size);
                if (!crypto.DecryptCbc(ivkey, data, decrypted, crypto_error)) {
                    LOG_WARNING(Loader, "Failed to decrypt {}: {}", name, crypto_error);
                } else {
                    data = std::move(decrypted);
                }
            }

            Common::FS::IOFile out(filepath, Common::FS::FileAccessMode::Write);
            if (!out.IsOpen()) {
                error = std::string("Failed to create ") + std::string(name);
                return false;
            }
            if (out.WriteRaw<u8>(data.data(), data.size()) != data.size()) {
                error = std::string("Failed to write ") + std::string(name);
                return false;
            }
            out.Close();

            if (entry.id == 0x1000) {
                m_sfo_data = data;
            }
        }

        file.Seek(current_pos);
    }

    return true;
}

bool PKGInstaller::ExtractPfsImage(const std::filesystem::path& pkg_path,
                                   const std::filesystem::path& extract_path,
                                   const ProgressCallback& progress_callback,
                                   std::string& error) {
    Common::FS::IOFile file(pkg_path, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        error = "Failed to open PKG file";
        return false;
    }

    auto keyManager = KeyManager::GetInstance();
    Crypto crypto(keyManager->GetAllKeys());

    // Read seed
    std::array<u8, 16> seed;
    const u64 file_size = file.GetSize();
    if (m_header.pfs_image_offset > file_size ||
        0x380 > file_size - m_header.pfs_image_offset ||
        !file.Seek(m_header.pfs_image_offset + 0x370)) {
        error = "Failed to seek to PFS seed";
        return false;
    }
    if (file.Read(seed) != seed.size()) {
        error = "PFS seed is truncated";
        return false;
    }

    // Generate PFS keys
    crypto.GeneratePfsKeys(m_ekpfs_key, seed, m_data_key, m_tweak_key);

    const u64 pfsc_length = static_cast<u64>(m_header.pfs_cache_size) * 2;
    if (pfsc_length == 0) {
        return true; // No PFS image
    }

    if (pfsc_length > 1024ULL * 1024 * 1024 || m_header.pfs_image_offset > file_size ||
        pfsc_length > file_size - m_header.pfs_image_offset) {
        error = "PFS cache size is unreasonably large";
        return false;
    }

    // Read and decrypt PFS image
    std::vector<u8> pfs_encrypted(pfsc_length);
    if (!file.Seek(m_header.pfs_image_offset)) {
        error = "Failed to seek to PFS image";
        return false;
    }
    if (file.Read(pfs_encrypted) != pfs_encrypted.size()) {
        error = "PFS image is truncated";
        return false;
    }

    std::vector<u8> pfs_decrypted(pfsc_length);
    std::string crypto_error;
    if (!crypto.DecryptPfs(m_data_key, m_tweak_key, pfs_encrypted, pfs_decrypted, 0,
                           crypto_error)) {
        error = "Failed to decrypt PFS: " + crypto_error;
        return false;
    }

    // Find PFSC
    m_pfsc_offset = GetPFSCOffset(pfs_decrypted);
    if (m_pfsc_offset == static_cast<u32>(-1)) {
        error = "PFSC signature not found";
        return false;
    }

    std::vector<u8> pfsc_data(pfsc_length - m_pfsc_offset);
    std::memcpy(pfsc_data.data(), pfs_decrypted.data() + m_pfsc_offset, pfsc_data.size());

    PFSCHdr pfsc_hdr;
    if (pfsc_data.size() < sizeof(pfsc_hdr)) {
        error = "PFSC header too small";
        return false;
    }
    std::memcpy(&pfsc_hdr, pfsc_data.data(), sizeof(pfsc_hdr));

    if (pfsc_hdr.block_sz2 <= 0 || pfsc_hdr.data_length < 0 || pfsc_hdr.block_offsets < 0) {
        error = "Invalid PFSC header";
        return false;
    }
    const s64 block_count = pfsc_hdr.data_length / pfsc_hdr.block_sz2;
    if (block_count < 0 || block_count > 1000000) {
        error = "Invalid number of PFS blocks";
        return false;
    }
    const int num_blocks = static_cast<int>(block_count);

    m_sector_map.resize(num_blocks + 1);
    const size_t sector_map_size = (num_blocks + 1) * 8;
    const u64 block_offsets = static_cast<u64>(pfsc_hdr.block_offsets);
    if (block_offsets > pfsc_data.size() || sector_map_size > pfsc_data.size() - block_offsets) {
        error = "Sector map extends beyond PFSC data";
        return false;
    }

    for (int i = 0; i < num_blocks + 1; i++) {
        std::memcpy(&m_sector_map[i], pfsc_data.data() + block_offsets + i * 8, 8);
        if (i > 0 && m_sector_map[i] < m_sector_map[i - 1]) {
            error = "PFSC sector map is not ordered";
            return false;
        }
    }
    if (!m_sector_map.empty() && m_sector_map.back() > pfsc_data.size()) {
        error = "PFSC sector map extends beyond its data";
        return false;
    }

    // Parse filesystem structure
    u32 ndinode = 0;
    int ndinode_counter = 0;
    bool dinode_reached = false;
    bool uroot_reached = false;

    std::vector<u8> decompressed_data(BLOCK_SIZE);

    for (int i = 0; i < num_blocks; i++) {
        if (progress_callback &&
            !progress_callback(i, num_blocks * 2, "Parsing filesystem structure...")) {
            error = "Package installation was canceled";
            return false;
        }

        const u64 sector_offset = m_sector_map[i];
        const u64 sector_size = m_sector_map[i + 1] - sector_offset;

        if (sector_offset + sector_size > pfsc_data.size()) {
            error = "Sector extends beyond PFSC data";
            return false;
        }

        if (sector_size == BLOCK_SIZE) {
            std::memcpy(decompressed_data.data(), pfsc_data.data() + sector_offset, BLOCK_SIZE);
        } else if (sector_size < BLOCK_SIZE && sector_size > 0) {
            std::span<const u8> compressed(pfsc_data.data() + sector_offset, sector_size);
            if (!DecompressPFSC(compressed, decompressed_data)) {
                error = "Failed to decompress PFSC filesystem block";
                return false;
            }
        } else {
            continue;
        }

        if (i == 0 && decompressed_data.size() >= 0x34) {
            std::memcpy(&ndinode, decompressed_data.data() + 0x30, 4);
        }

        const int occupied_blocks = ((ndinode * 0xA8) + BLOCK_SIZE - 1) / BLOCK_SIZE;

        if (i >= 1 && i <= occupied_blocks) {
            for (size_t p = 0; p + sizeof(Inode) <= decompressed_data.size(); p += 0xA8) {
                Inode node;
                std::memcpy(&node, &decompressed_data[p], sizeof(node));
                if (node.Mode == 0) {
                    break;
                }
                m_inode_buf.push_back(node);
            }
        }

        // Check for flat_path_table
        if (decompressed_data.size() > 25) {
            std::string_view flat_path_table(reinterpret_cast<const char*>(decompressed_data.data()) + 0x10, 15);
            if (flat_path_table == "flat_path_table") {
                uroot_reached = true;
            }
        }

        if (uroot_reached) {
            for (size_t j = 0; j < decompressed_data.size();) {
                if (j + sizeof(Dirent) > decompressed_data.size()) break;

                Dirent dirent;
                std::memcpy(&dirent, &decompressed_data[j], sizeof(dirent));

                if (dirent.entsize < 16 || dirent.entsize > 512 ||
                    j + dirent.entsize > decompressed_data.size()) {
                    break;
                }

                if (dirent.ino != 0) {
                    ndinode_counter++;
                    j += dirent.entsize;
                } else {
                    m_extract_paths[ndinode_counter] = extract_path;
                    uroot_reached = false;
                    break;
                }
            }
        }

        // Check for dirent structure
        if (decompressed_data.size() > 0x28 && decompressed_data[0x10] == '.' &&
            decompressed_data.size() > 0x29 &&
            decompressed_data[0x28] == '.' && decompressed_data[0x29] == '.') {
            dinode_reached = true;
        }

        if (dinode_reached) {
            for (size_t j = 0; j < decompressed_data.size();) {
                if (j + sizeof(Dirent) > decompressed_data.size()) break;

                Dirent dirent;
                std::memcpy(&dirent, &decompressed_data[j], sizeof(dirent));

                if (dirent.ino == 0 || dirent.entsize < 16 || dirent.entsize > 512 ||
                    j + dirent.entsize > decompressed_data.size() ||
                    dirent.namelen > sizeof(dirent.name)) {
                    break;
                }

                PfsFileEntry table;
                table.name = std::string(dirent.name, std::min(dirent.namelen, 255u));
                table.inode = dirent.ino;
                table.type = dirent.type;

                const std::filesystem::path entry_path(table.name);
                if (entry_path.empty() || entry_path.is_absolute() || entry_path.has_parent_path() ||
                    entry_path == "." || entry_path == "..") {
                    error = "Invalid path in PFS directory entry";
                    return false;
                }

                if (table.type == PFS_CURRENT_DIR) {
                    m_current_dir = m_extract_paths[table.inode];
                }

                m_extract_paths[table.inode] = m_current_dir / entry_path;

                if (table.type == PFS_FILE || table.type == PFS_DIR) {
                    if (table.type == PFS_DIR) {
                        std::filesystem::create_directories(m_extract_paths[table.inode]);
                    }
                    m_fs_table.push_back(table);
                    ndinode_counter++;

                    if ((ndinode_counter + 1) >= static_cast<int>(ndinode)) {
                        goto extraction_phase;
                    }
                }

                j += dirent.entsize;
            }
        }
    }

extraction_phase:
    // Validate paths before extraction
    if (!ValidatePaths()) {
        error = "Path traversal attack detected";
        return false;
    }

    // Extract files
    const size_t total_files = m_fs_table.size();
    for (size_t idx = 0; idx < total_files; idx++) {
        const auto& fs_entry = m_fs_table[idx];

        if (progress_callback &&
            !progress_callback(num_blocks + static_cast<int>(idx), num_blocks * 2,
                               "Extracting: " + fs_entry.name)) {
            error = "Package installation was canceled";
            return false;
        }

        if (fs_entry.type != PFS_FILE) {
            continue;
        }

        if (fs_entry.inode >= m_inode_buf.size()) {
            error = "Invalid inode index for " + fs_entry.name;
            return false;
        }

        const auto& inode = m_inode_buf[fs_entry.inode];
        const int sector_loc = inode.loc;
        const int nblocks = inode.Blocks;
        const s64 file_size = inode.Size;

        if (file_size < 0 || file_size > 100LL * 1024 * 1024 * 1024) { // 100GB sanity check
            error = "Invalid file size for " + fs_entry.name;
            return false;
        }

        Common::FS::IOFile out_file(m_extract_paths[fs_entry.inode],
                                    Common::FS::FileAccessMode::Write);
        if (!out_file.IsOpen()) {
            error = "Failed to create file: " + fs_entry.name;
            return false;
        }

        s64 size_written = 0;
        const u64 pfsc_buf_size = BLOCK_SIZE + 0x1000;
        std::vector<u8> pfsc_buffer(pfsc_buf_size);
        std::vector<u8> pfs_decrypted_block(pfsc_buf_size);
        std::vector<u8> decompressed_block(BLOCK_SIZE);

        for (int j = 0; j < nblocks; j++) {
            if (sector_loc + j >= static_cast<int>(m_sector_map.size()) - 1) {
                error = "Sector out of range for " + fs_entry.name;
                return false;
            }

            const u64 sector_offset = m_sector_map[sector_loc + j];
            const u64 sector_size = m_sector_map[sector_loc + j + 1] - sector_offset;
            const u64 file_offset = m_header.pfs_image_offset + m_pfsc_offset + sector_offset;
            const u64 current_sector = (m_pfsc_offset + sector_offset) / 0x1000;

            const int sector_offset_mask = static_cast<int>((sector_offset + m_pfsc_offset) & 0xFFFFF000);
            const int previous_data = static_cast<int>((sector_offset + m_pfsc_offset) - sector_offset_mask);

            if (!file.Seek(file_offset - previous_data)) {
                error = "Failed to seek for " + fs_entry.name;
                return false;
            }

            if (file_offset < static_cast<u64>(previous_data) ||
                file_offset - previous_data > file_size ||
                pfsc_buf_size > file_size - (file_offset - previous_data)) {
                error = "Encrypted PFS block extends beyond the package";
                return false;
            }
            if (file.ReadRaw<u8>(pfsc_buffer.data(), pfsc_buf_size) != pfsc_buf_size) {
                error = "Encrypted PFS block is truncated for " + fs_entry.name;
                return false;
            }

            std::string decrypt_error;
            if (!crypto.DecryptPfs(m_data_key, m_tweak_key,
                                  pfsc_buffer,
                                  pfs_decrypted_block,
                                  current_sector, decrypt_error)) {
                error = "Failed to decrypt block for " + fs_entry.name + ": " + decrypt_error;
                return false;
            }

            if (sector_size == BLOCK_SIZE) {
                std::memcpy(decompressed_block.data(),
                           pfs_decrypted_block.data() + previous_data, BLOCK_SIZE);
            } else if (sector_size < BLOCK_SIZE && sector_size > 0) {
                std::span<const u8> compressed(pfs_decrypted_block.data() + previous_data, sector_size);
                    if (!DecompressPFSC(compressed, decompressed_block)) {
                        error = "Failed to decompress file block for " + fs_entry.name;
                        return false;
                    }
            } else {
                continue;
            }

            size_written += BLOCK_SIZE;

            if (j < nblocks - 1) {
                if (out_file.WriteRaw<u8>(decompressed_block.data(), BLOCK_SIZE) != BLOCK_SIZE) {
                    error = "Failed to write " + fs_entry.name;
                    return false;
                }
            } else {
                const size_t final_write = std::min(static_cast<size_t>(file_size - (size_written - BLOCK_SIZE)),
                                                    BLOCK_SIZE);
                if (out_file.WriteRaw<u8>(decompressed_block.data(), final_write) != final_write) {
                    error = "Failed to write " + fs_entry.name;
                    return false;
                }
            }
        }

        out_file.Close();
    }

    return true;
}

bool PKGInstaller::Extract(const std::filesystem::path& pkg_path,
                           const std::filesystem::path& extract_path,
                           const ProgressCallback& progress_callback,
                           std::string& error) {
    m_fs_table.clear();
    m_inode_buf.clear();
    m_sector_map.clear();
    m_extract_paths.clear();
    m_sfo_data.clear();

    if (!ReadHeader(pkg_path, error)) {
        return false;
    }

    if (progress_callback && !progress_callback(0, 100, "Extracting metadata...")) {
        error = "Package installation was canceled";
        return false;
    }

    if (!ExtractSceMetadata(pkg_path, extract_path, error)) {
        return false;
    }

    if (progress_callback && !progress_callback(20, 100, "Extracting game files...")) {
        error = "Package installation was canceled";
        return false;
    }

    if (!ExtractPfsImage(pkg_path, extract_path, progress_callback, error)) {
        return false;
    }

    if (progress_callback && !progress_callback(100, 100, "Extraction complete")) {
        error = "Package installation was canceled";
        return false;
    }

    return true;
}

} // namespace T7::Pkg
