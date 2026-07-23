#pragma once

#ifndef __blowfish__
#define __blowfish__

#include <stdint.h>

class Blowfish {
public:
    void SetKey(const unsigned char* key, int byte_length);
    void Encrypt(unsigned char* dst, const unsigned char* src, int byte_length) const;
    void Decrypt(unsigned char* dst, const unsigned char* src, int byte_length) const;
    void EncryptBlock(uint32_t *left, uint32_t *right) const;
    void DecryptBlock(uint32_t *left, uint32_t *right) const;
private:
    uint32_t Feistel(uint32_t value) const;
    
private:
    uint32_t pary_[18];
    uint32_t sbox_[4][256];
};

#endif /* defined(__blowfish__) */