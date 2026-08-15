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

#include "common.h"
#include "strutil.h"
#include <algorithm>
#include <limits>
#include <algorithm>
#include <utility>
#include <functional>
#include <cctype>
#include <utf8.h>
#include "memorystream.h"

namespace KBEngine{ 
namespace strutil {

	int bytes2string(unsigned char *src, int srcsize, unsigned char *dst, int dstsize)     
	{     
		if (dst != NULL)  
		{  
			*dst = 0;  
		}  
	      
		if (src == NULL || srcsize <= 0 || dst == NULL || dstsize <= srcsize * 2)  
		{  
			return 0;  
		}  
	      
		const char szTable[] = "0123456789ABCDEF";

		for(int i=0; i<srcsize; ++i)     
		{     
			*dst++ = szTable[src[i] >> 4];     
			*dst++ = szTable[src[i] & 0x0f];   
		}     
 
		*dst = 0;      
		return  srcsize * 2;     
	}

	int string2bytes(unsigned char* src, unsigned char* dst, int dstsize)     
	{  
		if(src == NULL)
			return 0;  

		int iLen = (int)strlen((char *)src);
		if (iLen <= 0 || iLen%2 != 0 || dst == NULL || dstsize < iLen/2)  
		{  
			return 0;  
		}  
	      
		iLen /= 2;  
		str_toupper((char *)src); 
		for (int i=0; i<iLen; ++i)  
		{  
			int iVal = 0;  
			unsigned char *pSrcTemp = src + i*2;  
			sscanf((char *)pSrcTemp, "%02x", &iVal);  
			dst[i] = (unsigned char)iVal;  
		}  
	      
		return iLen;  
	}

    std::string toLower(const std::string& str) {
        std::string t = str;
		// C locale 分类函数只接受 EOF 或 unsigned char 范围，先转换可避免 UTF-8 高位字节在 MSVC Debug CRT 中触发断言。
		// C locale classifiers accept only EOF or unsigned-char values; conversion prevents UTF-8 bytes from asserting in the MSVC Debug CRT.
		std::transform(t.begin(), t.end(), t.begin(),
			[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return t;
    }

    std::string toUpper(const std::string& str) {
        std::string t = str;
		// 保持非 ASCII 字节原样，只让当前 locale 对合法范围内的单字节字符执行大小写转换。
		// Preserve non-ASCII bytes while allowing the active locale to convert valid single-byte characters.
		std::transform(t.begin(), t.end(), t.begin(),
			[](unsigned char value) { return static_cast<char>(std::toupper(value)); });
        return t;
    }

	std::string &kbe_ltrim(std::string &s) 
	{
		// 空白判断同样必须先提升为 unsigned char，否则 UTF-8 路径和 XML 文本可能触发未定义行为。
		// Whitespace checks require the same unsigned-char promotion to avoid undefined behavior in UTF-8 paths and XML text.
		s.erase(s.begin(), std::find_if(s.begin(), s.end(),
			[](unsigned char value) { return std::isspace(value) == 0; }));
		return s;
	}

	std::string &kbe_rtrim(std::string &s) 
	{
		s.erase(std::find_if(s.rbegin(), s.rend(),
			[](unsigned char value) { return std::isspace(value) == 0; }).base(), s.end());
		return s;
	}

	std::string kbe_trim(std::string s) 
	{
		return kbe_ltrim(kbe_rtrim(s));
	}

	// 字符串替换
	int kbe_replace(std::string& str,  const std::string& pattern,  const std::string& newpat) 
	{ 
		int count = 0; 
		const size_t nsize = newpat.size(); 
		const size_t psize = pattern.size(); 

		for(size_t pos = str.find(pattern, 0);  
			pos != std::string::npos; 
			pos = str.find(pattern,pos + nsize)) 
		{ 
			str.replace(pos, psize, newpat); 
			count++; 
		} 

		return count; 
	}

	int kbe_replace(std::wstring& str,  const std::wstring& pattern,  const std::wstring& newpat) 
	{ 
		int count = 0; 
		const size_t nsize = newpat.size(); 
		const size_t psize = pattern.size(); 

		for(size_t pos = str.find(pattern, 0);  
			pos != std::wstring::npos; 
			pos = str.find(pattern,pos + nsize)) 
		{ 
			str.replace(pos, psize, newpat); 
			count++; 
		} 

		return count; 
	}


	size_t kbe_splits(const std::string& s, const std::string& delim, std::vector< std::string >& out_result, const bool keep_empty)
	{
		if (delim.empty()) {
			out_result.push_back(s);
			return out_result.size();
		}

		std::string::const_iterator substart = s.begin(), subend;

		while (true) {
			subend = std::search(substart, s.end(), delim.begin(), delim.end());
			std::string temp(substart, subend);
			if (keep_empty || !temp.empty()) {
				out_result.push_back(temp);
			}
			if (subend == s.end()) {
				break;
			}
			substart = subend + delim.size();
		}

		return out_result.size();
	}

	char* wchar2char(const wchar_t* ts, size_t* outlen)
	{
		int len = (int)((wcslen(ts) + 1) * sizeof(wchar_t));
		char* ccattr =(char *)malloc(len);
		memset(ccattr, 0, len);

		size_t slen = wcstombs(ccattr, ts, len);

		if (outlen)
		{
			if ((size_t)-1 != slen)
				*outlen = slen;
			else
				*outlen = 0;
		}

		return ccattr;
	};

	void wchar2char(const wchar_t* ts, MemoryStream* pOutStream)
	{
		int len = (int)((wcslen(ts) + 1) * sizeof(wchar_t));
		pOutStream->data_resize(pOutStream->wpos() + len);
		size_t slen = wcstombs((char*)&pOutStream->data()[pOutStream->wpos()], ts, len);
		
		if((size_t)-1 != slen)
		{
			pOutStream->wpos(pOutStream->wpos() + slen + 1);
			pOutStream->data()[pOutStream->wpos() - 1] = 0;
		}
	};

	wchar_t* char2wchar(const char* cs, size_t* outlen)
	{
		// A multibyte string cannot produce more wide characters than input bytes.
		// mbstowcs() takes a wchar_t element count, not an allocation size in bytes;
		// passing the byte count trips glibc Fortify and can permit a real overflow.
		// 多字节字符串转换后的宽字符数不会超过输入字节数。mbstowcs() 的第三个参数
		// 是 wchar_t 元素数而非分配字节数；传入字节数会触发 glibc Fortify，并可能真实越界。
		const size_t capacity = strlen(cs) + 1;
		const size_t allocationBytes = capacity * sizeof(wchar_t);
		wchar_t* ccattr = (wchar_t*)malloc(allocationBytes);
		if (ccattr == NULL)
		{
			if (outlen)
				*outlen = 0;
			return NULL;
		}
		memset(ccattr, 0, allocationBytes);

		size_t slen = mbstowcs(ccattr, cs, capacity);

		if (outlen)
		{
			if ((size_t)-1 != slen)
				*outlen = slen;
			else
				*outlen = 0;
		}
		
		return ccattr;
	};

	/*
	int wchar2utf8(const wchar_t* in, int in_len, char* out, int out_max)   
	{   
	#if KBE_PLATFORM == PLATFORM_WIN32   
		BOOL use_def_char;   
		use_def_char = FALSE;   
		return ::WideCharToMultiByte(CP_UTF8, 0, in,in_len / sizeof(wchar_t), out, out_max, NULL, NULL);   
	#else   
		size_t result;   
		iconv_t env;   
	   
		env = iconv_open("UTF8", "WCHAR_T");   
		result = iconv(env,(char**)&in,(size_t*)&in_len,(char**)&out,(size_t*)&out_max);        
		iconv_close(env);   
		return (int) result;   
	#endif   
	}   
	   
	int wchar2utf8(const std::wstring& in, std::string& out)   
	{   
		int len = in.length() + 1;   
		int result;   

		char* pBuffer = new char[len * 4];   

		memset(pBuffer,0,len * 4);               

		result = wchar2utf8(in.c_str(), in.length() * sizeof(wchar_t), pBuffer,len * 4);   

		if(result >= 0)   
		{   
			out = pBuffer;   
		}   
		else   
		{   
			out = "";   
		}   

		delete[] pBuffer;   
		return result;   
	}   
	   
	int utf82wchar(const char* in, int in_len, wchar_t* out, int out_max)   
	{   
	#if KBE_PLATFORM == PLATFORM_WIN32   
		return ::MultiByteToWideChar(CP_UTF8, 0, in, in_len, out, out_max);   
	#else   
		size_t result;   
		iconv_t env;   
		env = iconv_open("WCHAR_T", "UTF8");   
		result = iconv(env,(char**)&in, (size_t*)&in_len, (char**)&out,(size_t*)&out_max);   
		iconv_close(env);   
		return (int) result;   
	#endif   
	}   
	   
	int utf82wchar(const std::string& in, std::wstring& out)   
	{   
		int len = in.length() + 1;   
		int result;   
	 
		wchar_t* pBuffer = new wchar_t[len];   
		memset(pBuffer,0,len * sizeof(wchar_t));   
		result = utf82wchar(in.c_str(), in.length(), pBuffer, len*sizeof(wchar_t));   

		if(result >= 0)   
		{   
			out = pBuffer;   
		}   
		else   
		{   
			out.clear();         
		}   

		delete[] pBuffer;   
		return result;   
	}   
	*/

	size_t utf8length(std::string& utf8str)
	{
		try
		{
			return utf8::distance(utf8str.c_str(), 
				utf8str.c_str() + utf8str.size());
		}
		catch (std::exception&)
		{
			utf8str = "";
			return 0;
		}
	}

	void utf8truncate(std::string& utf8str, size_t len)
	{
		try
		{
			size_t wlen = utf8::distance(utf8str.c_str(), 
				utf8str.c_str() + utf8str.size());
			if (wlen <= len)
				return;

			std::wstring wstr;
			wstr.resize(wlen);
			utf8::utf8to16(utf8str.c_str(), utf8str.c_str() + 
				utf8str.size(), &wstr[0]);
			wstr.resize(len);

			char* oend = utf8::utf16to8(wstr.c_str(), 
				wstr.c_str() + wstr.size(), &utf8str[0]);

			utf8str.resize(oend - (&utf8str[0]));
		}
		catch (std::exception&)
		{
			utf8str = "";
		}
	}

	bool utf82wchar(char const* utf8str, size_t csize, 
		wchar_t* wstr, size_t& wsize)
	{
		try
		{
			size_t len = utf8::distance(utf8str, utf8str + csize);

			if (len > wsize)
			{
				if (wsize > 0)
					wstr[0] = L'\0';
				wsize = 0;
				return false;
			}

			wsize = len;
			utf8::utf8to16(utf8str, utf8str + csize, wstr);
			wstr[len] = L'\0';
		}
		catch (std::exception&)
		{
			if (wsize > 0)
				wstr[0] = L'\0';
			wsize = 0;
			return false;
		}

		return true;
	}

	bool utf82wchar(const std::string& utf8str, std::wstring& wstr)
	{
		try
		{
			size_t len = utf8::distance(utf8str.c_str(), 
				utf8str.c_str() + utf8str.size());
			wstr.resize(len);

			if (len)
				utf8::utf8to16(utf8str.c_str(), 
				utf8str.c_str() + utf8str.size(), &wstr[0]);
		}
		catch (std::exception&)
		{
			wstr = L"";
			return false;
		}

		return true;
	}

	bool wchar2utf8(const wchar_t* wstr, size_t size, std::string& utf8str)
	{
		try
		{
			std::string utf8str2;
			utf8str2.resize(size * 4);                          // allocate for most long case

			char* oend = utf8::utf16to8(wstr, wstr + size, &utf8str2[0]);
			utf8str2.resize(oend - (&utf8str2[0]));             // remove unused tail
			utf8str = utf8str2;
		}
		catch (std::exception&)
		{
			utf8str = "";
			return false;
		}

		return true;
	}

	bool wchar2utf8(const std::wstring& wstr, std::string& utf8str)
	{
		try
		{
			std::string utf8str2;
			utf8str2.resize(wstr.size() * 4);                   // allocate for most long case

			char* oend = utf8::utf16to8(wstr.c_str(), 
				wstr.c_str() + wstr.size(), &utf8str2[0]);

			utf8str2.resize(oend - (&utf8str2[0]));             // remove unused tail
			utf8str = utf8str2;
		}
		catch (std::exception&)
		{
			utf8str = "";
			return false;
		}

		return true;
	}
}

}
