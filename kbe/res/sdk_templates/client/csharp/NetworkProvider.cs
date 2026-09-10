namespace KBEngine
{
	using System;
	using System.Collections.Generic;

	public enum NetworkConnectionRole
	{
		Loginapp,
		Baseapp,
	}

	public enum NetworkProviderState
	{
		Created,
		Connecting,
		Connected,
		Closing,
		Closed,
		Failed,
	}

	public enum NetworkSendResult
	{
		Accepted,
		NotConnected,
		Backpressure,
		Failed,
	}

	public sealed class NetworkEndpoint
	{
		public NetworkEndpoint(string host, int tcpPort, int udpPort)
		{
			if (string.IsNullOrWhiteSpace(host))
				throw new ArgumentException("Network host cannot be empty.", nameof(host));
			if (tcpPort < 0 || tcpPort > UInt16.MaxValue)
				throw new ArgumentOutOfRangeException(nameof(tcpPort));
			if (udpPort < 0 || udpPort > UInt16.MaxValue)
				throw new ArgumentOutOfRangeException(nameof(udpPort));

			Host = host;
			TcpPort = tcpPort;
			UdpPort = udpPort;
		}

		public string Host { get; }
		public int TcpPort { get; }
		public int UdpPort { get; }
	}

	public sealed class NetworkProviderContext
	{
		internal NetworkProviderContext(
			NetworkConnectionRole role,
			NetworkEndpoint endpoint,
			KBEngineArgs args,
			string serverVersion)
		{
			if (args == null)
				throw new ArgumentNullException(nameof(args));

			Role = role;
			Endpoint = endpoint ?? throw new ArgumentNullException(nameof(endpoint));
			ServerVersion = serverVersion ?? string.Empty;
			TcpSendBufferSize = args.getTCPSendBufferSize();
			TcpReceiveBufferSize = args.getTCPRecvBufferSize();
			UdpSendWindowSize = args.getUDPSendBufferSize();
			UdpReceiveWindowSize = args.getUDPRecvBufferSize();
			SendQueueSize = args.getSendQueueSize();
		}

		public NetworkConnectionRole Role { get; }
		public NetworkEndpoint Endpoint { get; }
		public string ServerVersion { get; }
		public int TcpSendBufferSize { get; }
		public int TcpReceiveBufferSize { get; }
		public int UdpSendWindowSize { get; }
		public int UdpReceiveWindowSize { get; }
		public int SendQueueSize { get; }
	}

	public sealed class NetworkCloseInfo
	{
		public NetworkCloseInfo(string message, Exception exception = null)
		{
			Message = message ?? string.Empty;
			Exception = exception;
		}

		public string Message { get; }
		public Exception Exception { get; }
	}

	public interface INetworkProviderListener
	{
		void OnConnected();
		void OnDataReceived(byte[] buffer, int offset, int count);
		void OnClosed(NetworkCloseInfo closeInfo);
	}

	// Provider 可以在内部使用线程或平台回调，但必须把结果排队，并且只在 Process 调用期间同步通知 listener。
	// A provider may use workers or platform callbacks internally, but it must queue results and notify the listener synchronously only from Process.
	public interface INetworkProvider : IDisposable
	{
		NetworkProviderState State { get; }

		void Connect(INetworkProviderListener listener);
		// Accepted 表示 Provider 已在返回前复制或接管所有字节；不得保留指向 Bundle 对象池内存的引用。
		// Accepted means the provider copied or took ownership of every byte before returning; it must not retain references to pooled Bundle memory.
		NetworkSendResult Send(IReadOnlyList<ArraySegment<byte>> packets);
		void Process();
		void Close();
	}

	public interface INetworkProviderFactory
	{
		INetworkProvider Create(NetworkProviderContext context);
	}
}
