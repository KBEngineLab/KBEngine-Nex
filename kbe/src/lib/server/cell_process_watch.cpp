#include "cell_process_watch.h"

#if defined(__linux__)
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace KBEngine::CellRecovery
{
namespace
{
#if KBE_PLATFORM == PLATFORM_WIN32
uint64 processStart(HANDLE handle)
{
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!GetProcessTimes(handle, &created, &exited, &kernel, &user))
		return 0;
	return (static_cast<uint64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
#elif defined(__linux__) && defined(SYS_pidfd_open)
ProcessIdentity readLinuxIdentity(const std::string& path)
{
	struct stat ns{};
	if (::stat((path + "/ns/pid").c_str(), &ns) != 0)
		return {};

	std::ifstream statFile(path + "/stat");
	std::string line;
	if (!std::getline(statFile, line))
		return {};
	const auto commEnd = line.rfind(')');
	if (commEnd == std::string::npos || line.size() <= commEnd + 2)
		return {};

	ProcessIdentity identity;
	std::istringstream pidField(line);
	std::istringstream fields(line.substr(commEnd + 2));
	std::string field;
	// comm 可以包含空格和括号；从最后一个右括号之后数到进程启动时间。
	for (int index = 3; index < 22; ++index)
		if (!(fields >> field))
			return {};
	if (!(pidField >> identity.pid) || !(fields >> identity.started))
		return {};
	identity.pidNamespace = static_cast<uint64>(ns.st_ino);
	return identity;
}
#endif
}

ProcessIdentity ProcessIdentity::current()
{
#if KBE_PLATFORM == PLATFORM_WIN32
	return { GetCurrentProcessId(), processStart(GetCurrentProcess()), 0 };
#elif defined(__linux__) && defined(SYS_pidfd_open)
	auto identity = readLinuxIdentity("/proc/self");
	// /proc 可能来自外层 PID 命名空间，不能用其中的 PID 打开本进程。
	return identity.pid == static_cast<uint32>(getpid()) ? identity : ProcessIdentity{};
#else
	return {};
#endif
}

std::unique_ptr<ProcessWatch> ProcessWatch::open(const ProcessIdentity& identity)
{
	if (!identity.pid || !identity.started)
		return {};

#if KBE_PLATFORM == PLATFORM_WIN32
	if (identity.pidNamespace != 0)
		return {};
	const auto handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, identity.pid);
	if (!handle)
		return {};
	std::unique_ptr<ProcessWatch> watch(new ProcessWatch(handle));
	if (processStart(handle) != identity.started || WaitForSingleObject(handle, 0) != WAIT_TIMEOUT)
		return {};
	return watch;
#elif defined(__linux__) && defined(SYS_pidfd_open)
	const auto local = ProcessIdentity::current();
	if (!local.pidNamespace || identity.pidNamespace != local.pidNamespace)
		return {};
	const auto handle = ::syscall(SYS_pidfd_open, static_cast<pid_t>(identity.pid), 0);
	if (handle < 0)
		return {};
	std::unique_ptr<ProcessWatch> watch(new ProcessWatch(static_cast<int>(handle)));
	const auto actual = readLinuxIdentity("/proc/" + std::to_string(identity.pid));
	struct pollfd state{static_cast<int>(handle), POLLIN, 0};
	// 读 /proc 期间如果旧进程退出，就不能把新 PID 对应的资料与旧 pidfd 混用。
	if (actual.pid != identity.pid || actual.started != identity.started ||
		actual.pidNamespace != identity.pidNamespace || ::poll(&state, 1, 0) != 0)
		return {};
	return watch;
#else
	return {};
#endif
}

ProcessWatch::~ProcessWatch()
{
#if KBE_PLATFORM == PLATFORM_WIN32
	CloseHandle(handle_);
#elif defined(__linux__)
	::close(handle_);
#endif
}

bool ProcessWatch::exited() const
{
#if KBE_PLATFORM == PLATFORM_WIN32
	return WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
#elif defined(__linux__) && defined(SYS_pidfd_open)
	struct pollfd state{handle_, POLLIN, 0};
	return ::poll(&state, 1, 0) == 1 && (state.revents & POLLIN) != 0;
#else
	return false;
#endif
}
}
