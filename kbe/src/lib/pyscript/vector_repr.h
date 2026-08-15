#ifndef KBE_PYSCRIPT_VECTOR_REPR_H
#define KBE_PYSCRIPT_VECTOR_REPR_H

#include "math/math.h"

#include <fmt/format.h>

#include <string>

namespace KBEngine { namespace script { namespace detail {

// 向量 repr 属于日志可观测性边界，必须动态扩容以容纳极值坐标，同时保持历史六位小数格式。
// Vector repr is an observability boundary and must grow dynamically for extreme coordinates while preserving the historical six-decimal format.
inline std::string formatVector2Repr(const Vector2& value)
{
	return fmt::format("Vector2({:.6f}, {:.6f})", value.x, value.y);
}

inline std::string formatVector3Repr(const Vector3& value, bool isReference)
{
	return fmt::format("Vector3({:.6f}, {:.6f}, {:.6f}), ref={}",
		value.x, value.y, value.z, isReference ? "true" : "false");
}

inline std::string formatVector4Repr(const Vector4& value)
{
	return fmt::format("Vector4({:.6f}, {:.6f}, {:.6f}, {:.6f})",
		value.x, value.y, value.z, value.w);
}

}}}

#endif
