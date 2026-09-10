namespace KBEngine
{
	using System;
	using System.Collections.Generic;
	using System.Net;
	using System.Net.Sockets;
	using System.Text;
	using System.Threading.Tasks;

	public sealed class KcpNetworkProvider : INetworkProvider
	{
		private const string UDP_HELLO = "62a559f3fa7748bc22f8e0766019d498";
		private const string UDP_HELLO_ACK = "1432ad7c829170a76dd31982c3501eca";
		private const int HANDSHAKE_TIMEOUT_MILLISECONDS = 30000;
		private const int HELLO_RETRY_MILLISECONDS = 1000;
		private const int KCP_MTU = 1400;
		private const int KCP_MAX_MESSAGE_SIZE =
			(KCP_MTU - Deps.KCP.IKCP_OVERHEAD) * Byte.MaxValue;

		private readonly object _lifecycleLock = new object();
		private readonly object _kcpLock = new object();
		private readonly object _eventLock = new object();
		private readonly Queue<ProviderEvent> _events = new Queue<ProviderEvent>();
		private readonly List<ArraySegment<byte>> _sendSegments = new List<ArraySegment<byte>>();
		private readonly NetworkProviderContext _context;
		private readonly byte[] _datagramBuffer = new byte[UInt16.MaxValue + Deps.KCP.IKCP_OVERHEAD * 2];
		private byte[] _processBuffer = new byte[NetworkLimits.UDP_PACKET_MAX];
		private INetworkProviderListener _listener;
		private Socket _socket;
		private Deps.KCP _kcp;
		private UInt32 _nextUpdate;
		private volatile NetworkProviderState _state = NetworkProviderState.Created;
		private bool _disposed;

		public KcpNetworkProvider(NetworkProviderContext context)
		{
			_context = context ?? throw new ArgumentNullException(nameof(context));
			if (context.Endpoint.UdpPort == 0)
				throw new ArgumentException("KCP requires a non-zero UDP port.", nameof(context));
			if (context.UdpSendWindowSize <= 0 || context.UdpReceiveWindowSize <= 0 || context.SendQueueSize <= 0)
				throw new ArgumentOutOfRangeException(nameof(context), "KCP window and send-queue limits must be greater than zero.");
		}

		public NetworkProviderState State => _state;

		public void Connect(INetworkProviderListener listener)
		{
			if (listener == null)
				throw new ArgumentNullException(nameof(listener));

			lock (_lifecycleLock)
			{
				if (_disposed)
					throw new ObjectDisposedException(nameof(KcpNetworkProvider));
				if (_state != NetworkProviderState.Created)
					throw new InvalidOperationException("KCP provider can connect only once.");

				_listener = listener;
				_state = NetworkProviderState.Connecting;
			}

			_ = Task.Run(HandshakeWorker);
		}

		public NetworkSendResult Send(IReadOnlyList<ArraySegment<byte>> packets)
		{
			if (packets == null)
				throw new ArgumentNullException(nameof(packets));

			lock (_lifecycleLock)
			{
				if (_disposed || _state != NetworkProviderState.Connected)
					return NetworkSendResult.NotConnected;

				lock (_kcpLock)
				{
					if (_kcp == null || _state != NetworkProviderState.Connected)
						return NetworkSendResult.NotConnected;

					try
					{
						BuildSendSegments(packets);
						int result = _kcp.SendBatch(_sendSegments, _context.SendQueueSize);
						if (result == -3)
							return NetworkSendResult.Backpressure;
						if (result < 0)
							return NetworkSendResult.Failed;

						_nextUpdate = 0;
						return NetworkSendResult.Accepted;
					}
					finally
					{
						// KCP 已复制已接受的数据；立即丢弃片段引用，避免持有随后归还对象池的 Bundle 数组。
						// KCP copies accepted data; drop segment references immediately so pooled Bundle arrays are never retained.
						_sendSegments.Clear();
					}
				}
			}
		}

		private void BuildSendSegments(IReadOnlyList<ArraySegment<byte>> packets)
		{
			_sendSegments.Clear();
			for (int packetIndex = 0; packetIndex < packets.Count; ++packetIndex)
			{
				ArraySegment<byte> packet = packets[packetIndex];
				if (packet.Array == null)
					throw new ArgumentException("A send segment has no backing array.", nameof(packets));

				int offset = packet.Offset;
				int remaining = packet.Count;
				if (remaining == 0)
				{
					_sendSegments.Add(packet);
					continue;
				}

				while (remaining > 0)
				{
					int count = Math.Min(remaining, KCP_MAX_MESSAGE_SIZE);
					_sendSegments.Add(new ArraySegment<byte>(packet.Array, offset, count));
					offset += count;
					remaining -= count;
				}
			}
		}

		public void Process()
		{
			DrainEvents();
			if (_state != NetworkProviderState.Connected)
				return;

			NetworkCloseInfo failure = null;
			lock (_kcpLock)
			{
				try
				{
					Socket socket = _socket;
					Deps.KCP kcp = _kcp;
					if (socket == null || kcp == null)
						return;

					while (socket.Available > 0)
					{
						int length = socket.Receive(_datagramBuffer, 0, _datagramBuffer.Length, SocketFlags.None);
						if (length <= 0)
							throw new SocketException((int)SocketError.ConnectionReset);
						if (kcp.Input(_datagramBuffer, 0, length) < 0)
							throw new InvalidOperationException("KCP rejected an incoming datagram.");

						_nextUpdate = 0;
					}

					UInt32 current = Deps.KCP.TimeUtils.iclock();
					if (_nextUpdate == 0 || Deps.KCP._itimediff(current, _nextUpdate) >= 0)
					{
						kcp.Update(current);
						_nextUpdate = kcp.Check(current);
					}
				}
				catch (Exception exception)
				{
					failure = new NetworkCloseInfo("KCP process failed.", exception);
				}
			}

			if (failure != null)
				CloseTransport(true, failure);
			else
				DrainKcpMessages();

			DrainEvents();
		}

		public void Close()
		{
			CloseTransport(false, null);
		}

		public void Dispose()
		{
			lock (_lifecycleLock)
			{
				if (_disposed)
					return;
				_disposed = true;
			}

			CloseTransport(false, null);
		}

		private void HandshakeWorker()
		{
			Socket socket = null;
			try
			{
				IPAddress address = ResolveAddress(_context.Endpoint.Host);
				socket = new Socket(address.AddressFamily, SocketType.Dgram, ProtocolType.Udp);
				lock (_lifecycleLock)
				{
					if (_disposed || _state != NetworkProviderState.Connecting)
					{
						socket.Close();
						return;
					}
					_socket = socket;
				}
				socket.Connect(address, _context.Endpoint.UdpPort);

				byte[] helloPacket = Encoding.ASCII.GetBytes(UDP_HELLO);
				byte[] buffer = new byte[NetworkLimits.UDP_PACKET_MAX];
				DateTime deadline = DateTime.UtcNow.AddMilliseconds(HANDSHAKE_TIMEOUT_MILLISECONDS);
				DateTime nextHelloTime = DateTime.MinValue;
				UInt32 conversationId = 0;

				while (DateTime.UtcNow < deadline)
				{
					if (_state != NetworkProviderState.Connecting)
						return;

					if (DateTime.UtcNow >= nextHelloTime)
					{
						socket.Send(helloPacket, 0, helloPacket.Length, SocketFlags.None);
						nextHelloTime = DateTime.UtcNow.AddMilliseconds(HELLO_RETRY_MILLISECONDS);
					}

					if (!socket.Poll(100000, SelectMode.SelectRead))
						continue;

					int length = socket.Receive(buffer);
					if (!TryParseHelloAck(buffer, length, out string version, out conversationId, out string error))
						throw new InvalidOperationException(error);
					if (!string.Equals(version, _context.ServerVersion, StringComparison.Ordinal))
						throw new InvalidOperationException("KCP server version mismatch: " + version + " != " + _context.ServerVersion);
					break;
				}

				if (conversationId == 0)
					throw new TimeoutException("KCP handshake timed out.");

				var kcp = new Deps.KCP(conversationId, this);
				kcp.SetOutput(OutputKcp);
				kcp.SetMTU(KCP_MTU);
				kcp.WndSize(_context.UdpSendWindowSize, _context.UdpReceiveWindowSize);
				kcp.NoDelay(1, 10, 2, 1);
				kcp.SetMinRTO(10);

				lock (_lifecycleLock)
				{
					if (_disposed || _state != NetworkProviderState.Connecting)
					{
						kcp.Release();
						socket.Close();
						return;
					}

					lock (_kcpLock)
					{
						_kcp = kcp;
						_nextUpdate = 0;
					}
					_state = NetworkProviderState.Connected;
				}

				EnqueueEvent(ProviderEvent.Connected());
			}
			catch (Exception exception)
			{
				socket?.Close();
				CloseTransport(true, new NetworkCloseInfo("KCP handshake failed.", exception));
			}
		}

		private void OutputKcp(byte[] data, int size, object userData)
		{
			Socket socket = _socket;
			if (socket == null || _state != NetworkProviderState.Connected)
				return;

			socket.Send(data, 0, size, SocketFlags.None);
		}

		private void DrainKcpMessages()
		{
			while (true)
			{
				int length;
				lock (_kcpLock)
				{
					Deps.KCP kcp = _kcp;
					if (kcp == null || _state != NetworkProviderState.Connected)
						return;

					int messageSize = kcp.PeekSize();
					if (messageSize < 0)
						return;

					EnsureProcessBufferCapacity(messageSize);
					length = kcp.Recv(_processBuffer, 0, messageSize);
					if (length < 0)
						throw new InvalidOperationException("KCP receive failed: " + length);
				}

				// listener 可能在协议错误时关闭 Session，因此回调不能持有 KCP 锁；同一数组只在回调返回后复用。
				// The listener may close the session on a protocol error, so invoke it without the KCP lock and reuse the array only after it returns.
				_listener?.OnDataReceived(_processBuffer, 0, length);
			}
		}

		private void EnsureProcessBufferCapacity(int required)
		{
			if (required <= _processBuffer.Length)
				return;

			int capacity = _processBuffer.Length;
			while (capacity < required)
				capacity = checked(capacity * 2);
			Array.Resize(ref _processBuffer, capacity);
		}

		private void CloseTransport(bool notify, NetworkCloseInfo closeInfo)
		{
			Socket socket;
			Deps.KCP kcp;
			lock (_lifecycleLock)
			{
				if (_state == NetworkProviderState.Closed || _state == NetworkProviderState.Failed)
					return;

				_state = notify ? NetworkProviderState.Failed : NetworkProviderState.Closing;
				lock (_kcpLock)
				{
					socket = _socket;
					kcp = _kcp;
					_socket = null;
					_kcp = null;
					_nextUpdate = 0;
				}
			}

			if (kcp != null)
			{
				kcp.SetOutput(null);
				kcp.Release();
			}
			socket?.Close();

			if (notify)
				EnqueueEvent(ProviderEvent.Closed(closeInfo));
			else
				_state = NetworkProviderState.Closed;
		}

		private void DrainEvents()
		{
			while (true)
			{
				ProviderEvent providerEvent;
				lock (_eventLock)
				{
					if (_events.Count == 0)
						return;
					providerEvent = _events.Dequeue();
				}

				if (providerEvent.IsConnected)
					_listener?.OnConnected();
				else
					_listener?.OnClosed(providerEvent.CloseInfo);
			}
		}

		private void EnqueueEvent(ProviderEvent providerEvent)
		{
			lock (_eventLock)
				_events.Enqueue(providerEvent);
		}

		private static bool TryParseHelloAck(
			byte[] buffer,
			int length,
			out string version,
			out UInt32 conversationId,
			out string error)
		{
			version = string.Empty;
			conversationId = 0;
			error = string.Empty;

			byte[] expectedAck = Encoding.ASCII.GetBytes(UDP_HELLO_ACK);
			int minimumLength = expectedAck.Length + 1 + 1 + sizeof(UInt32);
			if (buffer == null || length < minimumLength || length > buffer.Length)
			{
				error = "Malformed KCP hello acknowledgement length.";
				return false;
			}

			for (int index = 0; index < expectedAck.Length; ++index)
			{
				if (buffer[index] != expectedAck[index])
				{
					error = "KCP hello acknowledgement mismatch.";
					return false;
				}
			}

			if (buffer[expectedAck.Length] != 0)
			{
				error = "KCP hello acknowledgement is not NUL terminated.";
				return false;
			}

			int versionBegin = expectedAck.Length + 1;
			int versionEnd = versionBegin;
			while (versionEnd < length && buffer[versionEnd] != 0)
				++versionEnd;

			if (versionEnd == versionBegin || versionEnd >= length || length - versionEnd - 1 != sizeof(UInt32))
			{
				error = "Malformed KCP version or conversation field.";
				return false;
			}

			version = Encoding.ASCII.GetString(buffer, versionBegin, versionEnd - versionBegin);
			int offset = versionEnd + 1;
			conversationId = (UInt32)(buffer[offset] |
				(buffer[offset + 1] << 8) |
				(buffer[offset + 2] << 16) |
				(buffer[offset + 3] << 24));
			if (conversationId == 0)
			{
				error = "KCP conversation id is zero.";
				return false;
			}

			return true;
		}

		private static IPAddress ResolveAddress(string host)
		{
			if (IPAddress.TryParse(host, out IPAddress parsed))
				return parsed;

			IPAddress[] addresses = Dns.GetHostAddresses(host);
			for (int index = 0; index < addresses.Length; ++index)
			{
				if (addresses[index].AddressFamily == AddressFamily.InterNetwork)
					return addresses[index];
			}

			if (addresses.Length == 0)
				throw new SocketException((int)SocketError.HostNotFound);
			return addresses[0];
		}

		private sealed class ProviderEvent
		{
			private ProviderEvent(bool isConnected, NetworkCloseInfo closeInfo)
			{
				IsConnected = isConnected;
				CloseInfo = closeInfo;
			}

			public bool IsConnected { get; }
			public NetworkCloseInfo CloseInfo { get; }
			public static ProviderEvent Connected() => new ProviderEvent(true, null);
			public static ProviderEvent Closed(NetworkCloseInfo closeInfo) => new ProviderEvent(false, closeInfo);
		}
	}
}
