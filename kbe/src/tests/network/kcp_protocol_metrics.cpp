#include "network/ikcp.h"
#include "network/threshold_hysteresis.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
typedef std::vector<std::vector<char> > Datagrams;

bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

int captureDatagram(const char* buffer, int length, ikcpcb*, void* user)
{
	Datagrams* datagrams = static_cast<Datagrams*>(user);
	datagrams->push_back(std::vector<char>(buffer, buffer + length));
	return 0;
}

bool testAckCounters()
{
	Datagrams outboundA;
	Datagrams outboundB;
	ikcpcb* a = ikcp_create(7, &outboundA);
	ikcpcb* b = ikcp_create(7, &outboundB);
	if (!require(a != NULL && b != NULL, "KCP allocation failed"))
		return false;
	a->output = captureDatagram;
	b->output = captureDatagram;
	ikcp_nodelay(a, 1, 10, 2, 1);
	ikcp_nodelay(b, 1, 10, 2, 1);

	const char payload[] = "metrics";
	ikcp_send(a, payload, static_cast<int>(sizeof(payload)));
	ikcp_update(a, 0);
	bool ok = require(outboundA.size() == 1, "initial KCP payload was not emitted") &&
		require(ikcp_input(b, outboundA[0].data(), static_cast<long>(outboundA[0].size())) == 0,
			"peer rejected the KCP payload");
	ikcp_flushacks(b);
	ok = require(outboundB.size() == 1, "KCP ACK was not emitted") && ok;
	ok = require(b->ackcount == 0, "dedicated ACK flush did not clear pending ACKs") && ok;
	ikcp_flushacks(b);
	ok = require(outboundB.size() == 1, "empty ACK flush emitted a duplicate datagram") && ok;
	if (!outboundB.empty())
	{
		ok = require(ikcp_input(a, outboundB[0].data(), static_cast<long>(outboundB[0].size())) == 0,
			"sender rejected the KCP ACK") && ok;
	}
	ok = require(b->ack_sent == 1, "sent ACK counter is incorrect") &&
		require(a->ack_received == 1, "received ACK counter is incorrect") &&
		require(a->flush_calls == 1, "data flush call counter is incorrect") &&
		require(a->flush_scanned_segments == 1, "flush scan counter is incorrect") &&
		require(a->flush_data_segments == 1, "flush data segment counter is incorrect") &&
		require(a->flush_empty_data_calls == 0, "productive flush was classified as empty") &&
		require(a->data_output_calls == 1 && a->data_output_bytes == outboundA[0].size(),
			"data output counters are incorrect") &&
		require(b->ack_output_calls == 1 && b->ack_output_bytes == outboundB[0].size(),
			"ACK output counters are incorrect") && ok;

	ikcp_release(a);
	ikcp_release(b);
	return ok;
}

bool testRetransmissionCounters()
{
	Datagrams outbound;
	ikcpcb* kcp = ikcp_create(8, &outbound);
	if (!require(kcp != NULL, "KCP allocation failed"))
		return false;
	kcp->output = captureDatagram;
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	kcp->rx_rto = 20;

	const char payload[] = "timeout";
	ikcp_send(kcp, payload, static_cast<int>(sizeof(payload)));
	ikcp_update(kcp, 0);
	ikcp_update(kcp, 20);
	bool ok = require(kcp->timeout_retransmissions == 1, "timeout retransmission counter is incorrect");
	ikcp_release(kcp);

	outbound.clear();
	kcp = ikcp_create(9, &outbound);
	if (!require(kcp != NULL, "KCP allocation failed"))
		return false;
	kcp->output = captureDatagram;
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	ikcp_send(kcp, payload, static_cast<int>(sizeof(payload)));
	ikcp_update(kcp, 0);
	if (!require(!iqueue_is_empty(&kcp->snd_buf), "KCP send buffer is unexpectedly empty"))
	{
		ikcp_release(kcp);
		return false;
	}
	IKCPSEG* segment = iqueue_entry(kcp->snd_buf.next, IKCPSEG, node);
	segment->fastack = 2;
	ikcp_update(kcp, 10);
	ok = require(kcp->fast_retransmissions == 1, "fast retransmission counter is incorrect") && ok;
	ikcp_release(kcp);
	return ok;
}

bool testFlushSegmentBudget()
{
	Datagrams outbound;
	ikcpcb* kcp = ikcp_create(10, &outbound);
	if (!require(kcp != NULL, "KCP allocation failed"))
		return false;
	kcp->output = captureDatagram;
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	bool ok = require(ikcp_setflushlimit(kcp, 2) == 0, "flush limit was rejected");
	const char payload[] = "budget";
	for (int index = 0; index < 5; ++index)
		ok = require(ikcp_send(kcp, payload, static_cast<int>(sizeof(payload))) == 0,
			"KCP payload enqueue failed") && ok;

	ikcp_update(kcp, 0);
	ok = require(kcp->nsnd_que == 3 && kcp->nsnd_buf == 2,
		"first flush did not admit exactly two segments") && ok;
	ok = require(ikcp_check(kcp, 0) == 1,
		"limited flush did not request a prompt continuation") && ok;
	ikcp_update(kcp, 1);
	ok = require(kcp->nsnd_que == 1 && kcp->nsnd_buf == 4,
		"second flush did not admit exactly two segments") && ok;
	ikcp_update(kcp, 2);
	ok = require(kcp->nsnd_que == 0 && kcp->nsnd_buf == 5,
		"final flush did not drain the remaining segment") && ok;

	ikcp_release(kcp);
	return ok;
}

bool testStreamCoalescingAndPayloadAccounting()
{
	Datagrams outbound;
	ikcpcb* sender = ikcp_create(12, &outbound);
	ikcpcb* receiver = ikcp_create(12, NULL);
	if (!require(sender != NULL && receiver != NULL, "KCP stream allocation failed"))
		return false;
	sender->output = captureDatagram;
	sender->stream = 1;
	// Production external KCP disables congestion control; mirror that setup so
	// the first update admits queued bytes instead of remaining at initial cwnd=0.
	// 生产环境外部 KCP 关闭拥塞控制；测试保持相同配置，避免首轮因初始 cwnd=0 而不准入数据。
	ikcp_nodelay(sender, 1, 10, 2, 1);
	ikcp_nodelay(receiver, 1, 10, 2, 1);

	const char first[] = "alpha";
	const char second[] = "-beta";
	const char third[] = "-gamma";
	bool ok = require(ikcp_send(sender, first, 5) == 0, "first stream enqueue failed") &&
		require(ikcp_send(sender, second, 5) == 0, "second stream enqueue failed") &&
		require(ikcp_send(sender, third, 6) == 0, "third stream enqueue failed");
	ok = require(sender->nsnd_que == 1, "small stream writes were not coalesced") &&
		require(sender->snd_queue_bytes == 16, "queued payload bytes are incorrect") &&
		require(sender->stream_coalesces == 2, "stream coalesce count is incorrect") &&
		require(sender->stream_coalesced_bytes == 11, "stream coalesced byte count is incorrect") && ok;

	ikcp_update(sender, 0);
	ok = require(sender->snd_queue_bytes == 0 && sender->snd_buf_bytes == 16,
		"send payload accounting did not follow queue admission") && ok;
	for (Datagrams::const_iterator iter = outbound.begin(); iter != outbound.end(); ++iter)
	{
		ok = require(ikcp_input(receiver, iter->data(), static_cast<long>(iter->size())) == 0,
			"receiver rejected coalesced stream data") && ok;
	}
	char received[16] = {};
	ok = require(ikcp_recv(receiver, received, sizeof(received)) == sizeof(received),
		"receiver did not expose the complete coalesced stream") &&
		require(std::memcmp(received, "alpha-beta-gamma", sizeof(received)) == 0,
			"coalesced stream changed byte ordering") && ok;

	Datagrams acknowledgements;
	receiver->user = &acknowledgements;
	receiver->output = captureDatagram;
	ikcp_flushacks(receiver);
	for (Datagrams::const_iterator iter = acknowledgements.begin(); iter != acknowledgements.end(); ++iter)
	{
		ok = require(ikcp_input(sender, iter->data(), static_cast<long>(iter->size())) == 0,
			"sender rejected stream acknowledgement") && ok;
	}
	ok = require(sender->snd_buf_bytes == 0 && sender->nsnd_buf == 0,
		"acknowledged payload bytes were not released") && ok;

	ikcp_release(sender);
	ikcp_release(receiver);
	return ok;
}

bool testTimeoutRetransmissionBudget()
{
	Datagrams outbound;
	ikcpcb* kcp = ikcp_create(11, &outbound);
	if (!require(kcp != NULL, "KCP allocation failed"))
		return false;
	kcp->output = captureDatagram;
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	kcp->rx_rto = 20;
	bool ok = require(ikcp_setflushlimit(kcp, 2) == 0, "flush limit was rejected");
	const char payload[] = "retransmission-budget";
	for (int index = 0; index < 5; ++index)
		ok = require(ikcp_send(kcp, payload, static_cast<int>(sizeof(payload))) == 0,
			"KCP payload enqueue failed") && ok;

	// Drain initial admissions first, then align all deadlines to exercise only
	// timeout retransmission fairness. / 先完成首次发送，再对齐截止时间，只验证超时重传公平性。
	ikcp_update(kcp, 0);
	ikcp_update(kcp, 1);
	ikcp_update(kcp, 2);
	for (struct IQUEUEHEAD* node = kcp->snd_buf.next; node != &kcp->snd_buf; node = node->next)
	{
		IKCPSEG* segment = iqueue_entry(node, IKCPSEG, node);
		segment->resendts = 20;
	}
	kcp->timeout_retransmissions = 0;
	outbound.clear();

	ikcp_update(kcp, 20);
	ok = require(kcp->timeout_retransmissions == 2,
		"first retransmission flush did not emit exactly two segments") && ok;
	ok = require(ikcp_check(kcp, 20) == 21,
		"limited retransmission flush did not request a one-millisecond continuation") && ok;
	ikcp_update(kcp, 21);
	ok = require(kcp->timeout_retransmissions == 4,
		"second retransmission flush did not emit exactly two segments") && ok;
	ok = require(ikcp_check(kcp, 21) == 22,
		"second limited retransmission flush did not request a continuation") && ok;
	ikcp_update(kcp, 22);
	ok = require(kcp->timeout_retransmissions == 5,
		"final retransmission flush did not emit the remaining segment") && ok;

	ikcp_release(kcp);
	return ok;
}

bool testThresholdHysteresis()
{
	using KBEngine::Network::ThresholdHysteresis;
	return require(!ThresholdHysteresis::next(false, 127, 128, 32),
		"hysteresis activated below the high watermark") &&
		require(ThresholdHysteresis::next(false, 128, 128, 32),
			"hysteresis did not activate at the high watermark") &&
		require(ThresholdHysteresis::next(true, 33, 128, 32),
			"hysteresis did not retain state inside the band") &&
		require(!ThresholdHysteresis::next(true, 32, 128, 32),
			"hysteresis did not clear at the low watermark") &&
		require(!ThresholdHysteresis::next(true, 1000, 0, 32),
			"disabled hysteresis retained active state") &&
		require(!ThresholdHysteresis::next(true, 64, 64, 128),
			"low watermark above high was not bounded");
}
}

int main()
{
	if (!testAckCounters() || !testRetransmissionCounters() || !testFlushSegmentBudget() ||
		!testStreamCoalescingAndPayloadAccounting() || !testTimeoutRetransmissionBudget() ||
		!testThresholdHysteresis())
		return EXIT_FAILURE;

	std::cout << "KCP_PROTOCOL_METRICS_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
