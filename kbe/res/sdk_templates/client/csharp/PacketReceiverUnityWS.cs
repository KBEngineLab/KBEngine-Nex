

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
	using NativeWebSocket;
	/*
		包接收模块(与服务端网络部分的名称对应)
		处理网络数据的接收
	*/
	public class PacketReceiverUnityWS : PacketReceiverBase
	{
		private WebSocket _webSocket;
		public PacketReceiverUnityWS(NetworkInterfaceBase networkInterface) : base(networkInterface) 
		{
			_webSocket = ((NetworkInterfaceUnityWS)_networkInterface).GetWebSocket();
		}

		~PacketReceiverUnityWS()
		{
			KBELog.DEBUG_MSG("PacketReceiverUnityWS::~PacketReceiverUnityWS(), destroyed!");
		}

		public override void process()
		{
#if UNITY_5_3_OR_NEWER && (!UNITY_WEBGL || UNITY_EDITOR)
			// 非 WebGL 平台，必须每帧调用
			_webSocket.DispatchMessageQueue();
#endif
		}

		public override void startRecv()
		{
			//var v = new AsyncReceiveMethod(this._asyncReceive);
			//v.BeginInvoke(new AsyncCallback(_onRecv), null);
		}

		protected override void _asyncReceive()
		{
		}
	}
} 
