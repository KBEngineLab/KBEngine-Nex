#include "pyscript/vector_repr.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{
bool require(bool condition, const char* message)
{
	if (condition)
		return true;

	std::cerr << message << std::endl;
	return false;
}

bool contains(const std::string& value, const char* expected)
{
	return value.find(expected) != std::string::npos;
}
}

int main()
{
	const float maximum = std::numeric_limits<float>::max();
	const float infinity = std::numeric_limits<float>::infinity();
	const float quietNan = std::numeric_limits<float>::quiet_NaN();

	// 极值、无穷和 NaN 覆盖了会放大文本长度或产生特殊标记的所有浮点类别。
	// Extremes, infinity, and NaN cover every floating-point category that expands text or emits special markers.
	const std::string vector2Repr =
		KBEngine::script::detail::formatVector2Repr(Vector2(maximum, -maximum));
	if (!require(vector2Repr.size() > 80, "Vector2 extreme repr was truncated") ||
		!require(vector2Repr.rfind("Vector2(", 0) == 0, "Vector2 repr prefix changed") ||
		!require(!vector2Repr.empty() && vector2Repr.back() == ')', "Vector2 repr suffix changed"))
	{
		return EXIT_FAILURE;
	}

	const std::string vector3Repr = KBEngine::script::detail::formatVector3Repr(
		Vector3(maximum, -maximum, infinity), false);
	if (!require(vector3Repr.size() > 100, "Vector3 extreme repr was truncated") ||
		!require(contains(vector3Repr, ", inf), ref=false"),
			"Vector3 repr lost non-finite or reference state"))
	{
		return EXIT_FAILURE;
	}

	const std::string referencedVector3Repr = KBEngine::script::detail::formatVector3Repr(
		Vector3(maximum, infinity, quietNan), true);
	if (!require(contains(referencedVector3Repr, ", inf, nan), ref=true"),
		"Vector3 referenced repr lost non-finite or reference state"))
	{
		return EXIT_FAILURE;
	}

	const std::string vector4Repr = KBEngine::script::detail::formatVector4Repr(
		Vector4(maximum, -maximum, infinity, quietNan));
	if (!require(vector4Repr.size() > 100, "Vector4 extreme repr was truncated") ||
		!require(contains(vector4Repr, ", inf, nan)"),
			"Vector4 repr lost non-finite values"))
	{
		return EXIT_FAILURE;
	}

	std::cout << "SCRIPT_VECTOR_REPR_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
