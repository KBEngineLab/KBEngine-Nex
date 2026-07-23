

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

	using System.Threading.Tasks;
	using NativeWebSocket;

	/*
		包发送模块(与服务端网络部分的名称对应)
		处理网络数据的发送
	*/
    public class PacketSenderUnityWS : PacketSenderBase
    {
		WebSocket socket_;
		
		private byte[] _buffer;

		int _wpos = 0;				// 写入的数据位置
		int _spos = 0;				// 发送完毕的数据位置

		object _sendingObj = new object();
		Boolean _sending = false;

        public PacketSenderUnityWS(NetworkInterfaceBase networkInterface) : base(networkInterface)
        {
	        socket_ = ((NetworkInterfaceUnityWS)_networkInterface).GetWebSocket();
	        
	        _buffer = new byte[KBEngineApp.app.getInitArgs().TCP_SEND_BUFFER_MAX];

	        _wpos = 0; 
	        _spos = 0;
	        _sending = false;
        }

		~PacketSenderUnityWS()
		{
			KBELog.DEBUG_MSG("PacketSenderUnityWS::~PacketSenderUnityWS(), destroyed!");
		}
		
		
		public override bool send(MemoryStream stream)
		{
		
			int dataLength = (int)stream.length();
			if (dataLength <= 0)
				return true;
			
			Monitor.Enter(_sendingObj);
			if (!_sending)
			{
				if (_wpos == _spos)
				{
					_wpos = 0;
					_spos = 0;
				}
			}
			
			int t_spos =_spos;
			int space = 0;
			int tt_wpos = _wpos % _buffer.Length;
			int tt_spos = t_spos % _buffer.Length;
			
			if(tt_wpos >= tt_spos)
				space = _buffer.Length - tt_wpos + tt_spos - 1;
			else
				space = tt_spos - tt_wpos - 1;
			
			if (dataLength > space)
			{
				KBELog.ERROR_MSG("PacketSenderTCP::send(): no space, Please adjust 'SEND_BUFFER_MAX'! data(" + dataLength 
					+ ") > space(" + space + "), wpos=" + _wpos + ", spos=" + t_spos);
				
				return false;
			}
			
			int expect_total = tt_wpos + dataLength;
			if(expect_total <= _buffer.Length)
			{
				Array.Copy(stream.data(), stream.rpos, _buffer, tt_wpos, dataLength);
			}
			else
			{
				int remain = _buffer.Length - tt_wpos;
				Array.Copy(stream.data(), stream.rpos, _buffer, tt_wpos, remain);
				Array.Copy(stream.data(), stream.rpos + remain, _buffer, 0, expect_total - _buffer.Length);
			}
			
			_wpos += dataLength;
			
			if (!_sending)
			{
				_sending = true;
				Monitor.Exit(_sendingObj);
			
				_ = _asyncWebsocketSend();
			}
			else
			{
				Monitor.Exit(_sendingObj);
			}
			return true;
		}


		// protected override void _startSend()
		// {
		// 	try
		// 	{
		// 		_ = _asyncSend();
		// 	}
		// 	catch (Exception ex)
		// 	{
		// 		// 捕获异常，避免线程崩溃
		// 		KBELog.ERROR_MSG($"PacketSenderBase::_asyncSend error: {ex}");
		// 	}
		// }


		protected override void _asyncSend()
		{
			throw new NotImplementedException();
		}

		private async Task _asyncWebsocketSend()
		{
			if (_networkInterface == null || !_networkInterface.valid())
			{
				KBELog.WARNING_MSG("PacketSenderWS::_asyncSend(): network interface invalid!");
				return;
			}

			var socket = ((NetworkInterfaceUnityWS)_networkInterface).GetWebSocket();

			while (true)
			{

				Monitor.Enter(_sendingObj);

				int sendSize = _wpos - _spos;
				if (sendSize <= 0)
				{
					_sending = false;
					Monitor.Exit(_sendingObj);
					return;
				}

				int t_spos = _spos % _buffer.Length;
				if (t_spos == 0)
					t_spos = sendSize;

				if (sendSize > _buffer.Length - t_spos)
					sendSize = _buffer.Length - t_spos;

				// 从环形缓冲区拷贝数据
				byte[] sendBuf = new byte[sendSize];
				Buffer.BlockCopy(_buffer, _spos % _buffer.Length, sendBuf, 0, sendSize);

				try
				{
					await socket.Send(sendBuf);
				}
				catch (Exception e)
				{
					KBELog.ERROR_MSG($"PacketSenderWS::_asyncSend(): send data error, disconnect! error = '{e}'");
					Event.fireIn("_closeNetwork", new object[] { _networkInterface });

					Monitor.Exit(_sendingObj);
					return;
				}

				_spos += sendSize;

				if (_spos == _wpos)
				{
					_sending = false;
					Monitor.Exit(_sendingObj);
					return;
				}

				Monitor.Exit(_sendingObj);
			}
		}

	}
} 
