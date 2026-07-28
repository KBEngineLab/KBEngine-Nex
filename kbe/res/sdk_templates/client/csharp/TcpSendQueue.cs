namespace KBEngine
{
	using System;

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

			lock (_gate)
			{
				long queued = _writePosition - _sendPosition;
				if (count > _buffer.Length - queued)
				{
					startWorker = false;
					return false;
				}

				int writeOffset = (int)(_writePosition % _buffer.Length);
				int firstPart = Math.Min(count, _buffer.Length - writeOffset);
				Array.Copy(data, offset, _buffer, writeOffset, firstPart);
				if (firstPart < count)
					Array.Copy(data, offset + firstPart, _buffer, 0, count - firstPart);

				_writePosition += count;
				startWorker = !_workerRunning && count > 0;
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
