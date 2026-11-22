#include "common/memorystream.h"
#include "helper/debug_helper.h"

namespace KBEngine
{
// 日志输出由测试替身处理，这里只建立日志对象，不创建 Logger 连接或同步任务。
DebugHelper::DebugHelper() :
	_currLine(0),
	hasBufferedLogPackets_(0),
	pNetworkInterface_(nullptr),
	pDispatcher_(nullptr),
	scriptMsgType_(0),
	noSyncLog_(true),
	canLogFile_(false),
	loseLoggerTime_(0),
	mainThreadID_(),
	memoryStreamPool_("TestDebugHelperMemoryStream")
{
}

DebugHelper::~DebugHelper() = default;
}
