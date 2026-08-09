#include "helper/debug_helper.h"

#include <cstdlib>

namespace KBEngine
{
// 单元测试不初始化完整日志网络栈；这些边界实现仅满足 MemoryStream 异常分支的链接契约，成功路径不会调用它们。
// Unit tests do not initialize the full logging network stack; these boundary implementations satisfy MemoryStream exception-path linkage and are not called on successful paths.
KBE_SINGLETON_INIT(DebugHelper);

void DebugHelper::debug_msg(const std::string&)
{
}

void DebugHelper::error_msg(const std::string&)
{
}

void DebugHelper::info_msg(const std::string&)
{
}

void DebugHelper::warning_msg(const std::string&)
{
}

void DebugHelper::critical_msg(const std::string&)
{
	std::abort();
}

// 测试目标不链接完整日志运行时，但断言失败仍必须立即终止，不能静默继续。
// Test targets do not link the full logging runtime, but a failed assertion
// must still terminate immediately rather than allowing corrupted state onward.
void myassert(const char*, const char*, const char*, unsigned int)
{
	std::abort();
}
}
