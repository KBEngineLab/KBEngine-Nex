#ifndef KBE_SERVER_CELL_PROCESS_WATCH_H
#define KBE_SERVER_CELL_PROCESS_WATCH_H

#include "common/platform.h"
#include <memory>

namespace KBEngine::CellRecovery
{
struct ProcessIdentity
{
	uint32 pid = 0;
	uint64 started = 0;
	uint64 pidNamespace = 0;

	static ProcessIdentity current();
};

class ProcessWatch
{
public:
	~ProcessWatch();
	ProcessWatch(const ProcessWatch&) = delete;
	ProcessWatch& operator=(const ProcessWatch&) = delete;

	// 必须在已绑定的本机 Cell 连接仍有效时调用，断线后不能再用 PID 查找进程。
	static std::unique_ptr<ProcessWatch> open(const ProcessIdentity& identity);
	bool exited() const;

private:
#if KBE_PLATFORM == PLATFORM_WIN32
	using Handle = HANDLE;
#else
	using Handle = int;
#endif
	explicit ProcessWatch(Handle handle) : handle_(handle) {}
	Handle handle_;
};
}

#endif
