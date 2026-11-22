#include "helper/debug_helper.h"

#include <cstdlib>

namespace KBEngine
{
// 单元测试使用空日志方法，避免连接 Logger。验证异常路径的测试需要先创建 DebugHelper 实例。
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
