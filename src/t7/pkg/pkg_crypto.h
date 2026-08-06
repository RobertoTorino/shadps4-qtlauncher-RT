// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>

#include "common/key_manager.h"

namespace T7::Pkg {

class Crypto {
public:
    explicit Crypto(const KeyManager::AllKeys& keys);

    bool Validate(std::string& error) const;
    bool DecryptDerivedKey3(std::span<const u8, 256> ciphertext,
                            std::span<u8, 32> plaintext, std::string& error) const;
    bool DecryptImageKey(std::span<const u8, 256> ciphertext, std::span<u8, 32> plaintext,
                         std::string& error) const;

    static void HashIvKey(std::span<const u8, 64> input, std::span<u8, 32> output);
    static bool DecryptCbc(std::span<const u8, 32> iv_key, std::span<const u8> ciphertext,
                           std::span<u8> plaintext, std::string& error);
    static void GeneratePfsKeys(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                                std::span<u8, 16> data_key, std::span<u8, 16> tweak_key);
    static bool DecryptPfs(std::span<const u8, 16> data_key,
                           std::span<const u8, 16> tweak_key, std::span<const u8> source,
                           std::span<u8> destination, u64 sector, std::string& error);

private:
    bool DecryptRsa(const KeyManager::PkgDerivedKey3Keyset& keyset,
                    std::span<const u8, 256> ciphertext, std::span<u8, 32> plaintext,
                    std::string& error) const;
    bool DecryptRsa(const KeyManager::FakeKeyset& keyset,
                    std::span<const u8, 256> ciphertext, std::span<u8, 32> plaintext,
                    std::string& error) const;

    const KeyManager::AllKeys& m_keys;
};

} // namespace T7::Pkg