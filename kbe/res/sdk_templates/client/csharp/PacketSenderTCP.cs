namespace KBEngine
{
	using System;
	using System.Net.Sockets;

	/*
		包发送模块(与服务端网络部分的名称对应)
		处理网络数据的发送
	*/
	public class PacketSenderTCP : PacketSenderBase
	{
		private readonly TcpSendQueue _sendQueue;

		public PacketSenderTCP(NetworkInterfaceBase networkInterface) : base(networkInterface)
		{
			// CLR 数组使用 Int32 索引；checked 转换让无效的超大配置在初始化时明确失败，而不是产生截断后的队列。
			// CLR arrays use Int32 indices; a checked conversion rejects an invalid oversized setting during initialization instead of creating a truncated queue.
			_sendQueue = new TcpSendQueue(checked((int)KBEngineApp.app.getInitArgs().TCP_SEND_BUFFER_MAX));
		}

		~PacketSenderTCP()
		{
			KBELog.DEBUG_MSG("PacketSenderTCP::~PacketSenderTCP(), destroyed!");
		}

		public override bool send(MemoryStream stream)
		{
			int dataLength = (int)stream.length();
			if (dataLength <= 0)
				return true;

			bool startWorker;
			if (!_sendQueue.tryEnqueue(stream.data(), stream.rpos, dataLength, out startWorker))
			{
				KBELog.ERROR_MSG("PacketSenderTCP::send(): no space, Please adjust 'TCP_SEND_BUFFER_MAX'! data(" +
					dataLength + ") > available queue capacity, capacity=" + _sendQueue.capacity);
				return false;
			}

			if (startWorker)
				_startSend();

			return true;
		}

		protected override void _asyncSend()
		{
			NetworkInterfaceBase networkInterface = _networkInterface;
			if (networkInterface == null || !networkInterface.valid())
			{
				KBELog.WARNING_MSG("PacketSenderTCP::_asyncSend(): network interface invalid!");
				_sendQueue.abort();
				return;
			}

			Socket socket = networkInterface.sock();
			try
			{
				byte[] buffer;
				int offset;
				int count;
				while (_sendQueue.tryGetContiguousData(out buffer, out offset, out count))
				{
					// 队列在 send 完成前不会释放当前片段的容量，因此生产者无法覆盖锁外发送所引用的字节。
					// The queue does not release this segment until send completes, so producers cannot overwrite bytes referenced by the lock-free socket operation.
					int bytesSent = socket.Send(buffer, offset, count, SocketFlags.None);
					if (bytesSent <= 0)
						throw new SocketException((int)SocketError.ConnectionReset);

					_sendQueue.consume(bytesSent);
				}
			}
			catch (Exception exception)
			{
				_sendQueue.abort();
				KBELog.ERROR_MSG(string.Format("PacketSenderTCP::_asyncSend(): send data error, disconnect! error = '{0}'", exception));
				Event.fireIn("_closeNetwork", new object[] { networkInterface });
			}
		}
	}
}
