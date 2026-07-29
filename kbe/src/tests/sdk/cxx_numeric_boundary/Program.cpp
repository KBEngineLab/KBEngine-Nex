#include "KBECommon.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{

void require(bool condition, const char* message)
{
	if (!condition)
	{
		throw std::runtime_error(message);
	}
}

template<typename Exception, typename Callback>
void requireThrows(Callback callback, const char* message)
{
	try
	{
		callback();
	}
	catch (const Exception&)
	{
		return;
	}

	throw std::runtime_error(message);
}

void verifyUint16Boundaries()
{
	require(KBEngine::checkedUint16(0, "test uint16") == 0, "uint16 lower boundary changed.");
	require(KBEngine::checkedUint16(65535, "test uint16") == std::numeric_limits<uint16>::max(),
		"uint16 upper boundary changed.");
	requireThrows<std::out_of_range>([]() { KBEngine::checkedUint16(-1, "test uint16"); },
		"A negative uint16 input was accepted.");
	requireThrows<std::out_of_range>([]() { KBEngine::checkedUint16(65536, "test uint16"); },
		"An overflowing uint16 input was accepted.");
}

void verifyUint32Boundaries()
{
	const std::size_t maximum = static_cast<std::size_t>(std::numeric_limits<uint32>::max());
	require(KBEngine::checkedUint32(0, "test uint32") == 0, "uint32 lower boundary changed.");
	require(KBEngine::checkedUint32(maximum, "test uint32") == std::numeric_limits<uint32>::max(),
		"uint32 upper boundary changed.");
	requireThrows<std::length_error>([maximum]() { KBEngine::checkedUint32(maximum + 1, "test uint32"); },
		"An overflowing uint32 length was accepted.");
}

}

int main()
{
	try
	{
		verifyUint16Boundaries();
		verifyUint32Boundaries();
		std::cout << "CXX_NUMERIC_BOUNDARY_TEST_PASS uint16=true uint32=true overflow-rejected=true\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "CXX_NUMERIC_BOUNDARY_TEST_FAIL error=" << exception.what() << '\n';
		return 1;
	}
}
