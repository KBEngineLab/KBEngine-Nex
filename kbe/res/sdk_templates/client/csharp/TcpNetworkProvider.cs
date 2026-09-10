namespace KBEngine
{
	using System;
	using System.Collections.Generic;
	using System.Net;
	using System.Net.Sockets;
	using System.Threading.Tasks;

	public sealed class TcpNetworkProvider : INetworkProvider
	{
		private readonly object _lifecycleLock = new object();
		private readonly object _eventLock = new object();
		private readonly Queue<ProviderEvent> _events = new Queue<ProviderEvent>();
		private readonly NetworkProviderContext _context;
		private readonly TcpReceiveQueue _receiveQueue;
		private readonly TcpSendQueue _sendQueue;
		private readonly byte[] _socketBuffer;
		private readonly byte[] _processBuffer;
		private INetworkProviderListener _listener;
		private Socket _socket;
		private volatile NetworkProviderState _state = NetworkProviderState.Created;
		private bool _disposed;

		public TcpNetworkProvider(NetworkProviderContext context)
		{
			_context = context ?? throw new ArgumentNullException(nameof(context));
			if (context.TcpSendBufferSize <= 0 || context.TcpReceiveBufferSize <= 0 || context.SendQueueSize <= 0)
				throw new ArgumentOutOfRangeException(nameof(context), "TCP buffer and send-queue limits must be greater than zero.");

			_receiveQueue = new TcpReceiveQueue(context.TcpReceiveBufferSize);
			_sendQueue = new TcpSendQueue(context.SendQueueSize);
			_socketBuffer = new byte[Math.Min(context.TcpReceiveBufferSize, NetworkLimits.TCP_PACKET_MAX)];
			_processBuffer = new byte[context.TcpReceiveBufferSize];
		}

		public NetworkProviderState State => _state;

		public void Connect(INetworkProviderListener listener)
		{
			if (listener == null)
				throw new ArgumentNullException(nameof(listener));

			lock (_lifecycleLock)
			{
				if (_disposed)
					throw new ObjectDisposedException(nameof(TcpNetworkProvider));
				if (_state != NetworkProviderState.Created)
					throw new InvalidOperationException("TCP provider can connect only once.");

				_listener = listener;
				_state = NetworkProviderState.Connecting;
			}

			_ = Task.Run(ConnectWorker);
		}

		public NetworkSendResult Send(IReadOnlyList<ArraySegment<byte>> packets)
		{
			if (packets == null)
				throw new ArgumentNullException(nameof(packets));

			bool startWorker;
			lock (_lifecycleLock)
			{
				if (_disposed || _state != NetworkProviderState.Connected)
					return NetworkSendResult.NotConnected;

				// 状态检查与原子批次入队共用生命周期锁，避免关闭转换后仍向调用方报告 Accepted。
				// Share the lifecycle lock across the state check and atomic enqueue so a closing transport cannot still report Accepted.
				if (!_sendQueue.tryEnqueue(packets, out startWorker))
					return NetworkSendResult.Backpressure;
			}

			if (startWorker)
				_ = Task.Run(SendWorker);

			return NetworkSendResult.Accepted;
		}

		public void Process()
		{
			DrainEvents();
			if (_state != NetworkProviderState.Connected)
				return;

			int length = _receiveQueue.drain(_processBuffer);
			if (length > 0)
				_listener?.OnDataReceived(_processBuffer, 0, length);

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

		private void ConnectWorker()
		{
			Socket socket = null;
			try
			{
				IPAddress address = ResolveAddress(_context.Endpoint.Host);
				socket = new Socket(address.AddressFamily, SocketType.Stream, ProtocolType.Tcp);
				socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReceiveBuffer, _context.TcpReceiveBufferSize * 2);
				socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.SendBuffer, _context.TcpSendBufferSize * 2);
				socket.NoDelay = true;
				lock (_lifecycleLock)
				{
					if (_disposed || _state != NetworkProviderState.Connecting)
					{
						socket.Close();
						return;
					}
					_socket = socket;
				}
				socket.Connect(address, _context.Endpoint.TcpPort);

				lock (_lifecycleLock)
				{
					if (_disposed || _state != NetworkProviderState.Connecting)
					{
						socket.Close();
						return;
					}

					_state = NetworkProviderState.Connected;
				}

				EnqueueEvent(ProviderEvent.Connected());
				_ = Task.Run(ReceiveWorker);
			}
			catch (Exception exception)
			{
				socket?.Close();
				CloseTransport(true, new NetworkCloseInfo("TCP connect failed.", exception));
			}
		}

		private void ReceiveWorker()
		{
			try
			{
				while (_state == NetworkProviderState.Connected)
				{
					Socket socket = _socket;
					if (socket == null)
						return;

					int bytesRead = socket.Receive(_socketBuffer, 0, _socketBuffer.Length, SocketFlags.None);
					if (bytesRead <= 0)
						throw new SocketException((int)SocketError.ConnectionReset);
					if (!_receiveQueue.write(_socketBuffer, 0, bytesRead))
						return;
				}
			}
			catch (Exception exception)
			{
				if (_state == NetworkProviderState.Connected)
					CloseTransport(true, new NetworkCloseInfo("TCP receive failed.", exception));
			}
		}

		private void SendWorker()
		{
			try
			{
				byte[] buffer;
				int offset;
				int count;
				while (_state == NetworkProviderState.Connected &&
					_sendQueue.tryGetContiguousData(out buffer, out offset, out count))
				{
					Socket socket = _socket;
					if (socket == null)
						throw new SocketException((int)SocketError.NotConnected);

					int bytesSent = socket.Send(buffer, offset, count, SocketFlags.None);
					if (bytesSent <= 0)
						throw new SocketException((int)SocketError.ConnectionReset);
					_sendQueue.consume(bytesSent);
				}
			}
			catch (Exception exception)
			{
				_sendQueue.abort();
				CloseTransport(true, new NetworkCloseInfo("TCP send failed.", exception));
			}
		}

		private void CloseTransport(bool notify, NetworkCloseInfo closeInfo)
		{
			Socket socket;
			lock (_lifecycleLock)
			{
				if (_state == NetworkProviderState.Closed || _state == NetworkProviderState.Failed)
					return;

				_state = notify ? NetworkProviderState.Failed : NetworkProviderState.Closing;
				socket = _socket;
				_socket = null;
			}

			_receiveQueue.stop();
			_sendQueue.abort();
			if (socket != null)
			{
				try { socket.Shutdown(SocketShutdown.Both); } catch { }
				socket.Close();
			}

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
