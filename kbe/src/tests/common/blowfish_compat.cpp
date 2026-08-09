#include "common/blowfish.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace
{

constexpr std::array<unsigned char, 20> kKey = {
	0x03, 0x0A, 0x11, 0x18, 0x1F, 0x26, 0x2D, 0x34, 0x3B, 0x42,
	0x49, 0x50, 0x57, 0x5E, 0x65, 0x6C, 0x73, 0x7A, 0x81, 0x88
};

constexpr std::array<unsigned char, 32> kPlain = {
	0x01, 0x06, 0x0B, 0x10, 0x15, 0x1A, 0x1F, 0x24,
	0x29, 0x2E, 0x33, 0x38, 0x3D, 0x42, 0x47, 0x4C,
	0x51, 0x56, 0x5B, 0x60, 0x65, 0x6A, 0x6F, 0x74,
	0x79, 0x7E, 0x83, 0x88, 0x8D, 0x92, 0x97, 0x9C
};

constexpr std::array<unsigned char, 32> kExpectedCipher = {
	0x66, 0x4B, 0x6F, 0x91, 0x2C, 0x24, 0x5C, 0xE9,
	0xFA, 0x35, 0x34, 0xE0, 0x6F, 0x99, 0xCD, 0x0A,
	0x7D, 0x73, 0xEC, 0x19, 0x17, 0x35, 0x04, 0xB7,
	0x43, 0x38, 0x8D, 0xEA, 0xED, 0x57, 0x4E, 0x02
};

bool sameBytes(const std::array<unsigned char, 32>& lhs, const std::array<unsigned char, 32>& rhs)
{
	return std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

}

int main()
{
	KBEngine::KBEBlowfish blowfish(std::string(reinterpret_cast<const char*>(kKey.data()), kKey.size()));
	if (!blowfish.isGood())
	{
		std::printf("BLOWFISH_COMPAT_TEST_FAIL init\n");
		return 1;
	}

	std::array<unsigned char, 32> encrypted = {};
	if (blowfish.encrypt(kPlain.data(), encrypted.data(), static_cast<int>(encrypted.size())) != static_cast<int>(encrypted.size()) ||
		!sameBytes(encrypted, kExpectedCipher))
	{
		std::printf("BLOWFISH_COMPAT_TEST_FAIL encrypt\n");
		return 1;
	}

	std::array<unsigned char, 32> encryptedInPlace = kPlain;
	if (blowfish.encrypt(encryptedInPlace.data(), encryptedInPlace.data(), static_cast<int>(encryptedInPlace.size())) != static_cast<int>(encryptedInPlace.size()) ||
		!sameBytes(encryptedInPlace, kExpectedCipher))
	{
		std::printf("BLOWFISH_COMPAT_TEST_FAIL encrypt-in-place\n");
		return 1;
	}

	std::array<unsigned char, 32> decrypted = encrypted;
	if (blowfish.decrypt(decrypted.data(), decrypted.data(), static_cast<int>(decrypted.size())) != static_cast<int>(decrypted.size()) ||
		!sameBytes(decrypted, kPlain))
	{
		std::printf("BLOWFISH_COMPAT_TEST_FAIL decrypt\n");
		return 1;
	}

	std::printf("BLOWFISH_COMPAT_TEST_PASS\n");
	return 0;
}
