namespace KBEngine
{
	using System;
	using System.Collections.Generic;
	using System.Threading.Tasks;
	using NativeWebSocket;

	/*
		WebSocket 包发送模块
		处理有界队列与异步发送
		WebSocket packet sender
		Handles bounded queuing and asynchronous writes
	*/
	public class PacketSenderUnityWS : PacketSenderBase
	{
		private readonly TcpSendQueue _sendQueue;

		public PacketSenderUnityWS(NetworkInterfaceBase networkInterface) : base(networkInterface)
		{
			_sendQueue = new TcpSendQueue(KBEngineApp.app.getInitArgs().getSendQueueSize());
		}

		~PacketSenderUnityWS()
		{
			KBELog.DEBUG_MSG("PacketSenderUnityWS::~PacketSenderUnityWS(), destroyed!");
		}

		public override bool send(MemoryStream stream)
		{
			return send(new MemoryStream[] { stream });
		}

		public override bool send(IReadOnlyList<MemoryStream> streams)
		{
			var segments = new ArraySegment<byte>[streams.Count];
			long totalLength = 0;
			for (int index = 0; index < streams.Count; ++index)
			{
				MemoryStream stream = streams[index];
				int length = checked((int)stream.length());
				segments[index] = new ArraySegment<byte>(stream.data(), stream.rpos, length);
				totalLength += length;
			}

			if (!_sendQueue.tryEnqueue(segments, out bool startWorker))
			{
				KBELog.ERROR_MSG("PacketSenderUnityWS::send(): no space, Please adjust 'SEND_QUEUE_MAX'! batch(" +
					totalLength + ") > available queue capacity, capacity=" + _sendQueue.capacity);
				return false;
			}

			if (startWorker)
				_ = asyncWebSocketSend();

			return true;
		}

		protected override void _asyncSend()
		{
			throw new NotSupportedException("WebSocket sending uses its native asynchronous API.");
		}

		private async Task asyncWebSocketSend()
		{
			NetworkInterfaceBase networkInterface = _networkInterface;
			try
			{
				if (networkInterface == null || !networkInterface.valid())
					throw new InvalidOperationException("WebSocket network interface is invalid.");

				WebSocket socket = ((NetworkInterfaceUnityWS)networkInterface).GetWebSocket();
				byte[] buffer;
				int offset;
				int count;
				while (_sendQueue.tryGetContiguousData(out buffer, out offset, out count))
				{
					// WebSocket API 不接受数组切片，因此每个连续环形片段只复制一次；等待期间队列容量仍被占用以维持严格背压。
					// The WebSocket API does not accept an array slice, so each contiguous ring segment is copied once while its queue capacity remains reserved during the await for strict backpressure.
					byte[] packet = new byte[count];
					Buffer.BlockCopy(buffer, offset, packet, 0, count);
					await socket.Send(packet);
					_sendQueue.consume(count);
				}
			}
			catch (Exception exception)
			{
				_sendQueue.abort();
				KBELog.ERROR_MSG("PacketSenderUnityWS::asyncWebSocketSend(): send data error, disconnect! error = '" + exception + "'");
				if (networkInterface != null)
					Event.fireIn("_closeNetwork", new object[] { networkInterface });
			}
		}
	}
}
