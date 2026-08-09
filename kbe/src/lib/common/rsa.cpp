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

#include "rsa.h"
#include "common.h"
#include "helper/debug_helper.h"

#include <cstdio>
#include <cstring>
#include <openssl/bn.h>
#include <openssl/decoder.h>
#include <openssl/encoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace KBEngine
{

namespace
{
	EVP_PKEY* loadKeyFromPemFile(const std::string& keyname, int selection)
	{
		BIO* bio = BIO_new_file(keyname.c_str(), "rb");
		if (bio == NULL)
		{
			return NULL;
		}

		EVP_PKEY* key = NULL;
		OSSL_DECODER_CTX* ctx = OSSL_DECODER_CTX_new_for_pkey(&key, "PEM", NULL, NULL,
			selection, NULL, NULL);
		if (ctx == NULL)
		{
			BIO_free(bio);
			return NULL;
		}

		const int ret = OSSL_DECODER_from_bio(ctx, bio);
		OSSL_DECODER_CTX_free(ctx);
		BIO_free(bio);
		return ret == 1 ? key : NULL;
	}

	bool writeKeyToPemFile(const std::string& keyname, EVP_PKEY* key, int selection)
	{
		BIO* bio = BIO_new_file(keyname.c_str(), "wb");
		if (bio == NULL)
		{
			return false;
		}

		OSSL_ENCODER_CTX* ctx = OSSL_ENCODER_CTX_new_for_pkey(key, selection, "PEM", NULL, NULL);
		if (ctx == NULL)
		{
			BIO_free(bio);
			return false;
		}

		const int ret = OSSL_ENCODER_to_bio(ctx, bio);
		OSSL_ENCODER_CTX_free(ctx);
		BIO_free(bio);
		return ret == 1;
	}

	const char* errorString()
	{
		static thread_local char err[1024];
		return ERR_error_string(ERR_get_error(), err);
	}
}

//-------------------------------------------------------------------------------------
KBE_RSA::KBE_RSA(const std::string& pubkeyname, const std::string& prikeyname):
rsa_public(0),
rsa_private(0)
{
	if(pubkeyname.size() > 0 || prikeyname.size() > 0)
	{
		KBE_ASSERT(pubkeyname.size() > 0);
		KBE_ASSERT(prikeyname.size() > 0);

		bool key = loadPrivate(prikeyname) && loadPublic(pubkeyname);
		KBE_ASSERT(key);
	}
}

//-------------------------------------------------------------------------------------
KBE_RSA::KBE_RSA():
rsa_public(0),
rsa_private(0)
{
}

//-------------------------------------------------------------------------------------
KBE_RSA::~KBE_RSA()
{
	if(rsa_public != NULL)
	{
		EVP_PKEY_free(static_cast<EVP_PKEY*>(rsa_public));
		rsa_public = NULL;
	}

	if(rsa_private != NULL)
	{
		EVP_PKEY_free(static_cast<EVP_PKEY*>(rsa_private));
		rsa_private = NULL;
	}
}

//-------------------------------------------------------------------------------------
bool KBE_RSA::loadPublic(const std::string& keyname)
{
	if(rsa_public != NULL)
	{
		return true;
	}

	rsa_public = loadKeyFromPemFile(keyname, OSSL_KEYMGMT_SELECT_PUBLIC_KEY);
	if(NULL == rsa_public)
	{
		ERROR_MSG(fmt::format("KBE_RSA::loadPublic: loadKeyFromPemFile error({})\n", errorString()));
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool KBE_RSA::loadPrivate(const std::string& keyname)
{
	if(rsa_private != NULL)
	{
		return true;
	}

	rsa_private = loadKeyFromPemFile(keyname, OSSL_KEYMGMT_SELECT_KEYPAIR);
	if(NULL == rsa_private)
	{
		ERROR_MSG(fmt::format("KBE_RSA::loadPrivate: loadKeyFromPemFile error({})\n", errorString()));
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool KBE_RSA::generateKey(const std::string& pubkeyname,
						  const std::string& prikeyname, int keySize, int e)
{
	KBE_ASSERT(rsa_public == NULL && rsa_private == NULL);

	EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	if (ctx == NULL)
	{
		return false;
	}

	EVP_PKEY* pkey = NULL;
	BIGNUM* bne = BN_new();
	if (bne == NULL || BN_set_word(bne, static_cast<BN_ULONG>(e)) != 1 ||
		EVP_PKEY_keygen_init(ctx) <= 0 ||
		EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, keySize) <= 0 ||
		EVP_PKEY_CTX_set1_rsa_keygen_pubexp(ctx, bne) <= 0 ||
		EVP_PKEY_keygen(ctx, &pkey) <= 0)
	{
		ERROR_MSG(fmt::format("KBE_RSA::generateKey: EVP_PKEY_keygen error({})\n", errorString()));

		BN_free(bne);
		EVP_PKEY_CTX_free(ctx);
		if (pkey != NULL)
		{
			EVP_PKEY_free(pkey);
		}
		return false;
	}

	BN_free(bne);
	EVP_PKEY_CTX_free(ctx);

	if (!writeKeyToPemFile(prikeyname, pkey, OSSL_KEYMGMT_SELECT_KEYPAIR) ||
		!writeKeyToPemFile(pubkeyname, pkey, OSSL_KEYMGMT_SELECT_PUBLIC_KEY))
	{
		ERROR_MSG("KBE_RSA::generateKey: write pem file failed.\n");
		EVP_PKEY_free(pkey);
		return false;
	}

	EVP_PKEY_up_ref(pkey);
	rsa_private = pkey;
	rsa_public = pkey;

	INFO_MSG(fmt::format("KBE_RSA::generateKey: RSA key generated. keysize({}) bits.\n", keySize));
	return true;
}

//-------------------------------------------------------------------------------------
std::string KBE_RSA::encrypt(const std::string& instr)
{
	std::string encrypted;
	if(encrypt(instr, encrypted) < 0)
		return "";

	char strencrypted[1024];
	memset(strencrypted, 0, sizeof(strencrypted));
	strutil::bytes2string((unsigned char *)encrypted.data(), static_cast<int>(encrypted.size()), (unsigned char *)strencrypted, sizeof(strencrypted));
	return strencrypted;
}

//-------------------------------------------------------------------------------------
int KBE_RSA::encrypt(const std::string& instr, std::string& outCertifdata)
{
	KBE_ASSERT(rsa_public != NULL);

	EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(static_cast<EVP_PKEY*>(rsa_public), NULL);
	if (ctx == NULL)
	{
		return -1;
	}

	if (EVP_PKEY_encrypt_init(ctx) <= 0 ||
		EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return -1;
	}

	size_t outlen = 0;
	if (EVP_PKEY_encrypt(ctx, NULL, &outlen,
		reinterpret_cast<const unsigned char*>(instr.data()), instr.size()) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return -1;
	}

	outCertifdata.resize(outlen);
	if (EVP_PKEY_encrypt(ctx, reinterpret_cast<unsigned char*>(&outCertifdata[0]), &outlen,
		reinterpret_cast<const unsigned char*>(instr.data()), instr.size()) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		outCertifdata.clear();
		return -1;
	}

	EVP_PKEY_CTX_free(ctx);
	outCertifdata.resize(outlen);
	return static_cast<int>(outlen);
}

//-------------------------------------------------------------------------------------
void KBE_RSA::hexCertifData(const std::string& inCertifdata)
{
	std::string s = "KBE_RSA::encrypt: encrypted string = \n";

	for (int i=0; i<(int)inCertifdata.size(); ++i) {
		s += fmt::format("{:x}{:x}", ((inCertifdata.data()[i] >> 4) & 0xf),
			(inCertifdata.data()[i] & 0xf));
	}

	s += "\n";

	INFO_MSG(s.c_str());
}

//-------------------------------------------------------------------------------------
int KBE_RSA::decrypt(const std::string& inCertifdata, std::string& outstr)
{
	KBE_ASSERT(rsa_private != NULL);

	EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(static_cast<EVP_PKEY*>(rsa_private), NULL);
	if (ctx == NULL)
	{
		return -1;
	}

	if (EVP_PKEY_decrypt_init(ctx) <= 0 ||
		EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return -1;
	}

	size_t outlen = 0;
	if (EVP_PKEY_decrypt(ctx, NULL, &outlen,
		reinterpret_cast<const unsigned char*>(inCertifdata.data()), inCertifdata.size()) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return -1;
	}

	outstr.resize(outlen);
	if (EVP_PKEY_decrypt(ctx, reinterpret_cast<unsigned char*>(&outstr[0]), &outlen,
		reinterpret_cast<const unsigned char*>(inCertifdata.data()), inCertifdata.size()) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		outstr.clear();
		return -1;
	}

	EVP_PKEY_CTX_free(ctx);
	outstr.resize(outlen);
	return static_cast<int>(outlen);
}

//-------------------------------------------------------------------------------------
std::string KBE_RSA::decrypt(const std::string& instr)
{
	unsigned char strencrypted[1024];
	memset(strencrypted, 0, sizeof(strencrypted));
	strutil::string2bytes((unsigned char *)instr.data(), (unsigned char *)&strencrypted[0], sizeof(strencrypted));
	std::string encrypted;
	encrypted.assign((char*)strencrypted, sizeof(strencrypted));

	std::string out;
	if(decrypt(encrypted, out) < 0)
		return "";

	return out;
}

//-------------------------------------------------------------------------------------
}
