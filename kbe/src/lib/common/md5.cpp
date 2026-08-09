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

#include "md5.h"
#include "common.h"
#include "helper/debug_helper.h"
#include <cstdio>
#include <cstring>

namespace KBEngine
{

//-------------------------------------------------------------------------------------
KBE_MD5::KBE_MD5()
{
	state_ = EVP_MD_CTX_new();
	KBE_ASSERT(state_ != NULL);
	EVP_DigestInit_ex(state_, EVP_md5(), NULL);
	isFinal_ = false;
}

//-------------------------------------------------------------------------------------
KBE_MD5::KBE_MD5(const void * data, size_t numBytes)
{
	state_ = EVP_MD_CTX_new();
	KBE_ASSERT(state_ != NULL);
	EVP_DigestInit_ex(state_, EVP_md5(), NULL);
	isFinal_ = false;

	append(data, numBytes);
}

//-------------------------------------------------------------------------------------
KBE_MD5::~KBE_MD5()
{
	if(state_ != NULL)
	{
		EVP_MD_CTX_free(state_);
		state_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
void KBE_MD5::append(const void * data, size_t numBytes)
{
	KBE_ASSERT(state_ != NULL);
	EVP_DigestUpdate(state_, data, numBytes);
}

//-------------------------------------------------------------------------------------
const unsigned char* KBE_MD5::getDigest()
{
	final();
	return bytes_;
}

//-------------------------------------------------------------------------------------
std::string KBE_MD5::getDigestStr()
{
	const unsigned char* md = getDigest();

	char tmp[3]={'\0'}, md5str[33] = {'\0'};
	for (int i = 0; i < 16; ++i)
	{
		std::snprintf(tmp, sizeof(tmp), "%02X", md[i]);
		std::strcat(md5str, tmp);
	}

	return md5str;
}

//-------------------------------------------------------------------------------------
void KBE_MD5::final()
{
	if(!isFinal_)
	{
		unsigned int len = 0;
		KBE_ASSERT(state_ != NULL);
		EVP_DigestFinal_ex(state_, bytes_, &len);
		KBE_ASSERT(len == sizeof(bytes_));
		isFinal_ = true;
	}
}

//-------------------------------------------------------------------------------------
void KBE_MD5::clear()
{
	if(state_ == NULL)
	{
		state_ = EVP_MD_CTX_new();
	}
	else
	{
		EVP_MD_CTX_reset(state_);
	}

	KBE_ASSERT(state_ != NULL);
	EVP_DigestInit_ex(state_, EVP_md5(), NULL);
	memset(bytes_, 0, sizeof(bytes_));
	isFinal_ = false;
}

//-------------------------------------------------------------------------------------
bool KBE_MD5::operator==(const KBE_MD5 & other) const
{
	return memcmp(this->bytes_, other.bytes_, sizeof(bytes_)) == 0;
}

//-------------------------------------------------------------------------------------
bool KBE_MD5::operator<(const KBE_MD5 & other) const
{
	return memcmp(this->bytes_, other.bytes_, sizeof(bytes_)) < 0;
}

//-------------------------------------------------------------------------------------
std::string KBE_MD5::getDigest(const void * data, size_t numBytes)
{
	KBE_MD5 md5 = KBE_MD5(data, numBytes);
	return md5.getDigestStr();
}

//-------------------------------------------------------------------------------------
} 
