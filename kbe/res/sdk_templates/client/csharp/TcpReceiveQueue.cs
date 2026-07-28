namespace KBEngine
{
	using System;
	using System.Threading;

	internal sealed class TcpReceiveQueue
	{
		private readonly byte[] _buffer;
		private readonly object _sync = new object();
		private int _readPosition;
		private int _writePosition;
		private int _count;
		private bool _stopped;

		internal TcpReceiveQueue(int capacity)
		{
			if (capacity <= 0)
				throw new ArgumentOutOfRangeException(nameof(capacity));

			_buffer = new byte[capacity];
		}

		internal int capacity => _buffer.Length;

		internal bool write(byte[] source, int offset, int count)
		{
			if (source == null)
				throw new ArgumentNullException(nameof(source));
			if (offset < 0 || count < 0 || offset > source.Length - count)
				throw new ArgumentOutOfRangeException();
			if (count > _buffer.Length)
				throw new ArgumentOutOfRangeException(nameof(count));

			lock (_sync)
			{
				// 队列满时让 TCP 接收线程休眠并把背压交给内核窗口；主线程消费或关闭时会精确唤醒，不需要轮询和重复日志。
				// When full, sleep the TCP receiver and let the kernel window apply backpressure; consumption or shutdown wakes it without polling or repeated logs.
				while (!_stopped && _buffer.Length - _count < count)
					Monitor.Wait(_sync);

				if (_stopped)
					return false;

				int first = Math.Min(count, _buffer.Length - _writePosition);
				Array.Copy(source, offset, _buffer, _writePosition, first);
				int second = count - first;
				if (second > 0)
					Array.Copy(source, offset + first, _buffer, 0, second);

				_writePosition = (_writePosition + count) % _buffer.Length;
				_count += count;
				return true;
			}
		}

		internal int drain(byte[] destination)
		{
			if (destination == null)
				throw new ArgumentNullException(nameof(destination));

			lock (_sync)
			{
				int count = Math.Min(_count, destination.Length);
				if (count == 0)
					return 0;

				int first = Math.Min(count, _buffer.Length - _readPosition);
				Array.Copy(_buffer, _readPosition, destination, 0, first);
				int second = count - first;
				if (second > 0)
					Array.Copy(_buffer, 0, destination, first, second);

				_readPosition = (_readPosition + count) % _buffer.Length;
				_count -= count;
				Monitor.PulseAll(_sync);
				return count;
			}
		}

		internal void stop()
		{
			lock (_sync)
			{
				_stopped = true;
				_count = 0;
				_readPosition = _writePosition = 0;
				Monitor.PulseAll(_sync);
			}
		}
	}
}
