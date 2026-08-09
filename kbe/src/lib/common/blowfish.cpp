/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "blowfish.h"
#include "helper/debug_helper.h"
#include "openssl/evp.h"
#include "openssl/provider.h"
#include "openssl/rand.h"
#include <mutex>

namespace KBEngine { 

namespace {

std::once_flag g_blowfishProviderOnce;

void ensureBlowfishProvider()
{
	std::call_once(g_blowfishProviderOnce, []()
	{
		// Blowfish is provided by OpenSSL 3's legacy provider. Loading default as
		// well keeps the process provider set complete after an explicit load.
		OSSL_PROVIDER_load(NULL, "legacy");
		OSSL_PROVIDER_load(NULL, "default");
	});
}

EVP_CIPHER_CTX* createBlowfishContext(const KBEBlowfish::Key& key, bool encrypt)
{
	ensureBlowfishProvider();

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL)
		return NULL;

	const int enc = encrypt ? 1 : 0;
	if (EVP_CipherInit_ex(ctx, EVP_bf_ecb(), NULL, NULL, NULL, enc) != 1 ||
		EVP_CIPHER_CTX_set_key_length(ctx, static_cast<int>(key.size())) != 1 ||
		EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
		EVP_CipherInit_ex(ctx, NULL, NULL, reinterpret_cast<const unsigned char*>(key.data()), NULL, enc) != 1)
	{
		EVP_CIPHER_CTX_free(ctx);
		return NULL;
	}

	return ctx;
}

bool processBlowfishBlock(EVP_CIPHER_CTX* ctx, const unsigned char* src, unsigned char* dest)
{
	unsigned char block[KBEBlowfish::BLOCK_SIZE];
	int outLen = 0;
	if (EVP_CipherUpdate(ctx, block, &outLen, src, KBEBlowfish::BLOCK_SIZE) != 1 ||
		outLen != KBEBlowfish::BLOCK_SIZE)
	{
		return false;
	}

	memcpy(dest, block, KBEBlowfish::BLOCK_SIZE);
	return true;
}

bool finishBlowfishContext(EVP_CIPHER_CTX* ctx)
{
	unsigned char finalBlock[KBEBlowfish::BLOCK_SIZE];
	int finalLen = 0;
	return EVP_CipherFinal_ex(ctx, finalBlock, &finalLen) == 1 && finalLen == 0;
}

}

//-------------------------------------------------------------------------------------
KBEBlowfish::KBEBlowfish(const Key & key):
key_(key),
keySize_(key.size()),
isGood_(false)
{
	init();
}

//-------------------------------------------------------------------------------------
KBEBlowfish::KBEBlowfish(int keySize):
	key_(keySize, 0),
	keySize_(static_cast<size_t>(keySize)),
	isGood_(false)
{
	RAND_bytes((unsigned char*)const_cast<char *>(key_.c_str()), 
		static_cast<int>(key_.size()));

	this->init();
}

//-------------------------------------------------------------------------------------
KBEBlowfish::~KBEBlowfish()
{
}

//-------------------------------------------------------------------------------------
bool KBEBlowfish::init()
{
	if ((MIN_KEY_SIZE <= keySize_) && (keySize_ <= MAX_KEY_SIZE))
	{
		isGood_ = true;
	}
	else
	{
		ERROR_MSG(fmt::format("KBEBlowfish::init: "
			"invalid length {}\n",
			keySize_));

		isGood_ = false;
	}

	return isGood_;
}

//-------------------------------------------------------------------------------------
const char * KBEBlowfish::strBlowFishKey() const
{
	static char buf[1024];
	char *c = buf;

	for (size_t i = 0; i < keySize_; ++i)
	{
		c += kbe_snprintf(c, sizeof(buf) - static_cast<size_t>(c - buf), "%02hhX ", (unsigned char)key_[i]);
	}

	if (c > buf)
		c[-1] = '\0';
	else
		*c = '\0';

	return buf;
}

//-------------------------------------------------------------------------------------
int KBEBlowfish::encrypt( const unsigned char * src, unsigned char * dest,
	int length )
{
	// BLOCK_SIZE的整数倍
	if(length % BLOCK_SIZE != 0)
	{
		CRITICAL_MSG(fmt::format("Blowfish::encrypt: "
			"Input length ({}) is not a multiple of block size ({})\n",
			length, (int)(BLOCK_SIZE)));

		return -1;
	}

	EVP_CIPHER_CTX* ctx = createBlowfishContext(key_, true);
	if (ctx == NULL)
	{
		ERROR_MSG("Blowfish::encrypt: create EVP cipher context failed.\n");
		return -1;
	}

	uint64 prevBlock = 0;
	bool hasPrevBlock = false;
	for (int i=0; i < length; i += BLOCK_SIZE)
	{
		uint64 currentBlock = *(uint64*)(src + i);
		if (hasPrevBlock)
		{
			*(uint64*)(dest + i) = currentBlock ^ prevBlock;
		}
		else
		{
			*(uint64*)(dest + i) = currentBlock;
		}

		if (!processBlowfishBlock(ctx, dest + i, dest + i))
		{
			EVP_CIPHER_CTX_free(ctx);
			ERROR_MSG("Blowfish::encrypt: EVP_CipherUpdate failed.\n");
			return -1;
		}

		prevBlock = currentBlock;
		hasPrevBlock = true;
	}

	if (!finishBlowfishContext(ctx))
	{
		EVP_CIPHER_CTX_free(ctx);
		ERROR_MSG("Blowfish::encrypt: EVP_CipherFinal_ex failed.\n");
		return -1;
	}

	EVP_CIPHER_CTX_free(ctx);

	return length;
}

//-------------------------------------------------------------------------------------
int KBEBlowfish::decrypt( const unsigned char * src, unsigned char * dest,
	int length )
{
	if (length % BLOCK_SIZE != 0)
	{
		ERROR_MSG(fmt::format("Blowfish::decrypt: "
			"Input stream size ({}) is not a multiple of the block size ({})\n",
			length, (int)(BLOCK_SIZE)));

		return -1;
	}

	EVP_CIPHER_CTX* ctx = createBlowfishContext(key_, false);
	if (ctx == NULL)
	{
		ERROR_MSG("Blowfish::decrypt: create EVP cipher context failed.\n");
		return -1;
	}

	uint64 * pPrevBlock = NULL;
	for (int i=0; i < length; i += BLOCK_SIZE)
	{
		if (!processBlowfishBlock(ctx, src + i, dest + i))
		{
			EVP_CIPHER_CTX_free(ctx);
			ERROR_MSG("Blowfish::decrypt: EVP_CipherUpdate failed.\n");
			return -1;
		}

		if (pPrevBlock)
		{
			*(uint64*)(dest + i) ^= *pPrevBlock;
		}

		pPrevBlock = (uint64*)(dest + i);
	}

	if (!finishBlowfishContext(ctx))
	{
		EVP_CIPHER_CTX_free(ctx);
		ERROR_MSG("Blowfish::decrypt: EVP_CipherFinal_ex failed.\n");
		return -1;
	}

	EVP_CIPHER_CTX_free(ctx);

	return length;
}

//-------------------------------------------------------------------------------------

}
