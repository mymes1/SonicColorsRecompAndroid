#pragma once
// AES-128, decryption only, with CBC.
//
// The only user is the disc installer: Wii partition data is AES-128-CBC, one sector at a
// time (0x8000-byte sectors, hashed or plain, each with its own IV). Nothing else in the
// runtime needs a cipher, so this stays small and dependency-free rather than pulling in a
// crypto library for one call.
#include <cstddef>
#include <cstdint>

// One AES-128 key schedule: 11 round keys of 16 bytes.
struct Aes128
{
    uint8_t roundKey[176];

    // False when the key is all zeroes, which is a caller error, not a valid key.
    bool SetKey(const uint8_t key[16]);
    void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const;
};

// AES-128-CBC decryption in place. `len` must be a multiple of 16.
void Aes128CbcDecrypt(const Aes128& aes, const uint8_t iv[16], uint8_t* data, size_t len);

// Encryption, only for the installer's own tests (tools/iso_install_test builds an encrypted
// disc image and checks that the installer decrypts it). Keeping both directions in one file
// keeps the S-box tables and round keys in one place.
void Aes128CbcEncrypt(const Aes128& aes, const uint8_t iv[16], uint8_t* data, size_t len);
