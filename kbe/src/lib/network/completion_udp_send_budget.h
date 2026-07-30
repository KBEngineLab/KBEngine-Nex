#ifndef KBE_COMPLETION_UDP_SEND_BUDGET_H
#define KBE_COMPLETION_UDP_SEND_BUDGET_H

#include "common/common.h"

#include <map>

namespace KBEngine {
namespace Network
{

class CompletionUdpSendBudget
{
public:
	bool tryReserve(uint64 destination, size_t bytes, size_t limit);
	void release(uint64 destination, size_t bytes);
	void restore(uint64 destination, size_t bytes);
	void clear();
	size_t pendingBytes(uint64 destination) const;

private:
	std::map<uint64, size_t> pendingBytes_;
};

}
}

#endif // KBE_COMPLETION_UDP_SEND_BUDGET_H
