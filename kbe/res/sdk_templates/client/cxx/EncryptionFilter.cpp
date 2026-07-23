#include "EncryptionFilter.h"
#include <cstring>
#include <random>
#include <iostream>
#include <algorithm>

#include "MessageReader.h"
#include "NetworkInterfaceBase.h"
#include "blowfish/Blowfish.h"

namespace KBEngine {


// 生成随机密钥
void BlowfishFilter::generateRandomKey(int keySize) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

    key_.resize(keySize);
    for (int i = 0; i < keySize; ++i) {
        key_[i] = alphanum[dis(gen)];
    }
}

// 大小端转换（如果平台是小端，需要转换）
uint64_t BlowfishFilter::swapUint64(uint64_t value) const {
    // 检测是否是小端系统
    static const union {
        uint32_t i;
        uint8_t c[4];
    } test = {0x01020304};

    const bool isLittleEndian = (test.c[0] == 0x04);

    if (isLittleEndian) {
        // 在小端系统上需要转换为大端（网络字节序）
        return ((value & 0xFF00000000000000ULL) >> 56) |
               ((value & 0x00FF000000000000ULL) >> 40) |
               ((value & 0x0000FF0000000000ULL) >> 24) |
               ((value & 0x000000FF00000000ULL) >> 8)  |
               ((value & 0x00000000FF000000ULL) << 8)  |
               ((value & 0x0000000000FF0000ULL) << 24) |
               ((value & 0x000000000000FF00ULL) << 40) |
               ((value & 0x00000000000000FFULL) << 56);
    }

    return value;
}

// 执行Blowfish加密（单个8字节块）
void BlowfishFilter::blowfishEncrypt(uint8_t* dst, const uint8_t* src) const {
    // Blowfish算法期望数据是大端字节序
    uint32_t left = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
    uint32_t right = (src[4] << 24) | (src[5] << 16) | (src[6] << 8) | src[7];

    pBlowFish()->EncryptBlock(&left,&right);

    // 将结果转换回字节数组（大端）
    dst[0] = (left >> 24) & 0xFF;
    dst[1] = (left >> 16) & 0xFF;
    dst[2] = (left >> 8) & 0xFF;
    dst[3] = left & 0xFF;
    dst[4] = (right >> 24) & 0xFF;
    dst[5] = (right >> 16) & 0xFF;
    dst[6] = (right >> 8) & 0xFF;
    dst[7] = right & 0xFF;
}

// 执行Blowfish解密（单个8字节块）
void BlowfishFilter::blowfishDecrypt(uint8_t* dst, const uint8_t* src) const {
    // Blowfish算法期望数据是大端字节序
    uint32_t left = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
    uint32_t right = (src[4] << 24) | (src[5] << 16) | (src[6] << 8) | src[7];

    pBlowFish()->DecryptBlock(&left, &right);

    // 将结果转换回字节数组（大端）
    dst[0] = (left >> 24) & 0xFF;
    dst[1] = (left >> 16) & 0xFF;
    dst[2] = (left >> 8) & 0xFF;
    dst[3] = left & 0xFF;
    dst[4] = (right >> 24) & 0xFF;
    dst[5] = (right >> 16) & 0xFF;
    dst[6] = (right >> 8) & 0xFF;
    dst[7] = right & 0xFF;
}

BlowfishFilter::BlowfishFilter(int keySize)
    : isGood_(false)
    , pPacket_(new MemoryStream())
    , pEncryptStream_(new MemoryStream())
    , packetLen_(0)
    , padSize_(0)
    , keySize_(0)
    , pBlowFish_(nullptr) {

    generateRandomKey(keySize);
    keySize_ = static_cast<int>(key_.size());
    init();
}

BlowfishFilter::BlowfishFilter(const std::string& key)
    : isGood_(false)
    , pPacket_(new MemoryStream())
    , pEncryptStream_(new MemoryStream())
    , packetLen_(0)
    , padSize_(0)
    , key_(key)
    , keySize_(static_cast<int>(key.size()))
    , pBlowFish_(nullptr) {

    init();
}

BlowfishFilter::~BlowfishFilter() {
    KBE_SAFE_RELEASE(pPacket_);
    KBE_SAFE_RELEASE(pEncryptStream_);
    delete pBlowFish_;
}

bool BlowfishFilter::init() {
    pBlowFish_ = new Blowfish();

    if (MIN_KEY_SIZE <= keySize_ && keySize_ <= MAX_KEY_SIZE) {
        pBlowFish_->SetKey(reinterpret_cast<const unsigned char*>(key_.c_str()), keySize_);
        isGood_ = true;
    } else {
        ERROR_MSG("BlowfishFilter::init: invalid length %d", keySize_);
        isGood_ = false;
    }

    return isGood_;
}

void BlowfishFilter::encrypt(MemoryStream* pMemoryStream) {
    if (!pMemoryStream) return;

    // Blowfish 每次只能加密和解密8字节数据
    // 不足8字节则填充0
    uint8 padSize = 0;
    uint32 dataLen = pMemoryStream->length();

    if (dataLen % BLOCK_SIZE != 0) {
        padSize = static_cast<uint8>(BLOCK_SIZE - dataLen % BLOCK_SIZE);

        // 调整大小并填充0
        uint32 currentSize = pMemoryStream->size();
        pMemoryStream->data_resize(currentSize + padSize);
        memset(pMemoryStream->data() + pMemoryStream->wpos(), 0, padSize);
        pMemoryStream->wpos(pMemoryStream->wpos() + padSize);
    }

    // 加密数据
    if (pMemoryStream->length() > 0) {
        encrypt(pMemoryStream->data() + pMemoryStream->rpos(), pMemoryStream->length());
    }

    // 构建加密包：包长度(2字节) + 填充大小(1字节) + 加密数据
    pEncryptStream_->clear(false);
    uint16 packetLen = static_cast<uint16>(pMemoryStream->length() + 1);
    pEncryptStream_->writeUint16(packetLen);
    pEncryptStream_->writeUint8(padSize);
    pEncryptStream_->append(pMemoryStream->data() + pMemoryStream->rpos(), pMemoryStream->length());

    // 交换缓冲区
    pMemoryStream->swap(*pEncryptStream_);
    pEncryptStream_->clear(false);
}

void BlowfishFilter::encrypt(uint8 *buf, uint32 len) {
    if (len % BLOCK_SIZE != 0) {
        ERROR_MSG("BlowfishFilter::encrypt: Input length (%d) is not a multiple of block size ", len);
        return;
    }

    uint8* data = buf;
    uint64_t prevBlock = 0;

    for (uint32 i = 0; i < len; i += BLOCK_SIZE) {
        // 读取当前块
        uint64_t currentBlock = 0;
        memcpy(&currentBlock, data + i, BLOCK_SIZE);

        // 处理大小端：OpenSSL的BF_ecb_encrypt使用大端字节序
        currentBlock = swapUint64(currentBlock);

        if (prevBlock != 0) {
            uint64_t oldValue = currentBlock;
            currentBlock ^= prevBlock;
            prevBlock = oldValue;
        } else {
            prevBlock = currentBlock;
        }

        // 转换回大端字节序存储
        currentBlock = swapUint64(currentBlock);
        memcpy(data + i, &currentBlock, BLOCK_SIZE);

        // 执行加密
        uint8_t tempBlock[BLOCK_SIZE];
        blowfishEncrypt(tempBlock, data + i);
        memcpy(data + i, tempBlock, BLOCK_SIZE);
    }
}

void BlowfishFilter::encrypt(uint8 *buf, uint32 offset, uint32 len) {
    encrypt(buf + offset, len);
}

void BlowfishFilter::decrypt(MemoryStream* pMemoryStream) {
    if (!pMemoryStream) return;

    if (pMemoryStream->length() > 0) {
        decrypt(pMemoryStream->data() + pMemoryStream->rpos(), pMemoryStream->length());
    }
}

void BlowfishFilter::decrypt(uint8 *buf, uint32 len) {
    if (len % BLOCK_SIZE != 0) {
        ERROR_MSG("BlowfishFilter::decrypt: Input length (%d) is not a multiple of block size ", len);
        return;
    }

    uint8* data = buf;
    uint64_t prevBlock = 0;

    for (uint32 i = 0; i < len; i += BLOCK_SIZE) {
        // 执行解密
        uint8_t tempBlock[BLOCK_SIZE];
        blowfishDecrypt(tempBlock, data + i);
        memcpy(data + i, tempBlock, BLOCK_SIZE);

        // 读取解密后的块
        uint64_t currentBlock = 0;
        memcpy(&currentBlock, data + i, BLOCK_SIZE);

        // 处理大小端
        currentBlock = swapUint64(currentBlock);

        if (prevBlock != 0) {
            currentBlock ^= prevBlock;
            // 转换回大端字节序存储
            currentBlock = swapUint64(currentBlock);
            memcpy(data + i, &currentBlock, BLOCK_SIZE);
        } else {
            // 转换回大端字节序存储
            currentBlock = swapUint64(currentBlock);
            memcpy(data + i, &currentBlock, BLOCK_SIZE);
        }

        // 保存当前块的解密前值用于下一个块
        prevBlock = swapUint64(currentBlock);
    }
}

void BlowfishFilter::decrypt(uint8 *buf, uint32 offset, uint32 len) {
    decrypt(buf + offset, len);
}

bool BlowfishFilter::send(NetworkInterfaceBase* pPacketSender, MemoryStream* pPacket) {
    if (!isGood_) {
        ERROR_MSG("BlowfishFilter::send: Dropping packet due to invalid filter");
        return false;
    }

    encrypt(pPacket);
    // 这里需要实现PacketSenderBase::send接口
    // return pPacketSender->send(pPacket);
    return pPacketSender->sendTo(pPacket);
}

bool BlowfishFilter::recv(MessageReader* pMessageReader, MemoryStream* pPacket) {
    if (!isGood_) {
        ERROR_MSG("BlowfishFilter::recv: Dropping packet due to invalid filter");
        return false;
    }

    uint32 oldrpos = pPacket->rpos();
    uint32 len = pPacket->length();
    uint16 packeLen = pPacket->readUint16();

    // 检查是否为一个完整的包
    if (len > MIN_PACKET_SIZE) {

        if (0 == pPacket_->length() && packeLen - 1 == len - 3) {
            uint8 padSize = pPacket->readUint8();
            decrypt(pPacket);

            // 移除填充字节
            // if (padSize > 0 && padSize <= pPacket->length()) {
            //     uint32 newWpos = pPacket->wpos() - padSize;
            //     pPacket->wpos(newWpos);
            // }

            // 处理消息
            if (pMessageReader) {
                // 这里需要实现MessageReader::process接口
                pMessageReader->process(pPacket->data() + pPacket->rpos(), 0, pPacket->length() - padSize);
            }

            pPacket->clear(false);
            return true;
        }
    }

    // 重置读位置，处理分片包
    pPacket->rpos(oldrpos);
    pPacket_->append(pPacket->data() + pPacket->rpos(), pPacket->length());
    pPacket->clear(false);

    // 处理分片包逻辑
    while (pPacket_->length() > 0) {
        uint32 currLen = 0;
        int oldwpos = 0;

        if (packetLen_ <= 0) {
            if (pPacket_->length() >= MIN_PACKET_SIZE) {
                (*pPacket_) >> packetLen_;
                (*pPacket_) >> padSize_;

                packetLen_ -= 1;

                if (pPacket_->length() > packetLen_) {
                    currLen = pPacket_->rpos() + packetLen_;
                    oldwpos = pPacket_->wpos();
                    pPacket_->wpos(currLen);
                } else if (pPacket_->length() < packetLen_) {
                    return false;
                }
            } else {
                return false;
            }
        } else {
            if (pPacket_->length() > packetLen_) {
                currLen = pPacket_->rpos() + packetLen_;
                oldwpos = pPacket_->wpos();
                pPacket_->wpos(currLen);
            } else if (pPacket_->length() < packetLen_) {
                return false;
            }
        }

        // 解密数据
        decrypt(pPacket_);

        // 移除填充字节
        if (padSize_ > 0 && padSize_ <= pPacket_->length()) {
            uint32 newWpos = pPacket_->wpos() - padSize_;
            pPacket_->wpos(newWpos);
        }

        // 处理消息
        if (pMessageReader) {
            // 这里需要实现MessageReader::process接口
            pMessageReader->process(pPacket_->data() + pPacket_->rpos(), 0, pPacket_->length());
        }

        // 处理剩余数据
        if (currLen > 0) {
            pPacket_->rpos(currLen);
            pPacket_->wpos(oldwpos);
        } else {
            pPacket_->clear(false);
        }

        packetLen_ = 0;
        padSize_ = 0;
    }

    return true;
}

} // namespace KBEngine