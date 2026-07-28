namespace KBEngine
{
	using System;
	using System.Collections.Generic;

	// TCP 发送队列将容量判断、环形缓冲区游标和工作线程所有权集中管理，避免网络发送路径散落手工锁操作。
	// The TCP send queue centralizes capacity checks, ring-buffer cursors, and worker ownership so the network path does not scatter manual lock operations.
	internal sealed class TcpSendQueue
	{
		private readonly object _gate = new object();
		private readonly byte[] _buffer;
		private long _writePosition;
		private long _sendPosition;
		private bool _workerRunning;

		internal TcpSendQueue(int capacity)
		{
			if (capacity <= 0)
				throw new ArgumentOutOfRangeException(nameof(capacity));

			_buffer = new byte[capacity];
		}

		internal int capacity => _buffer.Length;

		internal bool tryEnqueue(byte[] data, int offset, int count, out bool startWorker)
		{
			if (data == null)
				throw new ArgumentNullException(nameof(data));

			if (offset < 0 || count < 0 || offset > data.Length - count)
				throw new ArgumentOutOfRangeException(nameof(offset));

			return tryEnqueue(new ArraySegment<byte>[] { new ArraySegment<byte>(data, offset, count) }, out startWorker);
		}

		internal bool tryEnqueue(IReadOnlyList<ArraySegment<byte>> segments, out bool startWorker)
		{
			if (segments == null)
				throw new ArgumentNullException(nameof(segments));

			lock (_gate)
			{
				long totalCount = 0;
				for (int index = 0; index < segments.Count; ++index)
				{
					ArraySegment<byte> segment = segments[index];
					if (segment.Array == null)
						throw new ArgumentException("A send segment has no backing array.", nameof(segments));

					totalCount += segment.Count;
				}

				long queued = _writePosition - _sendPosition;
				if (totalCount > _buffer.Length - queued)
				{
					startWorker = false;
					return false;
				}

				// 所有片段在同一个容量检查和锁周期内复制，失败时队列保持不变，成功时其他生产者也看不到半个 Bundle。
				// Every segment is copied under one capacity check and lock cycle, leaving the queue unchanged on failure and hiding partial Bundles from other producers.
				for (int index = 0; index < segments.Count; ++index)
				{
					ArraySegment<byte> segment = segments[index];
					int writeOffset = (int)(_writePosition % _buffer.Length);
					int firstPart = Math.Min(segment.Count, _buffer.Length - writeOffset);
					Array.Copy(segment.Array, segment.Offset, _buffer, writeOffset, firstPart);
					if (firstPart < segment.Count)
						Array.Copy(segment.Array, segment.Offset + firstPart, _buffer, 0, segment.Count - firstPart);

					_writePosition += segment.Count;
				}

				startWorker = !_workerRunning && totalCount > 0;
				if (startWorker)
					_workerRunning = true;

				return true;
			}
		}

		internal bool tryGetContiguousData(out byte[] buffer, out int offset, out int count)
		{
			lock (_gate)
			{
				long queued = _writePosition - _sendPosition;
				if (queued == 0)
				{
					_workerRunning = false;
					buffer = null;
					offset = 0;
					count = 0;
					return false;
				}

				offset = (int)(_sendPosition % _buffer.Length);
				count = (int)Math.Min(queued, _buffer.Length - offset);
				buffer = _buffer;
				return true;
			}
		}

		internal void consume(int count)
		{
			lock (_gate)
			{
				long queued = _writePosition - _sendPosition;
				if (count <= 0 || count > queued)
					throw new ArgumentOutOfRangeException(nameof(count));

				_sendPosition += count;
			}
		}

		internal void abort()
		{
			lock (_gate)
			{
				// 传输失败后旧连接的数据不得由后续工作线程重发；关闭事件会负责推动网络生命周期。
				// Data from a failed transport must not be retried by a later worker; the close event advances the network lifecycle.
				_sendPosition = _writePosition;
				_workerRunning = false;
			}
		}
	}
}
