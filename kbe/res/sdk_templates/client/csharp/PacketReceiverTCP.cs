namespace KBEngine
{
	using System;
	using System.Net.Sockets;

	using MessageLengthEx = System.UInt32;

	/*
		包接收模块(与服务端网络部分的名称对应)
		处理网络数据的接收
	*/
	public class PacketReceiverTCP : PacketReceiverBase
	{
		private readonly TcpReceiveQueue _receiveQueue;
		private readonly byte[] _socketBuffer;
		private readonly byte[] _processBuffer;

		public PacketReceiverTCP(NetworkInterfaceBase networkInterface) : base(networkInterface)
		{
			int capacity = KBEngineApp.app.getInitArgs().getTCPRecvBufferSize();
			_receiveQueue = new TcpReceiveQueue(capacity);
			_socketBuffer = new byte[Math.Min(capacity, NetworkInterfaceBase.TCP_PACKET_MAX)];
			_processBuffer = new byte[capacity];
			_messageReader = new MessageReaderTCP();
		}

		~PacketReceiverTCP()
		{
			KBELog.DEBUG_MSG("PacketReceiverTCP::~PacketReceiverTCP(), destroyed!");
		}

		public override void process()
		{
			int length = _receiveQueue.drain(_processBuffer);
			if (length == 0)
				return;

			// 先在队列锁内复制并释放容量，再在锁外解密和派发 RPC，避免业务回调阻塞接收线程或形成锁重入。
			// Copy and release queue capacity under lock, then decrypt and dispatch RPCs outside it so callbacks cannot block the receiver or reenter the queue lock.
			EncryptionFilter filter = _networkInterface.fileter();
			if (filter != null)
				filter.recv(_messageReader, _processBuffer, 0, (MessageLengthEx)length);
			else
				_messageReader.process(_processBuffer, 0, (MessageLengthEx)length);
		}

		public override void stop()
		{
			_receiveQueue.stop();
		}

		protected override void _asyncReceive()
		{
			NetworkInterfaceBase networkInterface = _networkInterface;
			if (networkInterface == null || !networkInterface.valid())
			{
				KBELog.WARNING_MSG("PacketReceiverTCP::_asyncReceive(): network interface invalid!");
				return;
			}

			Socket socket = networkInterface.sock();
			while (true)
			{
				int bytesRead;
				try
				{
					bytesRead = socket.Receive(_socketBuffer, 0, _socketBuffer.Length, SocketFlags.None);
				}
				catch (SocketException exception)
				{
					// reset/destroy 会先使接口失效再关闭 socket，接收任务此时被本地主动唤醒，不应伪造网络错误或重复关闭事件。
					// reset/destroy invalidates the interface before closing its socket, so the locally awakened receiver must not report a network fault or duplicate close event.
					if (!networkInterface.valid())
						return;

					KBELog.ERROR_MSG(string.Format("PacketReceiverTCP::_asyncReceive(): receive error, disconnect! error = '{0}'", exception));
					Event.fireIn("_closeNetwork", new object[] { networkInterface });
					return;
				}

				if (bytesRead <= 0)
				{
					if (!networkInterface.valid())
						return;

					KBELog.WARNING_MSG("PacketReceiverTCP::_asyncReceive(): receive 0 bytes, disconnect!");
					Event.fireIn("_closeNetwork", new object[] { networkInterface });
					return;
				}

				if (!_receiveQueue.write(_socketBuffer, 0, bytesRead))
					return;
			}
		}
	}
}
