#include "network/ikcp.h"

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
	ikcp_update(b, 0);
	ok = require(outboundB.size() == 1, "KCP ACK was not emitted") && ok;
	if (!outboundB.empty())
	{
		ok = require(ikcp_input(a, outboundB[0].data(), static_cast<long>(outboundB[0].size())) == 0,
			"sender rejected the KCP ACK") && ok;
	}
	ok = require(b->ack_sent == 1, "sent ACK counter is incorrect") &&
		require(a->ack_received == 1, "received ACK counter is incorrect") && ok;

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
}

int main()
{
	if (!testAckCounters() || !testRetransmissionCounters() || !testFlushSegmentBudget())
		return EXIT_FAILURE;

	std::cout << "KCP_PROTOCOL_METRICS_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
