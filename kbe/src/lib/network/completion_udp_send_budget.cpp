#include "completion_udp_send_budget.h"

namespace KBEngine {
namespace Network
{

bool CompletionUdpSendBudget::tryReserve(uint64 destination, size_t bytes, size_t limit)
{
	const size_t current = pendingBytes(destination);
	if (bytes > limit || current > limit - bytes)
		return false;

	pendingBytes_[destination] = current + bytes;
	return true;
}

void CompletionUdpSendBudget::release(uint64 destination, size_t bytes)
{
	auto iter = pendingBytes_.find(destination);
	if (iter == pendingBytes_.end())
		return;

	if (iter->second <= bytes)
		pendingBytes_.erase(iter);
	else
		iter->second -= bytes;
}

void CompletionUdpSendBudget::restore(uint64 destination, size_t bytes)
{
	pendingBytes_[destination] += bytes;
}

void CompletionUdpSendBudget::clear()
{
	pendingBytes_.clear();
}

size_t CompletionUdpSendBudget::pendingBytes(uint64 destination) const
{
	auto iter = pendingBytes_.find(destination);
	return iter != pendingBytes_.end() ? iter->second : 0;
}

}
}
