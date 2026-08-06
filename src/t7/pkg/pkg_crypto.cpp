// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pkg_crypto.h"

#include <algorithm>
#include <cstring>
#include <cryptopp/aes.h>
#include <cryptopp/hmac.h>
#include <cryptopp/modes.h>
#include <cryptopp/osrng.h>
#include <cryptopp/rsa.h>
#include <cryptopp/sha.h>

namespace T7::Pkg {
namespace {

template <typename Keyset>
bool ValidateKeyset(const Keyset& keyset, std::string_view name, std::string& error) {
    const std::array components{
        std::pair{&keyset.Exponent1, size_t{128}},
        std::pair{&keyset.Exponent2, size_t{128}},
        std::pair{&keyset.PublicExponent, size_t{4}},
        std::pair{&keyset.Coefficient, size_t{128}},
        std::pair{&keyset.Modulus, size_t{256}},
        std::pair{&keyset.Prime1, size_t{128}},
        std::pair{&keyset.Prime2, size_t{128}},
        std::pair{&keyset.PrivateExponent, size_t{256}},
    };
    for (const auto& [component, expected_size] : components) {
        if (component->size() != expected_size) {
            error = std::string{name} + " contains a missing or incorrectly sized RSA component";
            return false;
        }
    }
    return true;
}

template <typename Keyset>
bool BuildPrivateKey(const Keyset& keyset, CryptoPP::RSA::PrivateKey& private_key,
                     std::string& error) {
    CryptoPP::InvertibleRSAFunction parameters;
    parameters.SetPrime1(CryptoPP::Integer(keyset.Prime1.data(), keyset.Prime1.size()));
    parameters.SetPrime2(CryptoPP::Integer(keyset.Prime2.data(), keyset.Prime2.size()));
    parameters.SetPublicExponent(
        CryptoPP::Integer(keyset.PublicExponent.data(), keyset.PublicExponent.size()));
    parameters.SetPrivateExponent(
        CryptoPP::Integer(keyset.PrivateExponent.data(), keyset.PrivateExponent.size()));
    parameters.SetModPrime1PrivateExponent(
        CryptoPP::Integer(keyset.Exponent1.data(), keyset.Exponent1.size()));
    parameters.SetModPrime2PrivateExponent(
        CryptoPP::Integer(keyset.Exponent2.data(), keyset.Exponent2.size()));
    parameters.SetModulus(CryptoPP::Integer(keyset.Modulus.data(), keyset.Modulus.size()));
    parameters.SetMultiplicativeInverseOfPrime2ModPrime1(
        CryptoPP::Integer(keyset.Coefficient.data(), keyset.Coefficient.size()));
    private_key = CryptoPP::RSA::PrivateKey(parameters);

    CryptoPP::AutoSeededRandomPool random;
    if (!private_key.Validate(random, 2)) {
        error = "RSA private key validation failed";
        return false;
    }
    return true;
}

template <typename Keyset>
bool DecryptRsaImpl(const Keyset& keyset, std::span<const u8, 256> ciphertext,
                    std::span<u8, 32> plaintext, std::string& error) {
    try {
        CryptoPP::RSA::PrivateKey private_key;
        if (!BuildPrivateKey(keyset, private_key, error)) {
            return false;
        }
        CryptoPP::RSAES_PKCS1v15_Decryptor decryptor(private_key);
        CryptoPP::AutoSeededRandomPool random;
        std::array<u8, 256> decrypted{};
        const auto result = decryptor.Decrypt(random, ciphertext.data(), ciphertext.size(),
                                              decrypted.data());
        if (!result.isValidCoding || result.messageLength < plaintext.size()) {
            error = "RSA PKCS#1 decryption failed";
            return false;
        }
        std::copy_n(decrypted.begin(), plaintext.size(), plaintext.begin());
        return true;
    } catch (const CryptoPP::Exception& exception) {
        error = exception.what();
        return false;
    }
}

void XorBlock(u8* output, const u8* left, const u8* right) {
    for (size_t index = 0; index < CryptoPP::AES::BLOCKSIZE; ++index) {
        output[index] = left[index] ^ right[index];
    }
}

void MultiplyTweak(std::span<u8, CryptoPP::AES::BLOCKSIZE> tweak) {
    int carry = 0;
    for (auto& value : tweak) {
        const int next_carry = (value >> 7) & 1;
        value = static_cast<u8>((value << 1) | carry);
        carry = next_carry;
    }
    if (carry != 0) {
        tweak[0] ^= 0x87;
    }
}

} // namespace

Crypto::Crypto(const KeyManager::AllKeys& keys) : m_keys(keys) {}

bool Crypto::Validate(std::string& error) const {
    return ValidateKeyset(m_keys.PkgDerivedKey3Keyset, "PkgDerivedKey3Keyset", error) &&
           ValidateKeyset(m_keys.FakeKeyset, "FakeKeyset", error);
}

bool Crypto::DecryptDerivedKey3(std::span<const u8, 256> ciphertext,
                                std::span<u8, 32> plaintext, std::string& error) const {
    return DecryptRsa(m_keys.PkgDerivedKey3Keyset, ciphertext, plaintext, error);
}

bool Crypto::DecryptImageKey(std::span<const u8, 256> ciphertext,
                             std::span<u8, 32> plaintext, std::string& error) const {
    return DecryptRsa(m_keys.FakeKeyset, ciphertext, plaintext, error);
}

bool Crypto::DecryptRsa(const KeyManager::PkgDerivedKey3Keyset& keyset,
                        std::span<const u8, 256> ciphertext, std::span<u8, 32> plaintext,
                        std::string& error) const {
    return DecryptRsaImpl(keyset, ciphertext, plaintext, error);
}

bool Crypto::DecryptRsa(const KeyManager::FakeKeyset& keyset,
                        std::span<const u8, 256> ciphertext, std::span<u8, 32> plaintext,
                        std::string& error) const {
    return DecryptRsaImpl(keyset, ciphertext, plaintext, error);
}

void Crypto::HashIvKey(std::span<const u8, 64> input, std::span<u8, 32> output) {
    CryptoPP::SHA256 hash;
    hash.CalculateDigest(output.data(), input.data(), input.size());
}

bool Crypto::DecryptCbc(std::span<const u8, 32> iv_key, std::span<const u8> ciphertext,
                        std::span<u8> plaintext, std::string& error) {
    if (ciphertext.size() != plaintext.size() ||
        ciphertext.size() % CryptoPP::AES::BLOCKSIZE != 0) {
        error = "AES-CBC input size is invalid";
        return false;
    }
    try {
        CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption decryptor;
        decryptor.SetKeyWithIV(iv_key.data() + 16, 16, iv_key.data(), 16);
        decryptor.ProcessData(plaintext.data(), ciphertext.data(), ciphertext.size());
        return true;
    } catch (const CryptoPP::Exception& exception) {
        error = exception.what();
        return false;
    }
}

void Crypto::GeneratePfsKeys(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                             std::span<u8, 16> data_key, std::span<u8, 16> tweak_key) {
    CryptoPP::HMAC<CryptoPP::SHA256> hmac(ekpfs.data(), ekpfs.size());
    std::array<u8, 20> input{};
    input[0] = 1;
    std::copy(seed.begin(), seed.end(), input.begin() + 4);
    std::array<u8, CryptoPP::SHA256::DIGESTSIZE> output{};
    hmac.CalculateDigest(output.data(), input.data(), input.size());
    std::copy_n(output.begin(), tweak_key.size(), tweak_key.begin());
    std::copy_n(output.begin() + tweak_key.size(), data_key.size(), data_key.begin());
}

bool Crypto::DecryptPfs(std::span<const u8, 16> data_key,
                        std::span<const u8, 16> tweak_key, std::span<const u8> source,
                        std::span<u8> destination, u64 sector, std::string& error) {
    if (source.size() != destination.size() || source.size() % 0x1000 != 0) {
        error = "PFS input size is invalid";
        return false;
    }
    CryptoPP::ECB_Mode<CryptoPP::AES>::Encryption tweak_encryptor(tweak_key.data(),
                                                                  tweak_key.size());
    CryptoPP::ECB_Mode<CryptoPP::AES>::Decryption data_decryptor(data_key.data(), data_key.size());
    for (size_t offset = 0; offset < source.size(); offset += 0x1000) {
        const u64 current_sector = sector + offset / 0x1000;
        std::array<u8, 16> tweak_input{};
        std::memcpy(tweak_input.data(), &current_sector, sizeof(current_sector));
        std::array<u8, 16> encrypted_tweak{};
        tweak_encryptor.ProcessData(encrypted_tweak.data(), tweak_input.data(),
                                    encrypted_tweak.size());
        for (size_t block = 0; block < 0x1000; block += CryptoPP::AES::BLOCKSIZE) {
            std::array<u8, 16> buffer{};
            XorBlock(buffer.data(), source.data() + offset + block, encrypted_tweak.data());
            data_decryptor.ProcessData(buffer.data(), buffer.data(), buffer.size());
            XorBlock(destination.data() + offset + block, buffer.data(), encrypted_tweak.data());
            MultiplyTweak(encrypted_tweak);
        }
    }
    return true;
}

} // namespace T7::Pkg