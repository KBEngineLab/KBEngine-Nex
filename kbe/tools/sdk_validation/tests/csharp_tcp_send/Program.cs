using KBEngine;

internal static class Program
{
    private static int Main()
    {
        fullQueueDoesNotLeakLock();
        wraparoundPreservesBytes();
        concurrentProducersPreservePackets();
        abortAllowsWorkerRestart();

        Console.WriteLine("CSHARP_TCP_SEND_QUEUE_TEST_PASS full=true wrap=true concurrent=true restart=true");
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
