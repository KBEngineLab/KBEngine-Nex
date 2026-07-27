namespace KBEngine
{
	
	using System;
	using System.Net.Sockets;
	using System.Net;
	using System.Collections;
	using System.Collections.Generic;
	using System.Text;
	using System.Text.RegularExpressions;
	using System.Threading;

	using MessageID = System.UInt16;
	using MessageLength = System.UInt16;

	/// <summary>
	/// 网络模块
	/// 处理连接、收发数据
	/// </summary>
	public class NetworkInterfaceKCP : NetworkInterfaceBase
	{
		private const int HandshakeTimeoutMilliseconds = 30000;
		private const int HelloRetryMilliseconds = 1000;

		private Deps.KCP kcp_ = null;
		public UInt32 connID;
		public UInt32 nextTickKcpUpdate = 0;
		public EndPoint remoteEndPint = null;

		protected override Socket createSocket()
		{
			Socket pSocket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
			return pSocket;
		}

		protected override PacketReceiverBase createPacketReceiver()
		{
			return new PacketReceiverKCP(this);
		}

		protected override PacketSenderBase createPacketSender()
		{
			return new PacketSenderKCP(this);
		}

		public override void reset()
		{
			finiKCP();
			base.reset();
		}
		
        public override void close()
        {
			finiKCP();
			base.close();
        }

		public override bool valid()
		{
			return ((kcp_ != null) && (_socket != null) && connected);
		}

		protected void outputKCP(byte[] data, int size, object userData)
		{
			if (!valid())
			{
				throw new ArgumentException("invalid socket!");
			}

			if (_packetSender == null)
				_packetSender = createPacketSender();

			((PacketSenderKCP)_packetSender).sendto(data, size);
		}

		bool initKCP()
		{
            kcp_ = new Deps.KCP(connID, this);
            kcp_.SetOutput(outputKCP);

            kcp_.SetMTU(1400);
            kcp_.WndSize(KBEngineApp.app.getInitArgs().getUDPSendBufferSize(), KBEngineApp.app.getInitArgs().getUDPRecvBufferSize());
            kcp_.NoDelay(1, 10, 2, 1);
            kcp_.SetMinRTO(10);

			nextTickKcpUpdate = 0;
			return true;
		}

		bool finiKCP()
		{
			if(kcp_ != null)
			{
				kcp_.SetOutput(null);
				kcp_.Release();
				kcp_ = null;
			}

			remoteEndPint = null;
			connID = 0;
			nextTickKcpUpdate = 0;
			return true;
		}

		public Deps.KCP kcp()
		{
			return kcp_;
		}

		public override bool send(MemoryStream stream)
		{
			if (!valid())
			{
				throw new ArgumentException("invalid socket!");
			}

            if(_filter != null)
            {
                _filter.encrypt(stream);
            }

			nextTickKcpUpdate = 0;
			return kcp_.Send(stream.data(), stream.rpos, (int)stream.length()) >= 0;
		}

		public override void process()
		{
			if (!valid())
				return;

			uint current = Deps.KCP.TimeUtils.iclock();
			if(current >= nextTickKcpUpdate)
			{
				kcp_.Update(current);
				nextTickKcpUpdate = kcp_.Check(current);
			}

			if (_packetReceiver != null)
				_packetReceiver.process();
		}

		protected override void onAsyncConnectCB(ConnectState state)
		{
			if(state.error.Length > 0)
				return;

			if (!initKCP())
			{
				state.error = "failed to initialize KCP";
				return;
			}

			connected = true;
			remoteEndPint = state.socket.RemoteEndPoint;
		}

		protected override void onAsyncConnect(ConnectState state)
		{
			try
			{
				// 连接 UDP socket 会固定远端地址，使后续 Receive 自动拒绝其他来源伪造的握手和 KCP 数据报。
				// Connecting the UDP socket pins the peer so subsequent Receive calls reject spoofed handshake and KCP datagrams from other sources.
				state.socket.Connect(state.connectIP, state.connectPort);
				byte[] helloPacket = System.Text.Encoding.ASCII.GetBytes(UDP_HELLO);
				byte[] buffer = new byte[UDP_PACKET_MAX];
				DateTime deadline = DateTime.UtcNow.AddMilliseconds(HandshakeTimeoutMilliseconds);
				DateTime nextHelloTime = DateTime.MinValue;

				while (DateTime.UtcNow < deadline)
				{
					if (DateTime.UtcNow >= nextHelloTime)
					{
						state.socket.Send(helloPacket, 0, helloPacket.Length, SocketFlags.None);
						nextHelloTime = DateTime.UtcNow.AddMilliseconds(HelloRetryMilliseconds);
					}

					if (!state.socket.Poll(100000, SelectMode.SelectRead))
						continue;

					int length = state.socket.Receive(buffer);
					if (!tryParseHelloAck(buffer, length, out string versionString, out uint conv, out string parseError))
					{
						state.error = parseError;
						break;
					}

					if (KBEngineApp.app.serverVersion != versionString)
					{
						state.error = string.Format("version mismatch ({0}!={1})", versionString, KBEngineApp.app.serverVersion);
						break;
					}

					((NetworkInterfaceKCP)state.networkInterface).connID = conv;
					return;
				}

				if (state.error.Length == 0)
					state.error = "KCP handshake timeout";

				KBELog.ERROR_MSG(string.Format("NetworkInterfaceKCP::_asyncConnect(), failed to connect to '{0}:{1}': {2}",
					state.connectIP, state.connectPort, state.error));
			}
			catch (Exception e)
			{
				KBELog.ERROR_MSG(string.Format("NetworkInterfaceKCP::_asyncConnect(), connect to '{0}:{1}' fault! error = '{2}'", state.connectIP, state.connectPort, e));
				state.error = e.ToString();
			}
		}

		private static bool tryParseHelloAck(byte[] buffer, int length, out string versionString, out uint conv, out string error)
		{
			versionString = string.Empty;
			conv = 0;
			error = string.Empty;

			byte[] expectedAck = Encoding.ASCII.GetBytes(UDP_HELLO_ACK);
			int minimumLength = expectedAck.Length + 1 + 1 + sizeof(uint);
			if (buffer == null || length < minimumLength || length > buffer.Length)
			{
				error = "malformed KCP hello acknowledgement length";
				return false;
			}

			for (int index = 0; index < expectedAck.Length; ++index)
			{
				if (buffer[index] != expectedAck[index])
				{
					error = "KCP hello acknowledgement mismatch";
					return false;
				}
			}

			if (buffer[expectedAck.Length] != 0)
			{
				error = "KCP hello acknowledgement is not NUL terminated";
				return false;
			}

			int versionBegin = expectedAck.Length + 1;
			int versionEnd = versionBegin;
			while (versionEnd < length && buffer[versionEnd] != 0)
				++versionEnd;

			if (versionEnd == versionBegin || versionEnd >= length || length - versionEnd - 1 != sizeof(uint))
			{
				error = "malformed KCP version or conv field";
				return false;
			}

			versionString = Encoding.ASCII.GetString(buffer, versionBegin, versionEnd - versionBegin);
			int convOffset = versionEnd + 1;

			// 握手协议固定使用小端 uint32，逐字节解码以避免依赖运行平台的字节序。
			// The handshake uses a little-endian uint32; byte-wise decoding avoids depending on runtime platform endianness.
			conv = (uint)(buffer[convOffset] |
				(buffer[convOffset + 1] << 8) |
				(buffer[convOffset + 2] << 16) |
				(buffer[convOffset + 3] << 24));

			if (conv == 0)
			{
				error = "KCP conv is zero";
				return false;
			}

			return true;
		}
	}
}
