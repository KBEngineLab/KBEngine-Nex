#include "helper/debug_helper.h"

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
}
