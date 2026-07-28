#include "TcpReceiveQueue.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

void require(bool condition, const char* message)
{
	if (!condition)
	{
		throw std::runtime_error(message);
	}
}

void verifyWaitWakeAndFifo()
{
	KBEngine::TcpReceiveQueue queue(4);
	const std::array<uint8, 3> first = { 1, 2, 3 };
	require(queue.write(first.data(), first.size()), "The queue rejected its initial bytes.");

	const std::array<uint8, 2> second = { 5, 6 };
	std::future<bool> writer = std::async(std::launch::async, [&queue, &second]()
	{
		return queue.write(second.data(), second.size());
	});
	require(writer.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
		"A full receive queue did not block its producer.");

	std::vector<uint8> drained;
	require(queue.drain(drained) == first.size() &&
		drained == std::vector<uint8>(first.begin(), first.end()),
		"The first drain changed FIFO bytes.");
	require(writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready && writer.get(),
		"Draining capacity did not wake the receive producer.");
	require(queue.drain(drained) == second.size() &&
		drained == std::vector<uint8>(second.begin(), second.end()),
		"A receive queue wraparound changed FIFO bytes.");
}

void verifyStopAndRestart()
{
	KBEngine::TcpReceiveQueue queue(1);
	const uint8 first = 7;
	require(queue.write(&first, 1), "The stop test could not fill its queue.");

	const uint8 second = 8;
	std::future<bool> writer = std::async(std::launch::async, [&queue, &second]()
	{
		return queue.write(&second, 1);
	});
	require(writer.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
		"The stop test producer was not waiting.");
	queue.stop();
	require(writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready && !writer.get(),
		"Stopping the queue did not cancel its waiting producer.");

	queue.reset(2);
	const std::array<uint8, 2> restarted = { 9, 10 };
	std::vector<uint8> drained;
	require(queue.write(restarted.data(), restarted.size()), "The queue did not accept data after reset.");
	require(queue.drain(drained) == restarted.size() &&
		drained == std::vector<uint8>(restarted.begin(), restarted.end()),
		"Reset did not restore FIFO queue state.");
}

void verifyLargeStreamWithDefaultCapacity()
{
	constexpr std::size_t capacity = 1460;
	constexpr std::size_t payloadSize = 60000;
	std::vector<uint8> expected(payloadSize);
	for (std::size_t index = 0; index < expected.size(); ++index)
	{
		expected[index] = static_cast<uint8>(index % 251);
	}

	KBEngine::TcpReceiveQueue queue(capacity);
	std::future<bool> producer = std::async(std::launch::async, [&queue, &expected]()
	{
		std::size_t offset = 0;
		while (offset < expected.size())
		{
			const std::size_t count = std::min<std::size_t>(997, expected.size() - offset);
			if (!queue.write(expected.data() + offset, count))
			{
				return false;
			}
			offset += count;
		}
		return true;
	});

	// 大协议消息由 MessageReader 跨多次 drain 组装；队列只限制待处理字节，不要求整条消息同时驻留。
	// MessageReader assembles a large protocol message across drains; the queue bounds pending bytes without requiring the whole message to reside at once.
	std::vector<uint8> received;
	received.reserve(payloadSize);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (received.size() < payloadSize && std::chrono::steady_clock::now() < deadline)
	{
		std::vector<uint8> drained;
		if (queue.drain(drained) > 0)
		{
			received.insert(received.end(), drained.begin(), drained.end());
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (received.size() != payloadSize)
	{
		queue.stop();
	}
	require(producer.wait_for(std::chrono::seconds(1)) == std::future_status::ready && producer.get(),
		"The large stream producer did not finish.");
	require(received == expected, "The bounded queue changed the 60 KiB stream.");

	const std::uint64_t checksum = std::accumulate(received.begin(), received.end(), std::uint64_t{ 0 });
	const std::uint64_t expectedChecksum = std::accumulate(expected.begin(), expected.end(), std::uint64_t{ 0 });
	require(checksum == expectedChecksum, "The 60 KiB stream checksum changed.");
}

}

int main()
{
	try
	{
		verifyWaitWakeAndFifo();
		verifyStopAndRestart();
		verifyLargeStreamWithDefaultCapacity();
		std::cout << "CXX_TCP_RECEIVE_BACKPRESSURE_TEST_PASS wait=true wake=true fifo=true stop=true restart=true large-stream=true capacity=1460 bytes=60000\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "CXX_TCP_RECEIVE_BACKPRESSURE_TEST_FAIL error=" << exception.what() << '\n';
		return 1;
	}
}
