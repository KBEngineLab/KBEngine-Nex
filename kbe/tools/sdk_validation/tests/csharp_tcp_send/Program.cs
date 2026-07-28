using KBEngine;

internal static class Program
{
    private static int Main()
    {
        fullQueueDoesNotLeakLock();
        tcpBatchFailureIsAtomic();
        wraparoundPreservesBytes();
        concurrentProducersPreservePackets();
        abortAllowsWorkerRestart();
        largeMemoryStreamFieldGrowsAndReleasesCapacity();
        kcpBatchFailureIsAtomic();
        kcpAcknowledgementReleasesBytes();

        Console.WriteLine("CSHARP_SEND_BACKPRESSURE_TEST_PASS tcp-full=true tcp-wrap=true concurrent=true restart=true large-field=true kcp-atomic=true kcp-ack-release=true");
        return 0;
    }

    private static void fullQueueDoesNotLeakLock()
    {
        var queue = new TcpSendQueue(4);
        Require(queue.tryEnqueue(new byte[] { 1, 2, 3, 4 }, 0, 4, out bool start) && start,
            "The first enqueue did not claim the worker.");
        Require(!queue.tryEnqueue(new byte[] { 5 }, 0, 1, out start),
            "A full queue accepted another byte.");

        // 满队列失败后再次读取必须立即完成；旧实现会在这里因遗漏 Monitor.Exit 永久阻塞。
        // Reading after a full-queue failure must complete immediately; the old implementation blocked here forever after omitting Monitor.Exit.
        Require(queue.tryGetContiguousData(out _, out _, out int count) && count == 4,
            "The queue lock remained held after a capacity failure.");
        queue.consume(4);
        Require(!queue.tryGetContiguousData(out _, out _, out _), "The drained worker did not stop.");
        Require(queue.tryEnqueue(new byte[] { 5 }, 0, 1, out start) && start,
            "A later enqueue could not restart the worker.");
    }

    private static void wraparoundPreservesBytes()
    {
        var queue = new TcpSendQueue(5);
        queue.tryEnqueue(new byte[] { 1, 2, 3, 4 }, 0, 4, out _);
        queue.tryGetContiguousData(out _, out _, out int firstCount);
        queue.consume(firstCount - 1);
        Require(queue.tryEnqueue(new byte[] { 5, 6, 7 }, 0, 3, out _), "Wraparound enqueue failed.");
        Require(ReadAll(queue).SequenceEqual(new byte[] { 4, 5, 6, 7 }), "Wraparound changed FIFO byte order.");
    }

    private static void concurrentProducersPreservePackets()
    {
        const int producerCount = 32;
        var queue = new TcpSendQueue(producerCount * 2);
        int workerStarts = 0;

        Parallel.For(0, producerCount, value =>
        {
            byte[] packet = { (byte)value, (byte)(255 - value) };
            Require(queue.tryEnqueue(packet, 0, packet.Length, out bool start),
                "Concurrent enqueue exceeded the exact queue capacity.");
            if (start)
                Interlocked.Increment(ref workerStarts);
        });

        byte[] result = ReadAll(queue);
        Require(workerStarts == 1, "Concurrent producers claimed more than one send worker.");
        Require(result.Length == producerCount * 2, "Concurrent producers lost queued bytes.");

        var packetValues = new HashSet<byte>();
        for (int index = 0; index < result.Length; index += 2)
        {
            Require(result[index + 1] == 255 - result[index], "Concurrent producers interleaved packet bytes.");
            packetValues.Add(result[index]);
        }

        Require(packetValues.SetEquals(Enumerable.Range(0, producerCount).Select(value => (byte)value)),
            "Concurrent producers duplicated or omitted a packet.");
    }

    private static void abortAllowsWorkerRestart()
    {
        var queue = new TcpSendQueue(8);
        queue.tryEnqueue(new byte[] { 1, 2 }, 0, 2, out bool start);
        Require(start, "The initial worker was not started.");
        queue.abort();
        Require(queue.tryEnqueue(new byte[] { 3 }, 0, 1, out start) && start,
            "A failed worker left the running state armed.");
        Require(ReadAll(queue).SequenceEqual(new byte[] { 3 }), "Abort retained bytes from the failed transport.");
    }

    private static void largeMemoryStreamFieldGrowsAndReleasesCapacity()
    {
        byte[] payload = Enumerable.Range(0, 60000).Select(value => (byte)((value * 31 + 7) & 0xff)).ToArray();
        var stream = new KBEngine.MemoryStream();
        stream.ensureSpace(checked(payload.Length + sizeof(uint)));
        stream.writeBlob(payload);

        Require(stream.length() == payload.Length + sizeof(uint), "A large BLOB was truncated during serialization.");
        Require(BitConverter.ToUInt32(stream.data(), 0) == payload.Length,
            "A large BLOB changed its protocol length prefix.");
        Require(new ArraySegment<byte>(stream.data(), sizeof(uint), payload.Length).SequenceEqual(payload),
            "A large BLOB changed payload bytes during serialization.");

        int expandedCapacity = stream.data().Length;
        Require(expandedCapacity >= payload.Length + sizeof(uint), "MemoryStream did not grow to the required capacity.");
        stream.clear();

        // 池化对象释放一次性大字段后必须恢复默认容量，否则少量峰值消息会永久抬高客户端常驻内存。
        // A pooled object must return to its default capacity after a one-off large field, or a brief spike permanently raises client memory use.
        Require(stream.length() == 0 && stream.data().Length == KBEngine.MemoryStream.BUFFER_MAX,
            "MemoryStream retained an oversized pooled buffer after clear.");
    }

    private static void tcpBatchFailureIsAtomic()
    {
        var queue = new TcpSendQueue(4);
        Require(queue.tryEnqueue(new byte[] { 1, 2 }, 0, 2, out _), "Initial TCP bytes were rejected.");
        var rejected = new[]
        {
            new ArraySegment<byte>(new byte[] { 3 }),
            new ArraySegment<byte>(new byte[] { 4, 5 })
        };
        Require(!queue.tryEnqueue(rejected, out _), "Oversized TCP batch was accepted.");
        Require(ReadAll(queue).SequenceEqual(new byte[] { 1, 2 }),
            "Rejected TCP batch partially changed queued bytes.");

        var accepted = new[]
        {
            new ArraySegment<byte>(new byte[] { 3 }),
            new ArraySegment<byte>(new byte[] { 4, 5 })
        };
        Require(queue.tryEnqueue(accepted, out _), "Valid TCP batch was rejected after draining.");
        Require(ReadAll(queue).SequenceEqual(new byte[] { 3, 4, 5 }),
            "Accepted TCP batch changed segment order.");
    }

    private static void kcpBatchFailureIsAtomic()
    {
        var kcp = new Deps.KCP(1, null);
        byte[] first = Enumerable.Range(0, 100).Select(value => (byte)value).ToArray();
        Require(kcp.SendBatch(new[] { new ArraySegment<byte>(first) }, 120) == 0,
            "The initial KCP batch was rejected.");
        Require(kcp.PendingSendBytes() == 100 && kcp.WaitSnd() == 1,
            "KCP did not account for the accepted payload.");

        // 整个第二批超过剩余字节上限时必须零修改，不能留下可发送的前半批 segment。
        // When the complete second batch exceeds remaining bytes, it must make zero changes and cannot leave sendable segments from its first half.
        var rejected = new[]
        {
            new ArraySegment<byte>(new byte[10]),
            new ArraySegment<byte>(new byte[11])
        };
        Require(kcp.SendBatch(rejected, 120) == -3, "KCP did not report byte backpressure.");
        Require(kcp.PendingSendBytes() == 100 && kcp.WaitSnd() == 1,
            "A rejected KCP batch partially changed the send queue.");

        kcp.Release();
        Require(kcp.PendingSendBytes() == 0 && kcp.WaitSnd() == 0,
            "KCP release retained pending send accounting.");
    }

    private static void kcpAcknowledgementReleasesBytes()
    {
        var sender = new Deps.KCP(7, null);
        var receiver = new Deps.KCP(7, null);
        sender.NoDelay(1, 10, 2, 1);
        receiver.NoDelay(1, 10, 2, 1);
        sender.SetOutput((data, size, _) => Require(receiver.Input(data, 0, size) == 0,
            "The receiving KCP endpoint rejected a data datagram."));
        receiver.SetOutput((data, size, _) => Require(sender.Input(data, 0, size) == 0,
            "The sending KCP endpoint rejected an acknowledgement datagram."));

        byte[] payload = Enumerable.Range(0, 100).Select(value => (byte)value).ToArray();
        Require(sender.SendBatch(new[] { new ArraySegment<byte>(payload) }, 100) == 0,
            "KCP sender rejected a batch at its exact byte limit.");
        sender.Update(1);
        receiver.Update(1);

        var received = new byte[payload.Length];
        Require(receiver.Recv(received, 0, received.Length) == payload.Length && received.SequenceEqual(payload),
            "KCP loopback changed the acknowledged payload.");
        Require(sender.PendingSendBytes() == 0 && sender.WaitSnd() == 0,
            "KCP acknowledgement did not release pending payload bytes.");
    }

    private static byte[] ReadAll(TcpSendQueue queue)
    {
        var result = new List<byte>();
        while (queue.tryGetContiguousData(out byte[] buffer, out int offset, out int count))
        {
            result.AddRange(new ArraySegment<byte>(buffer, offset, count));
            queue.consume(count);
        }

        return result.ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }
}
