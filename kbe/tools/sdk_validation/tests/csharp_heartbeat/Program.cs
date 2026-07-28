using System.Diagnostics;
using KBEngine;

internal static class Program
{
    private static int Main()
    {
        const int intervalSeconds = 15;
        long origin = Stopwatch.Frequency * 100L;
        long interval = Stopwatch.Frequency * intervalSeconds;
        var state = new HeartbeatState();
        state.reset(origin);

        Require(!state.isDue(origin + interval, intervalSeconds), "The exact interval boundary must not fire early.");
        Require(state.isDue(origin + interval + 1, intervalSeconds), "Elapsed total time did not make the heartbeat due.");

        state.markAttempt(origin + interval + 1, true);
        Require(state.replyPending, "A sent heartbeat did not enter the pending state.");
        state.markReply();
        Require(!state.replyPending, "A heartbeat reply did not clear the pending state.");

        state.markAttempt(origin + interval * 2, true);
        Require(state.replyPending && state.isDue(origin + interval * 3 + 1, intervalSeconds),
            "An unanswered heartbeat did not remain pending through the next interval.");

        // 协议尚未导入、没有实际发送心跳时不能制造 pending，否则客户端会把初始化阶段误报为网络超时。
        // An unavailable protocol message must not create pending without an actual send, or initialization can be misreported as a network timeout.
        state.markAttempt(origin + interval * 4, false);
        Require(!state.replyPending, "An unsent heartbeat incorrectly entered the pending state.");

        state.markAttempt(origin + interval * 5, true);
        state.reset(origin + interval * 5 + 1);
        Require(!state.replyPending, "A connection reset inherited the old link's pending heartbeat.");

        Console.WriteLine("CSHARP_HEARTBEAT_STATE_TEST_PASS boundary=true reply=true timeout=true unsent=true reset=true");
        return 0;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }
}
