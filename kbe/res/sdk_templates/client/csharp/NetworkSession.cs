namespace KBEngine
{
	using System;
	using System.Collections.Generic;

	public sealed class NetworkSession : INetworkProviderListener, IDisposable
	{
		public delegate void ConnectCallback(bool success, NetworkCloseInfo closeInfo);
		public delegate void ClosedCallback(NetworkSession session, NetworkCloseInfo closeInfo);

		private readonly INetworkProvider _provider;
		private readonly KBEMessageReader _messageReader;
		private readonly ClosedCallback _closedCallback;
		private readonly NetworkLifecycleState _lifecycleState = new NetworkLifecycleState();
		private readonly object _operationLock = new object();
		private ConnectCallback _connectCallback;
		private INetworkFrameCodec _frameCodec;
		private bool _connectResultDelivered;
		private bool _terminal;
		private bool _closedCallbackDelivered;
		private bool _disposed;

		public NetworkSession(
			NetworkConnectionRole role,
			NetworkEndpoint endpoint,
			INetworkProvider provider,
			int maximumMessageSize,
			ClosedCallback closedCallback)
		{
			Role = role;
			Endpoint = endpoint ?? throw new ArgumentNullException(nameof(endpoint));
			_provider = provider ?? throw new ArgumentNullException(nameof(provider));
			_messageReader = new KBEMessageReader(maximumMessageSize);
			_closedCallback = closedCallback;
		}

		public NetworkConnectionRole Role { get; }
		public NetworkEndpoint Endpoint { get; }
		public bool Connected
		{
			get
			{
				lock (_operationLock)
					return IsConnected();
			}
		}
		public NetworkProviderState State => _provider.State;

		public void Connect(ConnectCallback callback)
		{
			lock (_operationLock)
			{
				if (_disposed)
					throw new ObjectDisposedException(nameof(NetworkSession));
				if (_connectCallback != null || _connectResultDelivered)
					throw new InvalidOperationException("The network session has already started connecting.");

				_connectCallback = callback;
				try
				{
					_provider.Connect(this);
				}
				catch (Exception exception)
				{
					NotifyClosed(new NetworkCloseInfo("Network provider connect failed.", exception));
				}
			}
		}

		public NetworkSendResult Send(IReadOnlyList<MemoryStream> streams)
		{
			if (streams == null)
				throw new ArgumentNullException(nameof(streams));

			lock (_operationLock)
			{
				if (!IsConnected())
					return NetworkSendResult.NotConnected;

				try
				{
					var packets = new ArraySegment<byte>[streams.Count];
					for (int index = 0; index < streams.Count; ++index)
					{
						MemoryStream stream = streams[index] ??
							throw new ArgumentException("A network stream cannot be null.", nameof(streams));

						_frameCodec?.Encode(stream);
						packets[index] = new ArraySegment<byte>(
							stream.data(), stream.rpos, checked((int)stream.length()));
					}

					return _provider.Send(packets);
				}
				catch (Exception exception)
				{
					KBELog.ERROR_MSG("NetworkSession::Send(): " + exception);
					return NetworkSendResult.Failed;
				}
			}
		}

		public void Process()
		{
			lock (_operationLock)
			{
				if (_disposed)
					return;

				try
				{
					_provider.Process();
				}
				catch (Exception exception)
				{
					NotifyClosed(new NetworkCloseInfo("Network provider process failed.", exception));
				}
			}
		}

		public void SetFrameCodec(INetworkFrameCodec frameCodec)
		{
			lock (_operationLock)
			{
				if (_disposed)
					throw new ObjectDisposedException(nameof(NetworkSession));

				_frameCodec?.Reset();
				_frameCodec = frameCodec;
			}
		}

		public void Reset()
		{
			Shutdown(false);
		}

		public void Close()
		{
			Shutdown(true);
		}

		public void Dispose()
		{
			Shutdown(false);
		}

		void INetworkProviderListener.OnConnected()
		{
			lock (_operationLock)
			{
				if (_disposed || _terminal || _connectResultDelivered ||
					_provider.State != NetworkProviderState.Connected)
					return;

				_lifecycleState.arm();
				CompleteConnect(true, null);
			}
		}

		void INetworkProviderListener.OnDataReceived(byte[] buffer, int offset, int count)
		{
			lock (_operationLock)
			{
				if (!IsConnected())
					return;

				try
				{
					if (_frameCodec != null)
						_frameCodec.Decode(buffer, offset, count, _messageReader.Process);
					else
						_messageReader.Process(buffer, offset, count);
				}
				catch (Exception exception)
				{
					NotifyClosed(new NetworkCloseInfo("Failed to decode a network message.", exception));
				}
			}
		}

		void INetworkProviderListener.OnClosed(NetworkCloseInfo closeInfo)
		{
			lock (_operationLock)
				NotifyClosed(closeInfo ?? new NetworkCloseInfo("Network connection closed."));
		}

		private void NotifyClosed(NetworkCloseInfo closeInfo)
		{
			if (_disposed || _terminal)
				return;

			_terminal = true;
			bool wasConnected = _connectResultDelivered;
			try
			{
				if (!wasConnected)
				{
					CompleteConnect(false, closeInfo);
				}
				else if (!_closedCallbackDelivered)
				{
					_closedCallbackDelivered = true;
					_closedCallback?.Invoke(this, closeInfo);
				}
			}
			catch (Exception exception)
			{
				// 生命周期回调属于应用层代码；它的异常不能阻止 socket、KCP 与加密状态的确定性释放。
				// Lifecycle callbacks are application code; their exceptions must not prevent deterministic socket, KCP, and codec cleanup.
				KBELog.ERROR_MSG("NetworkSession::NotifyClosed(): lifecycle callback failed: " + exception);
			}
			finally
			{
				// 连接失败没有断线语义；只有完成过连接的 Session 才触发一次 onDisconnected。
				// A failed connection has no disconnect semantics; only an established session fires onDisconnected once.
				Shutdown(wasConnected);
			}
		}

		private void CompleteConnect(bool success, NetworkCloseInfo closeInfo)
		{
			if (_connectResultDelivered)
				return;

			_connectResultDelivered = true;
			ConnectCallback callback = _connectCallback;
			_connectCallback = null;
			callback?.Invoke(success, closeInfo);
		}

		private void Shutdown(bool notifyDisconnected)
		{
			lock (_operationLock)
			{
				ShutdownLocked(notifyDisconnected);
			}
		}

		private bool IsConnected()
		{
			return !_disposed && !_terminal &&
				_provider.State == NetworkProviderState.Connected;
		}

		private void ShutdownLocked(bool notifyDisconnected)
		{
			if (_disposed)
				return;

			_disposed = true;
			_terminal = true;
			try
			{
				_provider.Close();
			}
			catch (Exception exception)
			{
				KBELog.WARNING_MSG("NetworkSession::Shutdown(): provider close failed: " + exception);
			}

			try
			{
				_provider.Dispose();
			}
			catch (Exception exception)
			{
				KBELog.WARNING_MSG("NetworkSession::Shutdown(): provider dispose failed: " + exception);
			}

			try
			{
				_frameCodec?.Reset();
			}
			catch (Exception exception)
			{
				KBELog.WARNING_MSG("NetworkSession::Shutdown(): frame codec reset failed: " + exception);
			}

			_frameCodec = null;
			_messageReader.Reset();

			if (_lifecycleState.consume(notifyDisconnected))
				Event.fireAll(EventOutTypes.onDisconnected);
		}
	}
}
