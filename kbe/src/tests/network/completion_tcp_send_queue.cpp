#include "network/completion_tcp_send_queue.h"
#include "network/completion_send_backpressure.h"
#include "network/channel_inactivity_policy.h"

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

bool testProducerBackpressureHysteresis()
{
	using KBEngine::Network::CompletionSendBackpressure;
	return require(!CompletionSendBackpressure::next(false, 2047, false, 2048, 512),
			"producer backpressure activated below the high watermark") &&
		require(CompletionSendBackpressure::next(false, 2048, false, 2048, 512),
			"producer backpressure did not activate at the high watermark") &&
		require(CompletionSendBackpressure::next(true, 513, false, 2048, 512),
			"producer backpressure resumed above the low watermark") &&
		require(!CompletionSendBackpressure::next(true, 512, false, 2048, 512),
			"producer backpressure did not resume at the low watermark") &&
		require(CompletionSendBackpressure::next(false, 0, true, 2048, 512),
			"buffered Channel bundles did not force producer backpressure") &&
		require(!CompletionSendBackpressure::next(true, 4096, false, 0, 0),
			"zero high watermark did not disable producer backpressure") &&
		require(!CompletionSendBackpressure::next(true, 4096, true, 0, 0),
			"buffered bundles bypassed the disabled producer backpressure setting");
}

bool testStandaloneBundleCoalescingPolicy()
{
	using KBEngine::Network::CompletionSendBackpressure;
	return require(CompletionSendBackpressure::shouldCoalesceStandaloneBundle(
			false, true, true, true, true),
			"backlogged standalone internal completion Bundle was not coalesced") &&
		require(!CompletionSendBackpressure::shouldCoalesceStandaloneBundle(
			true, true, true, true, true),
			"Channel-owned Bundle was coalesced twice") &&
		require(!CompletionSendBackpressure::shouldCoalesceStandaloneBundle(
			false, false, true, true, true),
			"external Channel entered internal Bundle compaction") &&
		require(!CompletionSendBackpressure::shouldCoalesceStandaloneBundle(
			false, true, false, true, true),
			"readiness backend entered completion Bundle compaction") &&
		require(!CompletionSendBackpressure::shouldCoalesceStandaloneBundle(
			false, true, true, false, true),
			"idle Channel entered backlog compaction") &&
		require(!CompletionSendBackpressure::shouldCoalesceStandaloneBundle(
			false, true, true, true, false),
			"empty Channel entered backlog compaction");
}

bool testInternalCompletionInactivityPolicy()
{
	using KBEngine::Network::ChannelInactivityPolicy;
	return require(ChannelInactivityPolicy::effectiveLastActivity(100, 150, true) == 150,
			"internal completion liveness ignored positive send progress") &&
		require(ChannelInactivityPolicy::effectiveLastActivity(100, 150, false) == 100,
			"external/readiness liveness unexpectedly used send progress") &&
		require(!ChannelInactivityPolicy::expired(209, 150, 60),
			"channel expired before the inactivity period") &&
		require(ChannelInactivityPolicy::expired(210, 150, 60),
			"channel did not expire at the inactivity boundary") &&
		require(!ChannelInactivityPolicy::expired(149, 150, 60),
			"timestamp ordering underflow expired the channel");
}
}

int main()
{
	if (!testSingleBufferOwnershipTransfer() ||
		!testCoalescingAndBoundaries() ||
		!testPartialCompletionRestore() ||
		!testBacklogLimit() ||
		!testDrainRetryOrdering() ||
		!testProducerBackpressureHysteresis() ||
		!testStandaloneBundleCoalescingPolicy() ||
		!testInternalCompletionInactivityPolicy())
	{
		return EXIT_FAILURE;
	}

	std::cout << "COMPLETION_TCP_SEND_QUEUE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
