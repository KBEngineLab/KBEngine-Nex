
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

	/// <summary>
	/// 网络模块
	/// 处理连接、收发数据
	/// </summary>
	public abstract class NetworkInterfaceBase
	{
		public const int TCP_PACKET_MAX = 1460;
		public const int UDP_PACKET_MAX = 1472;
		public const string UDP_HELLO = "62a559f3fa7748bc22f8e0766019d498";
		public const string UDP_HELLO_ACK = "1432ad7c829170a76dd31982c3501eca";

		public delegate void AsyncConnectMethod(ConnectState state);
		public delegate void ConnectCallback(string ip, int port, bool success, object userData);

		protected Socket _socket = null;
		protected PacketReceiverBase _packetReceiver = null;
		protected PacketSenderBase _packetSender = null;
		protected EncryptionFilter _filter = null;
		private readonly object _lifecycleLock = new object();
		private readonly NetworkLifecycleState _lifecycleState = new NetworkLifecycleState();

		public bool connected = false;
		
		public class ConnectState
		{
			// for connect
			public string connectIP = "";
			public int connectPort = 0;
			public ConnectCallback connectCB = null;
			public AsyncConnectMethod caller = null;
			public object userData = null;
			public Socket socket = null;
			public NativeWebSocket.WebSocket webSocket = null;
			public NetworkInterfaceBase networkInterface = null;
			public string error = "";
		}
		
		public NetworkInterfaceBase()
		{
		}

		~NetworkInterfaceBase()
		{
			KBELog.DEBUG_MSG("NetworkInterfaceBase::~NetworkInterfaceBase(), destructed!!!");
			reset();
		}

		public virtual Socket sock()
		{
			return _socket;
		}
		
		public virtual void reset()
		{
			shutdown(false, null);
		}

		public virtual void close()
		{
			shutdown(true, null);
		}

		private bool shutdown(bool notifyDisconnected, ConnectState expectedConnection)
		{
			bool shouldNotify;
			lock (_lifecycleLock)
			{
				if (expectedConnection != null && !isCurrentConnection(expectedConnection))
					return false;

				// 通知资格只在成功连接后建立，并在 reset/close 中一次性消费；socket 是否存在不能代表连接是否曾经可用。
				// Notification eligibility is armed only after a successful connection and consumed once by reset/close; socket presence does not prove that a connection was ever usable.
				shouldNotify = _lifecycleState.consume(notifyDisconnected);
				releaseTransport();
			}

			// 公开事件必须在生命周期锁外派发，业务回调才能同步发起重登录而不造成锁重入或锁顺序反转。
			// Dispatch the public event outside the lifecycle lock so application callbacks may relogin synchronously without lock reentry or lock-order inversion.
			if (shouldNotify)
				Event.fireAll(EventOutTypes.onDisconnected);

			return true;
		}

		protected virtual void releaseTransport()
		{
			// 先从接口摘除接收器再通知停止，确保被唤醒的接收线程观察到失效状态，避免把本地主动关闭误报为远端断线。
			// Detach the receiver before stopping it so the awakened receive thread observes invalid state and does not report a local shutdown as a remote disconnect.
			PacketReceiverBase packetReceiver = _packetReceiver;
			_packetReceiver = null;
			if (packetReceiver != null)
				packetReceiver.stop();

			_packetSender = null;
			_filter = null;
			connected = false;

			Socket socket = _socket;
			_socket = null;
			if(socket != null)
			{
				try
				{
					if(socket.RemoteEndPoint != null)
						KBELog.DEBUG_MSG(string.Format("NetworkInterfaceBase::releaseTransport(), close socket from '{0}'", socket.RemoteEndPoint.ToString()));
				}
				catch (Exception)
				{
					// ignored
				}
				try
				{
					socket.Shutdown(SocketShutdown.Both);
				}
				catch
				{
					// ignored
				}

				socket.Close(0);
			}
		}

		protected virtual bool isCurrentConnection(ConnectState state)
		{
			return Object.ReferenceEquals(state.networkInterface, this) && Object.ReferenceEquals(state.socket, _socket);
		}

		private bool activateConnection(ConnectState state, out PacketReceiverBase packetReceiver)
		{
			packetReceiver = null;
			lock (_lifecycleLock)
			{
				// 异步 connect 完成可能晚于 reset 或下一次连接；只有仍指向当前 transport 的回调可以激活接口。
				// Async connect completion may arrive after reset or a later connection; only a callback still referring to the current transport may activate the interface.
				if (!isCurrentConnection(state))
					return false;

				packetReceiver = createPacketReceiver();
				_packetReceiver = packetReceiver;
				connected = true;
				_lifecycleState.arm();
				return true;
			}
		}

		protected abstract PacketReceiverBase createPacketReceiver();
		protected abstract PacketSenderBase createPacketSender();
		protected abstract Socket createSocket();
		protected abstract void onAsyncConnect(ConnectState state);

		public virtual PacketReceiverBase packetReceiver()
		{
			return _packetReceiver;
		}

		public virtual PacketSenderBase PacketSender()
		{
			return _packetSender;
		}

		public virtual bool valid()
		{
			Socket socket = _socket;
			return socket != null && socket.Connected;
		}
		
		public void _onConnectionState(ConnectState state)
		{
			bool success = (state.error == "" && valid());
			bool handled;
			PacketReceiverBase packetReceiver = null;
			if (success)
			{
				handled = activateConnection(state, out packetReceiver);
				success = handled;
			}
			else
			{
				handled = shutdown(false, state);
			}

			// 连接已被 reset 或后续尝试替换时，迟到结果不能注销新事件、回调业务或改变当前状态。
			// When reset or a later attempt has replaced the connection, a late result must not deregister new events, call the application, or mutate current state.
			if (!handled)
				return;

			KBEngine.Event.deregisterIn(this);
			if (success)
			{
				KBELog.DEBUG_MSG(string.Format("NetworkInterfaceBase::_onConnectionState(), connect to {0}:{1} is success!", state.connectIP, state.connectPort));
				packetReceiver.startRecv();
			}
			else
			{
				KBELog.ERROR_MSG(string.Format("NetworkInterfaceBase::_onConnectionState(), connect error! ip: {0}:{1}, err: {2}", state.connectIP, state.connectPort, state.error));
			}

			Event.fireAll(EventOutTypes.onConnectionState, success);

			if (state.connectCB != null)
				state.connectCB(state.connectIP, state.connectPort, success, state.userData);
		}

		private static void connectCB(IAsyncResult ar)
		{
			ConnectState state = null;
			
			try 
			{
				// Retrieve the socket from the state object.
				state = (ConnectState) ar.AsyncState;

				// Complete the connection.
				state.socket.EndConnect(ar);

				Event.fireIn("_onConnectionState", new object[] { state });
			} 
			catch (Exception e) 
			{
				state.error = e.ToString();
				Event.fireIn("_onConnectionState", new object[] { state });
			}
		}

		/// <summary>
		/// 在非主线程执行：连接服务器
		/// </summary>
		private void _asyncConnect(ConnectState state)
		{
			KBELog.DEBUG_MSG(string.Format("NetworkInterfaceBase::_asyncConnect(), will connect to '{0}:{1}' ...", state.connectIP, state.connectPort));
			onAsyncConnect(state);
		}

		protected virtual void onAsyncConnectCB(ConnectState state)
		{

		}

		/// <summary>
		/// 在非主线程执行：连接服务器结果回调
		/// </summary>
		[Obsolete]
		private void _asyncConnectCB(IAsyncResult ar)
		{
			ConnectState state = (ConnectState)ar.AsyncState;
			
			onAsyncConnectCB(state);

			KBELog.DEBUG_MSG(string.Format("NetworkInterfaceBase::_asyncConnectCB(), connect to '{0}:{1}' finish. error = '{2}'", state.connectIP, state.connectPort, state.error));

			// Call EndInvoke to retrieve the results.
			state.caller.EndInvoke(ar);
			Event.fireIn("_onConnectionState", new object[] { state });
		}

		public virtual void connectTo(string ip, int port, ConnectCallback callback, object userData, Dictionary<string, string> domainMapping, Dictionary<int, int> portMapping)
		{
			if (valid())
				throw new InvalidOperationException("Have already connected!");

			if (!(new Regex(@"((?:(?:25[0-5]|2[0-4]\d|((1\d{2})|([1-9]?\d)))\.){3}(?:25[0-5]|2[0-4]\d|((1\d{2})|([1-9]?\d))))")).IsMatch(ip))
			{
				IPHostEntry ipHost = Dns.GetHostEntry(ip);
				ip = ipHost.AddressList[0].ToString();
			}

			_socket = createSocket();
			
			

			// AsyncConnectMethod asyncConnectMethod = new AsyncConnectMethod(this._asyncConnect);

			ConnectState state = new ConnectState();
			state.connectIP = ip;
			state.connectPort = port;
			state.connectCB = callback;
			state.userData = userData;
			state.socket = _socket;
			state.networkInterface = this;
			// state.caller = asyncConnectMethod;

			KBELog.DEBUG_MSG("connect to " + ip + ":" + port + " ...");
			connected = false;
			
			// 先注册一个事件回调，该事件在当前线程触发
			Event.registerIn("_onConnectionState", this, "_onConnectionState");

			// asyncConnectMethod.BeginInvoke(state, new AsyncCallback(this._asyncConnectCB), state);
			_ = ConnectAsync(state);
		}
		
		
		protected virtual async Task ConnectAsync(ConnectState state)
		{
			try
			{
				// 在线程池线程执行连接操作
				await Task.Run(() => _asyncConnect(state));

				// 这里直接调用回调函数
				onAsyncConnectCB(state);

				KBELog.DEBUG_MSG($"NetworkInterfaceBase::ConnectAsync(), connect to '{state.connectIP}:{state.connectPort}' finished. error = '{state.error}'");

				Event.fireIn("_onConnectionState", new object[] { state });
			}
			catch (Exception ex)
			{
				KBELog.ERROR_MSG($"NetworkInterfaceBase::ConnectAsync() error: {ex}");
				// 所有异步失败都必须回到主线程完成连接状态回调，否则登录状态机会永久等待。
				// Every asynchronous failure must return to the main thread connection callback or the login state machine waits forever.
				state.error = ex.ToString();
				Event.fireIn("_onConnectionState", new object[] { state });
			}
		}

		public virtual bool send(MemoryStream stream)
		{
			if (!valid())
			{
				throw new ArgumentException("invalid socket!");
			}

			if (_packetSender == null)
				_packetSender = createPacketSender();

			if (_filter != null)
				return _filter.send(_packetSender, stream);

			return _packetSender.send(stream);
		}

		public virtual bool send(IReadOnlyList<MemoryStream> streams)
		{
			if (!valid())
				return false;

			if (_packetSender == null)
				_packetSender = createPacketSender();

			if (_filter != null)
				return _filter.send(_packetSender, streams);

			return _packetSender.send(streams);
		}

		public virtual void process()
		{
			if (!valid())
				return;

			if (_packetReceiver != null)
				_packetReceiver.process();
		}


		public EncryptionFilter fileter()
		{
			return _filter;
		}

		public void setFilter(EncryptionFilter filter)
		{
			_filter = filter;
		}
	}
}
