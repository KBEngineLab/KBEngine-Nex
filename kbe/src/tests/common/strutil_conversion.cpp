#include "common/common.h"
#include "common/strutil.h"

#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <string>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << std::endl;
	}
	return condition;
}
}

int main()
{
	// Long resource paths exercise fortified C libraries, which validate the wchar_t
	// element count independently from the allocated byte count.
	// 长资源路径会触发强化 C 运行库的边界校验，确保宽字符元素数和分配字节数没有混用。
	const std::string input = "/runtime/" + std::string(4096, 'a') + "/scripts/common";
	size_t convertedLength = 0;
	wchar_t* converted = KBEngine::strutil::char2wchar(input.c_str(), &convertedLength);
	if (!require(converted != NULL, "char2wchar returned null") ||
		!require(convertedLength == input.size(), "char2wchar returned an unexpected length") ||
		!require(std::wcslen(converted) == input.size(), "char2wchar did not terminate the result") ||
		!require(converted[0] == L'/' && converted[input.size() - 1] == L'n',
			"char2wchar corrupted the converted path"))
	{
		free(converted);
		return EXIT_FAILURE;
	}

	free(converted);
	std::cout << "COMMON_STRUTIL_CONVERSION_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
