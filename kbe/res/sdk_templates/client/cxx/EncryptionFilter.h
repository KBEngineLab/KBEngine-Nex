#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include "MemoryStream.h"

class Blowfish;

namespace KBEngine {
    class NetworkInterfaceBase;

    class MessageReader;

    class EncryptionFilter {
    public:
        virtual ~EncryptionFilter() {}
        virtual bool send(NetworkInterfaceBase* pPacketSender, MemoryStream* pPacket) = 0;
        virtual bool recv(MessageReader* pMessageReader, MemoryStream* pPacket) = 0;
    };

    class BlowfishFilter : public EncryptionFilter {
    public:
        // 构造函数：生成随机密钥
        BlowfishFilter(int keySize = 20);

        // 构造函数：使用指定密钥
        explicit BlowfishFilter(const std::string& key);

        ~BlowfishFilter() override;

        // 初始化
        bool init();

        // 加密数据
        void encrypt(MemoryStream* pMemoryStream);
        void encrypt(uint8 *buf, uint32 len);
        void encrypt(uint8 *buf, uint32 offset, uint32 len);

        // 解密数据
        void decrypt(MemoryStream* pMemoryStream);
        void decrypt(uint8 *buf, uint32 len);
        void decrypt(uint8 *buf, uint32 offset, uint32 len);

        // 发送/接收接口
        bool send(NetworkInterfaceBase* pPacketSender, MemoryStream* pPacket) override;
        bool recv(MessageReader* pMessageReader, MemoryStream* pPacket) override;

        // 获取密钥
        const std::string& getKey() const { return key_; }

        KBArray<uint8> key()
        {
            KBArray<uint8> keyArray;
            keyArray.SetNum(key_.size());
            memcpy(keyArray.GetData(), key_.c_str(), key_.size());

            return keyArray;
        }

    private:
        static const int MIN_KEY_SIZE = 4;
        static const int MAX_KEY_SIZE = 56;
        static const int BLOCK_SIZE = 8;          // Blowfish块大小
        static const int MIN_PACKET_SIZE = 3;     // 最小包大小：uint16 + uint8

        bool isGood_;
        MemoryStream* pPacket_;
        MemoryStream* pEncryptStream_;
        uint16 packetLen_;
        uint8 padSize_;
        std::string key_;
        int keySize_;
        Blowfish* pBlowFish_;

        // 辅助函数
        Blowfish* pBlowFish() { return pBlowFish_; }
        const Blowfish* pBlowFish() const { return pBlowFish_; }

        // 生成随机密钥
        void generateRandomKey(int keySize);

        // 大小端转换辅助函数
        uint64_t swapUint64(uint64_t value) const;

        // 执行Blowfish加密（确保与OpenSSL一致）
        void blowfishEncrypt(uint8_t* dst, const uint8_t* src) const;
        void blowfishDecrypt(uint8_t* dst, const uint8_t* src) const;
    };

} // namespace KBEngine