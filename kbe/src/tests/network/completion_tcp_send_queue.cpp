#include "network/completion_tcp_send_queue.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << std::endl;
	}

	return condition;
}

std::string bufferText(const KBEngine::Network::CompletionTcpSendBuffer& buffer)
{
	return buffer.empty() ? std::string() : std::string(buffer.data(), buffer.size());
}

bool testSingleBufferOwnershipTransfer()
{
	KBEngine::Network::CompletionTcpSendQueue queue;
	const std::string payload("single-packet");
	if (!require(queue.push(payload.data(), payload.size(), 1024), "single buffer enqueue failed"))
	{
		return false;
	}

	const char* queuedStorage = queue.frontData();
	KBEngine::Network::CompletionTcpSendBuffer batch;
	bool copied = true;
	if (!require(queue.popBatch(1024, batch, copied), "single buffer pop failed") ||
		!require(!copied, "single buffer was copied instead of transferred") ||
		!require(batch.data() == queuedStorage, "single buffer storage ownership did not transfer") ||
		!require(bufferText(batch) == payload, "single buffer payload changed") ||
		!require(queue.empty() && queue.pendingBytes() == 0, "single buffer queue accounting did not drain"))
	{
		return false;
	}

	return true;
}

bool testCoalescingAndBoundaries()
{
	KBEngine::Network::CompletionTcpSendQueue queue;
	queue.push("abc", 3, 1024);
	queue.push("defg", 4, 1024);
	queue.push("hij", 3, 1024);

	KBEngine::Network::CompletionTcpSendBuffer batch;
	bool copied = false;
	if (!require(queue.popBatch(8, batch, copied), "coalesced batch pop failed") ||
		!require(copied, "multi-buffer batch did not report its copy") ||
		!require(bufferText(batch) == "abcdefgh", "coalesced batch order changed") ||
		!require(queue.pendingBytes() == 2, "coalesced batch accounting is incorrect") ||
		!require(std::string(queue.frontData(), queue.frontSize()) == "ij", "batch boundary tail changed"))
	{
		return false;
	}

	KBEngine::Network::CompletionTcpSendBuffer tail;
	copied = true;
	return require(queue.popBatch(8, tail, copied), "tail pop failed") &&
		require(!copied, "single tail should transfer without copying") &&
		require(bufferText(tail) == "ij", "tail payload changed") &&
		require(queue.empty() && queue.pendingBytes() == 0, "tail accounting did not drain");
}

bool testPartialCompletionRestore()
{
	KBEngine::Network::CompletionTcpSendQueue queue;
	const std::string payload("0123456789");
	queue.push(payload.data(), payload.size(), 1024);

	KBEngine::Network::CompletionTcpSendBuffer batch;
	bool copied = false;
	if (!require(queue.popBatch(64, batch, copied), "partial fixture pop failed"))
	{
		return false;
	}

	const char* originalStorage = batch.data();
	queue.push("later", 5, 1024);
	if (!require(queue.restore(batch, 4), "partial completion restore failed") ||
		!require(queue.pendingBytes() == 11, "restored pending byte count is incorrect") ||
		!require(queue.frontData() == originalStorage + 4, "partial restore copied or shifted storage") ||
		!require(std::string(queue.frontData(), queue.frontSize()) == "456789", "partial restore tail changed"))
	{
		return false;
	}

	const char* restoredStorage = queue.frontData();
	if (!require(queue.consumeFront(2), "front offset consumption failed") ||
		!require(queue.frontData() == restoredStorage + 2, "front consumption moved storage") ||
		!require(queue.pendingBytes() == 9, "front consumption accounting is incorrect"))
	{
		return false;
	}

	KBEngine::Network::CompletionTcpSendBuffer combined;
	copied = false;
	return require(queue.popBatch(64, combined, copied), "restored queue pop failed") &&
		require(copied, "restored multi-buffer batch should coalesce") &&
		require(bufferText(combined) == "6789later", "restored queue order changed") &&
		require(queue.empty() && queue.pendingBytes() == 0, "restored queue accounting did not drain");
}

bool testBacklogLimit()
{
	KBEngine::Network::CompletionTcpSendQueue queue;
	return require(queue.pushResult("1234", 4, 4) == KBEngine::Network::CompletionTcpSendQueue::PUSH_ACCEPTED,
			"exact backlog limit was rejected") &&
		require(queue.pushResult("5", 1, 4) == KBEngine::Network::CompletionTcpSendQueue::PUSH_BACKPRESSURED,
			"backlog overflow was not classified as transient backpressure") &&
		require(queue.pushResult("12345", 5, 4) == KBEngine::Network::CompletionTcpSendQueue::PUSH_OVERSIZED,
			"oversized chunk was not classified as permanently unqueueable") &&
		require(queue.pushResult(NULL, 1, 4) == KBEngine::Network::CompletionTcpSendQueue::PUSH_INVALID,
			"null payload was not classified as invalid") &&
		require(queue.pendingBytes() == 4 && queue.frontSize() == 4,
			"rejected backlog push changed queue state");
}

bool testDrainRetryOrdering()
{
	KBEngine::Network::CompletionTcpSendQueue queue;
	if (!require(queue.push("first", 5, 8), "drain fixture first push failed") ||
		!require(queue.pushResult("next", 4, 8) == KBEngine::Network::CompletionTcpSendQueue::PUSH_BACKPRESSURED,
			"drain fixture did not enter backpressure"))
	{
		return false;
	}

	KBEngine::Network::CompletionTcpSendBuffer first;
	bool copied = false;
	if (!require(queue.popBatch(8, first, copied), "drain fixture pop failed") ||
		!require(queue.push("next", 4, 8), "retry after drain failed") ||
		!require(queue.restore(first, 2), "partial first batch restore failed"))
	{
		return false;
	}

	KBEngine::Network::CompletionTcpSendBuffer ordered;
	return require(queue.popBatch(8, ordered, copied), "drain fixture ordered pop failed") &&
		require(bufferText(ordered) == "rstnext", "retry crossed the partially sent stream prefix") &&
		require(queue.empty() && queue.pendingBytes() == 0, "drain fixture did not empty");
}
}

int main()
{
	if (!testSingleBufferOwnershipTransfer() ||
		!testCoalescingAndBoundaries() ||
		!testPartialCompletionRestore() ||
		!testBacklogLimit() ||
		!testDrainRetryOrdering())
	{
		return EXIT_FAILURE;
	}

	std::cout << "COMPLETION_TCP_SEND_QUEUE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
