namespace KBEngine
{
	using System.Diagnostics;

	// 心跳状态只表达时间与请求生命周期，不持有网络接口；调用方仍负责发送、关闭和公开断线事件。
	// Heartbeat state models only timing and request lifecycle without owning the network interface; the caller remains responsible for sends, closes, and public disconnect events.
	internal sealed class HeartbeatState
	{
		private long _lastAttemptTimestamp;

		public bool replyPending { get; private set; }

		public HeartbeatState()
		{
			reset(timestamp());
		}

		public static long timestamp()
		{
			return Stopwatch.GetTimestamp();
		}

		public bool isDue(long now, int intervalSeconds)
		{
			return now - _lastAttemptTimestamp > (long)intervalSeconds * Stopwatch.Frequency;
		}

		public void markAttempt(long now, bool sent)
		{
			_lastAttemptTimestamp = now;
			replyPending = sent;
		}

		public void markReply()
		{
			replyPending = false;
		}

		public void reset(long now)
		{
			_lastAttemptTimestamp = now;
			replyPending = false;
		}
	}
}
