

namespace KBEngine
{
	 
	using System; 
	using System.Collections; 
	using System.Collections.Generic;
	using System.Text;
	using System.Threading;
	using System.Text.RegularExpressions;
	
	using MessageID = System.UInt16;
	using MessageLength = System.UInt16;
	
	/*
		这是KBEngine插件的核心模块
		包括网络创建、持久化协议、entities的管理、以及引起对外可调用接口。
		
		一些可以参考的地方:
		http://www.kbengine.org/docs/programming/clientsdkprogramming.html
		http://www.kbengine.org/docs/programming/kbe_message_format.html
		
		http://www.kbengine.org/cn/docs/programming/clientsdkprogramming.html
		http://www.kbengine.org/cn/docs/programming/kbe_message_format.html
	*/
	public class KBEngineApp
	{
		public static KBEngineApp app = null;
		private NetworkSession _networkSession = null;
		
		KBEngineArgs _args = null;
		
		// 客户端的类别
		// http://www.kbengine.org/docs/programming/clientsdkprogramming.html
		// http://www.kbengine.org/cn/docs/programming/clientsdkprogramming.html
		public enum CLIENT_TYPE
		{
			// Mobile(Phone, Pad)
			CLIENT_TYPE_MOBILE				= 1,

			// Windows Application program
			CLIENT_TYPE_WIN					= 2,

			// Linux Application program
			CLIENT_TYPE_LINUX				= 3,
				
			// Mac Application program
			CLIENT_TYPE_MAC					= 4,
				
			// Web，HTML5，Flash
			CLIENT_TYPE_BROWSER				= 5,

			// bots
			CLIENT_TYPE_BOTS				= 6,

			// Mini-Client
			CLIENT_TYPE_MINI				= 7,
		};

		//加密通信类型
		public enum NETWORK_ENCRYPT_TYPE
		{
			//无加密
			ENCRYPT_TYPE_NONE = 0,

			//Blowfish
			ENCRYPT_TYPE_BLOWFISH = 1,
		};


		public enum NETWORK_TYPE
		{
			TCP = 0,
			KCP = 1,
			CUSTOM = 2,
			CUSTOM_ALL = 3,
		}


		public string username = "kbengine";
		public string password = "123456";
		
		// 服务端分配的baseapp地址
		public string baseappIP = "";
		public UInt16 baseappTcpPort = 0;
		public UInt16 baseappUdpPort = 0;

		// 当前状态
		public string currserver = "";
		public string currstate = "";
		
		// 服务端下行以及客户端上行用于登录时处理的账号绑定的二进制信息
		// 该信息由用户自己进行扩展
		private byte[] _serverdatas = new byte[0];
		private byte[] _clientdatas = new byte[0];
		
		// 通信协议加密，blowfish协议
		private byte[] _encryptedKey = new byte[0];
		
		// 服务端与客户端的版本号以及协议MD5
		public string serverVersion = "";
		public string clientVersion = "@{KBE_VERSION}";
		public string serverScriptVersion = "";
		public string clientScriptVersion = "@{KBE_SCRIPT_VERSION}";
		public string serverProtocolMD5 = "@{KBE_SERVER_PROTO_MD5}";
		public string serverEntitydefMD5 = "@{KBE_SERVER_ENTITYDEF_MD5}";
		
		// 当前玩家的实体id与实体类别
		public UInt64 entity_uuid = 0;
		public Int32 entity_id = 0;
		public string entity_type = "";

		private List<Entity> _controlledEntities = new List<Entity>();
		
		// 当前服务端最后一次同步过来的玩家位置
		private KBVector3 _entityServerPos = new KBVector3(0f, 0f, 0f);
		
		// space的数据，具体看API手册关于spaceData
		// https://github.com/kbengine/kbengine/tree/master/docs/api
		private Dictionary<string, string> _spacedatas = new Dictionary<string, string>();
		
		// 所有实体都保存于这里， 请参看API手册关于entities部分
		// https://github.com/kbengine/kbengine/tree/master/docs/api
		public Dictionary<Int32, Entity> entities = new Dictionary<Int32, Entity>();
		
		// 在玩家View范围小于256个实体时我们可以通过一字节索引来找到entity
		private List<Int32> _entityIDAliasIDList = new List<Int32>();
		private Dictionary<Int32, MemoryStream> _bufferedCreateEntityMessages = new Dictionary<Int32, MemoryStream>(); 
		
		// 所有服务端错误码对应的错误描述
		private ServerErrorDescrs _serverErrs = new ServerErrorDescrs(); 
		
		private readonly HeartbeatState _heartbeatState = new HeartbeatState();
		private System.DateTime _lastUpdateToServerTime = System.DateTime.Now;
		
		//上传玩家信息到服务器间隔，单位毫秒
		private float _updatePlayerToServerPeroid = 100.0f;
		private const int _1MS_TO_100NS = 10000;

		//加密过滤器
		private INetworkFrameCodec _filter = null;

		// 玩家当前所在空间的id， 以及空间对应的资源
		public UInt32 spaceID = 0;
		public string spaceResPath = "";
		public bool isLoadedGeometry = false;
		
		// 按照标准，每个客户端部分都应该包含这个属性
		public const string component = "client"; 
		
		public KBEngineApp(KBEngineArgs args)
		{
			if (args == null)
				throw new ArgumentNullException(nameof(args));
			if (app != null)
				throw new Exception("Only one instance of KBEngineApp!");
			
			app = this;
			try
			{
				Event.outEventsImmediately = !args.isMultiThreads;
				initialize(args);
			}
			catch
			{
				// 初始化失败不能留下一个不可用的全局实例，否则修正参数后也无法重新创建 SDK。
				// Failed initialization must not leave a poisoned singleton that prevents retrying with corrected arguments.
				app = null;
				throw;
			}
		}

		public static KBEngineApp getSingleton() 
		{
			if(KBEngineApp.app == null)
			{
				throw new Exception("Please create KBEngineApp!");
			}

			return KBEngineApp.app;
		}

		public virtual bool initialize(KBEngineArgs args)
		{
			if (args == null)
				throw new ArgumentNullException(nameof(args));
			if ((args.networkType == NETWORK_TYPE.CUSTOM || args.networkType == NETWORK_TYPE.CUSTOM_ALL) &&
				args.customNetworkProviderFactory == null)
			{
				throw new InvalidOperationException("CUSTOM network mode requires customNetworkProviderFactory.");
			}

			_args = args;
			_updatePlayerToServerPeroid = (float)_args.syncPlayerMS;

			EntityDef.init();

			initNetwork();

			// 注册事件
			installEvents();

			return true;
		}
		
		void initNetwork()
		{
			_filter = null;
			Messages.init();
		}

		private void connectSession(
			NetworkConnectionRole role,
			NetworkEndpoint endpoint,
			Action<string, int, bool, object> callback)
		{
			int port = _args.networkType == NETWORK_TYPE.KCP && role == NetworkConnectionRole.Baseapp
				? endpoint.UdpPort
				: endpoint.TcpPort;

			if (_networkSession != null)
			{
				_networkSession.Reset();
				_networkSession = null;
			}

			try
			{
				NetworkProviderContext context = new NetworkProviderContext(role, endpoint, _args, serverVersion);
				int maximumMessageSize = _args.getMessageMaxSize();
				if (maximumMessageSize <= 0)
					throw new InvalidOperationException("MESSAGE_MAX must be greater than zero.");

				INetworkProvider provider = createNetworkProvider(context);
				NetworkSession session = new NetworkSession(
					role,
					endpoint,
					provider,
					maximumMessageSize,
					onNetworkSessionClosed);
				_networkSession = session;

				session.Connect((success, closeInfo) =>
				{
					if (!Object.ReferenceEquals(session, _networkSession))
						return;

					if (!success && closeInfo != null)
						KBELog.ERROR_MSG("KBEngine::connectSession(): " + closeInfo.Message +
							(closeInfo.Exception == null ? string.Empty : " " + closeInfo.Exception));

					Event.fireAll(EventOutTypes.onConnectionState, success);
					callback?.Invoke(endpoint.Host, port, success, null);
				});
			}
			catch (Exception exception)
			{
				KBELog.ERROR_MSG("KBEngine::connectSession(): provider creation failed: " + exception);
				Event.fireAll(EventOutTypes.onConnectionState, false);
				callback?.Invoke(endpoint.Host, port, false, null);
			}
		}

		private INetworkProvider createNetworkProvider(NetworkProviderContext context)
		{
			switch (_args.networkType)
			{
				case NETWORK_TYPE.TCP:
					return new TcpNetworkProvider(context);

				case NETWORK_TYPE.KCP:
					if (context.Role == NetworkConnectionRole.Loginapp)
						return new TcpNetworkProvider(context);
					if (context.Endpoint.UdpPort == 0)
						throw new InvalidOperationException("KCP baseapp connection requires a non-zero UDP port.");
					return new KcpNetworkProvider(context);

				case NETWORK_TYPE.CUSTOM:
					if (context.Role == NetworkConnectionRole.Loginapp)
						return new TcpNetworkProvider(context);
					return createCustomNetworkProvider(context);

				case NETWORK_TYPE.CUSTOM_ALL:
					return createCustomNetworkProvider(context);

				default:
					throw new InvalidOperationException("Unsupported network type: " + _args.networkType);
			}
		}

		private INetworkProvider createCustomNetworkProvider(NetworkProviderContext context)
		{
			if (_args.customNetworkProviderFactory == null)
				throw new InvalidOperationException("CUSTOM network mode requires customNetworkProviderFactory.");

			INetworkProvider provider = _args.customNetworkProviderFactory.Create(context);
			return provider ?? throw new InvalidOperationException("The custom network provider factory returned null.");
		}

		private void onNetworkSessionClosed(NetworkSession session, NetworkCloseInfo closeInfo)
		{
			if (closeInfo != null)
				KBELog.ERROR_MSG("KBEngine::onNetworkSessionClosed(): " + closeInfo.Message +
					(closeInfo.Exception == null ? string.Empty : " " + closeInfo.Exception));

			Event.fireIn("_closeNetwork", new object[] { session });
		}
		
		void installEvents()
		{
			Event.registerIn(EventInTypes.createAccount, this, "createAccount");
			Event.registerIn(EventInTypes.login, this, "login");
			Event.registerIn(EventInTypes.logout, this, "logout");
			Event.registerIn(EventInTypes.reloginBaseapp, this, "reloginBaseapp");
			Event.registerIn(EventInTypes.resetPassword, this, "resetPassword");
			Event.registerIn(EventInTypes.bindAccountEmail, this, "bindAccountEmail");
			Event.registerIn(EventInTypes.newPassword, this, "newPassword");
			
			// 内部事件
			Event.registerIn("_closeNetwork", this, "_closeNetwork");
		}

		public KBEngineArgs getInitArgs()
		{
			return _args;
		}
		
		public virtual void destroy()
		{
			KBELog.WARNING_MSG("KBEngine::destroy()");
			
			if(currserver == "baseapp")
				logout();
			
			reset();
			KBEngine.Event.deregisterIn(this);
			resetMessages();
			
			KBEngineApp.app = null;
		}
		
		public NetworkSession networkSession()
		{
			return _networkSession;
		}
		
		public byte[] serverdatas()
		{
			return _serverdatas;
		}
		
		public void entityServerPos(KBVector3 pos)
		{
			_entityServerPos = pos;
		}
		
		public void resetMessages()
		{
			_serverErrs.Clear();
			Messages.clear();
			EntityDef.reset();
			
			Entity.clear();
			KBELog.DEBUG_MSG("KBEngine::resetMessages()");
		}
		
		public virtual void reset()
		{
			KBEngine.Event.clearFiredEvents();
			
			clearEntities(true);
			
			currserver = "";
			currstate = "";
			_serverdatas = new byte[0];
			serverVersion = "";
			serverScriptVersion = "";
			
			entity_uuid = 0;
			entity_id = 0;
			entity_type = "";
			
			_entityIDAliasIDList.Clear();
			_bufferedCreateEntityMessages.Clear();
			
			resetHeartbeatState();
			_lastUpdateToServerTime = System.DateTime.Now;
			
			spaceID = 0;
			spaceResPath = "";
			isLoadedGeometry = false;
			
			if (_networkSession != null)
				_networkSession.Reset();

			_networkSession = null;
			_filter = null;
			
			_spacedatas.Clear();
		}
		
		public static bool validEmail(string strEmail) 
		{ 
			return Regex.IsMatch(strEmail, @"^([\w-\.]+)@((\[[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.)
				|(([\w-]+\.)+))([a-zA-Z]{2,4}|[0-9]{1,3})(\]?)$"); 
		}  
		
		/*
			插件的主循环处理函数
		*/
		public virtual void process()
		{
			// 处理网络
			if (_networkSession != null)
				_networkSession.Process();
			
			// 处理外层抛入的事件
			Event.processInEvents();
			
			// 向服务端发送心跳以及同步角色信息到服务端
			sendTick();
		}
		
		/*
			当前玩家entity
		*/
		public Entity player()
		{
			Entity e;
			if(entities.TryGetValue(entity_id, out e))
				return e;
			
			return null;
		}

		public void _closeNetwork(NetworkSession networkSession)
		{
			// 迟到的旧 Session 事件只能静默释放，不能关闭后来建立的新连接。
			// A late event from an old session may only release that session; it cannot close a newer connection.
			if (!Object.ReferenceEquals(networkSession, _networkSession))
			{
				networkSession.Reset();
				return;
			}

			networkSession.Close();
		}
		
		/*
			向服务端发送心跳以及同步角色信息到服务端
		*/
		public void sendTick()
		{
			if(_networkSession == null || !_networkSession.Connected)
				return;

			long now = HeartbeatState.timestamp();
			
			// 更新玩家的位置与朝向到服务端
			updatePlayerToServer();
			
			if(_args.serverHeartbeatTick > 0 && _heartbeatState.isDue(now, _args.serverHeartbeatTick))
			{
				// 显式 pending 状态避免使用墙钟时间隐式推断响应；系统时间跳变和超过 59 秒的间隔都不会改变超时语义。
				// Explicit pending state avoids inferring replies from wall-clock timestamps, so clock changes and intervals above 59 seconds cannot alter timeout semantics.
				if(_heartbeatState.replyPending)
				{
					KBELog.ERROR_MSG("sendTick: Receive appTick timeout!");
					_networkSession.Close();
					return;
				}

				Message Loginapp_onClientActiveTickMsg = null;
				Message Baseapp_onClientActiveTickMsg = null;
				bool heartbeatSent = false;
				
				Messages.messages.TryGetValue("Loginapp_onClientActiveTick", out Loginapp_onClientActiveTickMsg);
				Messages.messages.TryGetValue("Baseapp_onClientActiveTick", out Baseapp_onClientActiveTickMsg);

				if(currserver == "loginapp")
				{
					if(Loginapp_onClientActiveTickMsg != null)
					{
						Bundle bundle = Bundle.createObject();
						bundle.newMessage(Messages.messages["Loginapp_onClientActiveTick"]);
						heartbeatSent = bundle.send(_networkSession);
					}
				}
				else
				{
					if(Baseapp_onClientActiveTickMsg != null)
					{
						Bundle bundle = Bundle.createObject();
						bundle.newMessage(Messages.messages["Baseapp_onClientActiveTick"]);
						heartbeatSent = bundle.send(_networkSession);
					}
				}
				
				_heartbeatState.markAttempt(now, heartbeatSent);
			}
		}

		private void resetHeartbeatState()
		{
			// 连接切换和协议握手会重建网络接口；在新链路上不能继承旧链路尚未完成的心跳请求。
			// Connection transitions and protocol handshakes rebuild the network interface, so a new link must not inherit an outstanding heartbeat from the old link.
			_heartbeatState.reset(HeartbeatState.timestamp());
		}

		/*
			服务器心跳回调
		*/
		public void Client_onAppActiveTickCB()
		{
			_heartbeatState.markReply();
		}

		/*
			与服务端握手，与任何一个进程连接之后应该第一时间进行握手
		*/
		public void hello()
		{
			Bundle bundle = Bundle.createObject();
			if(currserver == "loginapp")
				bundle.newMessage(Messages.messages["Loginapp_hello"]);
			else
				bundle.newMessage(Messages.messages["Baseapp_hello"]);

			_filter = null;

			if (_args.networkEncryptType == NETWORK_ENCRYPT_TYPE.ENCRYPT_TYPE_BLOWFISH)
			{
				_filter = new BlowfishFrameCodec();
				_encryptedKey = ((BlowfishFrameCodec)_filter).Key;
				_networkSession.SetFrameCodec(null);
			}

			bundle.writeString(clientVersion);
			bundle.writeString(clientScriptVersion);
			bundle.writeBlob(_encryptedKey);
			bundle.send(_networkSession);
		}

		/*
			握手之后服务端的回调
		*/
		public void Client_onHelloCB(MemoryStream stream)
		{
			string str_serverVersion = stream.readString();
			serverScriptVersion = stream.readString();
			string currentServerProtocolMD5 = stream.readString();
			string currentServerEntitydefMD5 = stream.readString();
			Int32 ctype = stream.readInt32();
			
			KBELog.DEBUG_MSG("KBEngine::Client_onHelloCB: verInfo(" + str_serverVersion 
				+ "), scriptVersion("+ serverScriptVersion + "), srvProtocolMD5("+ serverProtocolMD5 
				+ "), srvEntitydefMD5("+ serverEntitydefMD5 + "), + ctype(" + ctype + ")!");
			
			if(str_serverVersion != "Getting")
			{
				serverVersion = str_serverVersion;

				/* 
				if(serverProtocolMD5 != currentServerProtocolMD5)
				{
					KBELog.ERROR_MSG("Client_onHelloCB: digest not match! serverProtocolMD5=" + serverProtocolMD5 + "(server: " + currentServerProtocolMD5 + ")");
					Event.fireAll(EventOutTypes.onVersionNotMatch, clientVersion, serverVersion);
					return;
				}
				*/
				
				if (serverEntitydefMD5 != currentServerEntitydefMD5)
				{
					KBELog.ERROR_MSG("Client_onHelloCB: digest not match! serverEntitydefMD5=" + serverEntitydefMD5 + "(server: " + currentServerEntitydefMD5 + ")");
					Event.fireAll(EventOutTypes.onVersionNotMatch, clientVersion, serverVersion);
					return;
				}
			}

			if (_args.networkEncryptType == NETWORK_ENCRYPT_TYPE.ENCRYPT_TYPE_BLOWFISH)
			{
				_networkSession.SetFrameCodec(_filter);
				_filter = null;
			}

			onServerDigest();
			
			if(currserver == "baseapp")
			{
				onLogin_baseapp();
			}
			else
			{
				onLogin_loginapp();
			}
		}
		
		/*
			服务端错误描述导入了
		*/
		public void Client_onImportServerErrorsDescr(MemoryStream stream)
		{
			// 无需实现，已由插件生成静态代码
		}

		/*
			从服务端返回的二进制流导入客户端消息协议
		*/
		public void Client_onImportClientMessages(MemoryStream stream)
		{
			// 无需实现，已由插件生成静态代码
		}

		/*
			从服务端返回的二进制流导入客户端消息协议
		*/
		public void Client_onImportClientEntityDef(MemoryStream stream)
		{
			// 无需实现，已由插件生成静态代码
		}

		/*
			引擎版本不匹配
		*/
		public void Client_onVersionNotMatch(MemoryStream stream)
		{
			serverVersion = stream.readString();
			
			KBELog.ERROR_MSG("Client_onVersionNotMatch: verInfo=" + clientVersion + "(server: " + serverVersion + ")");
			Event.fireAll(EventOutTypes.onVersionNotMatch, clientVersion, serverVersion);
		}

		/*
			脚本版本不匹配
		*/
		public void Client_onScriptVersionNotMatch(MemoryStream stream)
		{
			serverScriptVersion = stream.readString();
			
			KBELog.ERROR_MSG("Client_onScriptVersionNotMatch: verInfo=" + clientScriptVersion + "(server: " + serverScriptVersion + ")");
			Event.fireAll(EventOutTypes.onScriptVersionNotMatch, clientScriptVersion, serverScriptVersion);
		}
		
		/*
			被服务端踢出
		*/
		public void Client_onKicked(UInt16 failedcode)
		{
			KBELog.DEBUG_MSG("Client_onKicked: failedcode=" + failedcode + "(" + serverErr(failedcode) + ")");
			Event.fireAll(EventOutTypes.onKicked, failedcode);
		}
		
		/*
			登录到服务端，必须登录完成loginapp与网关(baseapp)，登录流程才算完毕
		*/
		public void login(string username, string password, byte[] datas)
		{
			KBEngineApp.app.username = username;
			KBEngineApp.app.password = password;
			KBEngineApp.app._clientdatas = datas;
			
			KBEngineApp.app.login_loginapp(true);
		}
		
		/*
			登录到服务端(loginapp), 登录成功后还必须登录到网关(baseapp)登录流程才算完毕
		*/
		public void login_loginapp(bool noconnect)
		{
			if(noconnect)
			{
				reset();
				connectSession(
					NetworkConnectionRole.Loginapp,
					new NetworkEndpoint(_args.ip, _args.port, 0),
					onConnectTo_loginapp_callback);
			}
			else
			{
				KBELog.DEBUG_MSG("KBEngine::login_loginapp(): send login! username=" + username);
				Bundle bundle = Bundle.createObject();
				bundle.newMessage(Messages.messages["Loginapp_login"]);
				bundle.writeInt8((sbyte)_args.clientType);
				bundle.writeBlob(KBEngineApp.app._clientdatas);
				bundle.writeString(username);
				bundle.writeString(password);
				bundle.send(_networkSession);
			}
		}
		
		private void onConnectTo_loginapp_callback(string ip, int port, bool success, object userData)
		{
			resetHeartbeatState();
			
			if(!success)
			{
				KBELog.ERROR_MSG(string.Format("KBEngine::login_loginapp(): connect {0}:{1} error!", ip, port));  
				return;
			}
			
			currserver = "loginapp";
			currstate = "login";
			
			KBELog.DEBUG_MSG(string.Format("KBEngine::login_loginapp(): connect {0}:{1} success!", ip, port));

			hello();
		}
		
		private void onLogin_loginapp()
		{
			resetHeartbeatState();
			login_loginapp(false);
		}
		
		/*
			登录到服务端，登录到网关(baseapp)
		*/
		public void login_baseapp(bool noconnect)
		{  
			if(noconnect)
			{
				Event.fireOut(EventOutTypes.onLoginBaseapp);
				connectSession(
					NetworkConnectionRole.Baseapp,
					new NetworkEndpoint(baseappIP, baseappTcpPort, baseappUdpPort),
					onConnectTo_baseapp_callback);
			}
			else
			{
				Bundle bundle = Bundle.createObject();
				bundle.newMessage(Messages.messages["Baseapp_loginBaseapp"]);
				bundle.writeString(username);
				bundle.writeString(password);
				bundle.send(_networkSession);
			}
		}

		private void onConnectTo_baseapp_callback(string ip, int port, bool success, object userData)
		{
			resetHeartbeatState();
			
			if(!success)
			{
				KBELog.ERROR_MSG(string.Format("KBEngine::login_baseapp(): connect {0}:{1} error!", ip, port));
				return;
			}
			
			currserver = "baseapp";
			currstate = "";
			
			KBELog.DEBUG_MSG(string.Format("KBEngine::login_baseapp(): connect {0}:{1} success!", ip, port));

			hello();
		}
		
		private void onLogin_baseapp()
		{
			resetHeartbeatState();
			login_baseapp(false);
		}
		
		/*
			重登录到网关(baseapp)
			一些移动类应用容易掉线，可以使用该功能快速的重新与服务端建立通信
		*/
		public void reloginBaseapp()
		{
			resetHeartbeatState();

			if(_networkSession != null && _networkSession.Connected)
				return;

			Event.fireAll(EventOutTypes.onReloginBaseapp);

			connectSession(
				NetworkConnectionRole.Baseapp,
				new NetworkEndpoint(baseappIP, baseappTcpPort, baseappUdpPort),
				onReConnectTo_baseapp_callback);
		}

		private void onReConnectTo_baseapp_callback(string ip, int port, bool success, object userData)
		{
			if(!success)
			{
				KBELog.ERROR_MSG(string.Format("KBEngine::reloginBaseapp(): connect {0}:{1} error!", ip, port));
				return;
			}
			
			KBELog.DEBUG_MSG(string.Format("KBEngine::relogin_baseapp(): connect {0}:{1} success!", ip, port));

			Bundle bundle = Bundle.createObject();
			bundle.newMessage(Messages.messages["Baseapp_reloginBaseapp"]);
			bundle.writeString(username);
			bundle.writeString(password);
			bundle.writeUint64(entity_uuid);
			bundle.writeInt32(entity_id);
			bundle.send(_networkSession);
			
			resetHeartbeatState();
		}

		/*
			登出baseapp
		*/
		public void logout()
		{
			Bundle bundle = Bundle.createObject();
			bundle.newMessage(Messages.messages["Baseapp_logoutBaseapp"]);
			bundle.writeUint64(entity_uuid);
			bundle.writeInt32(entity_id);
			bundle.send(_networkSession);
		}

		/*
			通过错误id得到错误描述
		*/
		public string serverErr(UInt16 id)
		{
			return _serverErrs.serverErrStr(id);
		}
		
		public void onOpenLoginapp_resetpassword()
		{  
			KBELog.DEBUG_MSG("KBEngine::onOpenLoginapp_resetpassword: successfully!");
			currserver = "loginapp";
			currstate = "resetpassword";
			resetHeartbeatState();

			resetpassword_loginapp(false);
		}

		/*
			重置密码, 通过loginapp
		*/
		public void resetPassword(string username)
		{
			KBEngineApp.app.username = username;
			resetpassword_loginapp(true);
		}
		
		/*
			重置密码, 通过loginapp
		*/
		public void resetpassword_loginapp(bool noconnect)
		{
			if(noconnect)
			{
				reset();
				connectSession(
					NetworkConnectionRole.Loginapp,
					new NetworkEndpoint(_args.ip, _args.port, 0),
					onConnectTo_resetpassword_callback);
			}
			else
			{
				Bundle bundle = Bundle.createObject();
				bundle.newMessage(Messages.messages["Loginapp_reqAccountResetPassword"]);
				bundle.writeString(username);
				bundle.send(_networkSession);
			}
		}

		private void onConnectTo_resetpassword_callback(string ip, int port, bool success, object userData)
		{
			resetHeartbeatState();
			
			if(!success)
			{
				KBELog.ERROR_MSG(string.Format("KBEngine::resetpassword_loginapp(): connect {0}:{1} error!", ip, port));
				return;
			}
			
			KBELog.DEBUG_MSG(string.Format("KBEngine::resetpassword_loginapp(): connect {0}:{1} success!", ip, port)); 
			onOpenLoginapp_resetpassword();
		}
		
		public void Client_onReqAccountResetPasswordCB(UInt16 failcode)
		{
			Event.fireOut(EventOutTypes.onResetPassword, failcode);
			
			if(failcode != 0)
			{
				KBELog.ERROR_MSG("KBEngine::Client_onReqAccountResetPasswordCB: " + username + " failed! code=" + failcode + "(" + serverErr(failcode) + ")!");
				return;
			}
	
			KBELog.DEBUG_MSG("KBEngine::Client_onReqAccountResetPasswordCB: " + username + " success!");
		}
		
		/*
			绑定Email，通过baseapp
		*/
		public void bindAccountEmail(string emailAddress)
		{
			Bundle bundle = Bundle.createObject();
			bundle.newMessage(Messages.messages["Baseapp_reqAccountBindEmail"]);
			bundle.writeInt32(entity_id);
			bundle.writeString(password);
			bundle.writeString(emailAddress);
			bundle.send(_networkSession);
		}

		public void Client_onReqAccountBindEmailCB(UInt16 failcode)
		{
			Event.fireOut(EventOutTypes.onBindAccountEmail, failcode);

			if(failcode != 0)
			{
				KBELog.ERROR_MSG("KBEngine::Client_onReqAccountBindEmailCB: " + username + " failed! code=" + failcode + "(" + serverErr(failcode) + ")!");
				return;
			}

			KBELog.DEBUG_MSG("KBEngine::Client_onReqAccountBindEmailCB: " + username + " success!");
		}
		
		/*
			设置新密码，通过baseapp， 必须玩家登录在线操作所以是baseapp。
		*/
		public void newPassword(string old_password, string new_password)
		{
			Bundle bundle = Bundle.createObject();
			bundle.newMessage(Messages.messages["Baseapp_reqAccountNewPassword"]);
			bundle.writeInt32(entity_id);
			bundle.writeString(old_password);
			bundle.writeString(new_password);
			bundle.send(_networkSession);
		}

		public void Client_onReqAccountNewPasswordCB(UInt16 failcode)
		{
			Event.fireOut(EventOutTypes.onNewPassword, failcode);

			if(failcode != 0)
			{
				KBELog.ERROR_MSG("KBEngine::Client_onReqAccountNewPasswordCB: " + username + " failed! code=" + failcode + "(" + serverErr(failcode) + ")!");
				return;
			}
	
			KBELog.DEBUG_MSG("KBEngine::Client_onReqAccountNewPasswordCB: " + username + " success!");
		}

		public void createAccount(string username, string password, byte[] datas)
		{
			KBEngineApp.app.username = username;
			KBEngineApp.app.password = password;
			KBEngineApp.app._clientdatas = datas;
			
			KBEngineApp.app.createAccount_loginapp(true);
		}

		/*
			创建账号，通过loginapp
		*/
		public void createAccount_loginapp(bool noconnect)
		{
			if(noconnect)
			{
				reset();
				connectSession(
					NetworkConnectionRole.Loginapp,
					new NetworkEndpoint(_args.ip, _args.port, 0),
					onConnectTo_createAccount_callback);
			}
			else
			{
				Bundle bundle = Bundle.createObject();
				bundle.newMessage(Messages.messages["Loginapp_reqCreateAccount"]);
				bundle.writeString(username);
				bundle.writeString(password);
				bundle.writeBlob(KBEngineApp.app._clientdatas);
				bundle.send(_networkSession);
			}
		}

		public void onOpenLoginapp_createAccount()
		{  
			KBELog.DEBUG_MSG("KBEngine::onOpenLoginapp_createAccount: successfully!");
			currserver = "loginapp";
			currstate = "createAccount";
			resetHeartbeatState();
			
			createAccount_loginapp(false);
		}
		
		private void onConnectTo_createAccount_callback(string ip, int port, bool success, object userData)
		{
			resetHeartbeatState();
			
			if(!success)
			{
				KBELog.ERROR_MSG(string.Format("KBEngine::createAccount_loginapp(): connect {0}:{1} error!", ip, port));
				return;
			}
			
			KBELog.DEBUG_MSG(string.Format("KBEngine::createAccount_loginapp(): connect {0}:{1} success!", ip, port)); 
			onOpenLoginapp_createAccount();
		}
		
		/*
			获得了服务端摘要信息， 摘要包括协议MD5， entitydefMD5
		*/
		public void onServerDigest()
		{
		}

		/*
			登录loginapp失败了
		*/
		public void Client_onLoginFailed(MemoryStream stream)
		{
			UInt16 failedcode = stream.readUint16();
			_serverdatas = stream.readBlob();
			KBELog.ERROR_MSG("KBEngine::Client_onLoginFailed: failedcode(" + failedcode + ":" + serverErr(failedcode) + "), datas(" + _serverdatas.Length + ")!");
			Event.fireAll(EventOutTypes.onLoginFailed, failedcode, _serverdatas);
		}
		
		/*
			登录loginapp成功了
		*/
		public void Client_onLoginSuccessfully(MemoryStream stream)
		{
			var accountName = stream.readString();
			username = accountName;
			baseappIP = stream.readString();
			baseappTcpPort = stream.readUint16();
			baseappUdpPort = stream.readUint16();
			_serverdatas = stream.readBlob();

			KBELog.DEBUG_MSG("KBEngine::Client_onLoginSuccessfully: accountName(" + accountName + "), addr(" + 
					baseappIP + ":" + baseappTcpPort + "|" + baseappUdpPort + "), datas(" + _serverdatas.Length + ")!");

			login_baseapp(true);
		}
		
		/*
			登录baseapp失败了
		*/
		public void Client_onLoginBaseappFailed(UInt16 failedcode)
		{
			KBELog.ERROR_MSG("KBEngine::Client_onLoginBaseappFailed: failedcode=" + failedcode + "("+ serverErr(failedcode) + ")!");
			Event.fireAll(EventOutTypes.onLoginBaseappFailed, failedcode);
		}

		/*
			重登录baseapp失败了
		*/
		public void Client_onReloginBaseappFailed(UInt16 failedcode)
		{
			KBELog.ERROR_MSG("KBEngine::Client_onReloginBaseappFailed: failedcode=" + failedcode + "(" + serverErr(failedcode) + ")!");
			Event.fireAll(EventOutTypes.onReloginBaseappFailed, failedcode);
		}
		
		/*
			登录baseapp成功了
		*/
		public void Client_onReloginBaseappSuccessfully(MemoryStream stream)
		{
			entity_uuid = stream.readUint64();
			KBELog.DEBUG_MSG("KBEngine::Client_onReloginBaseappSuccessfully: name(" + username + ")!");
			Event.fireAll(EventOutTypes.onReloginBaseappSuccessfully);
		}

		/*
			服务端通知创建一个角色
		*/
		public void Client_onCreatedProxies(UInt64 rndUUID, Int32 eid, string entityType)
		{
			KBELog.DEBUG_MSG("KBEngine::Client_onCreatedProxies: eid(" + eid + "), entityType(" + entityType + ")!");
			
			entity_uuid = rndUUID;
			entity_id = eid;
			entity_type = entityType;
			
			if(!this.entities.ContainsKey(eid))
			{
				ScriptModule module = null;
				if(!EntityDef.moduledefs.TryGetValue(entityType, out module))
				{
					KBELog.ERROR_MSG("KBEngine::Client_onCreatedProxies: not found module(" + entityType + ")!");
					return;
				}
				
				Type runclass = module.entityScript;
				if(runclass == null)
					return;
				
				Entity entity = null;
				try
				{
					entity = (Entity)Activator.CreateInstance(runclass);
					entity.id = eid;
					entity.className = entityType;
					entity.onGetBase();

					entities[eid] = entity;

					MemoryStream entityMessage = null;
					_bufferedCreateEntityMessages.TryGetValue(eid, out entityMessage);
					
					if(entityMessage != null)
					{
						Client_onUpdatePropertys(entityMessage);
						_bufferedCreateEntityMessages.Remove(eid);
						entityMessage.reclaimObject();
					}
					
					entity.__init__();
					entity.attachComponents();
					entity.inited = true;
					
					if(_args.isOnInitCallPropertysSetMethods)
						entity.callPropertysSetMethods();
				}
				catch(Exception e)
				{
					KBELog.ERROR_MSG("KBEngine::Client_onCreatedProxies: init entity failed! eid(" + eid + "), entityType(" + entityType + "), error=" + e.ToString());
					if(entity != null)
					{
						entities.Remove(eid);
						entity.destroy();
					}
				}
			}
			else
			{
				MemoryStream entityMessage = null;
				_bufferedCreateEntityMessages.TryGetValue(eid, out entityMessage);
				
				if(entityMessage != null)
				{
					Client_onUpdatePropertys(entityMessage);
					_bufferedCreateEntityMessages.Remove(eid);
					entityMessage.reclaimObject();
				}
			}
			
			
			
		}
		
		public Entity findEntity(Int32 entityID)
		{
			Entity entity = null;
			
			if(!entities.TryGetValue(entityID, out entity))
			{
				return null;
			}
			
			return entity;
		}

		/*
			通过流数据获得View实体的ID
		*/
		public Int32 getViewEntityIDFromStream(MemoryStream stream)
		{
			if (!_args.useAliasEntityID)
				return stream.readInt32();

			Int32 id = 0;
			if(_entityIDAliasIDList.Count > 255)
			{
				id = stream.readInt32();
			}
			else
			{
				byte aliasID = stream.readUint8();
				
				// 如果为0且客户端上一步是重登陆或者重连操作并且服务端entity在断线期间一直处于在线状态
				// 则可以忽略这个错误, 因为cellapp可能一直在向baseapp发送同步消息， 当客户端重连上时未等
				// 服务端初始化步骤开始则收到同步信息, 此时这里就会出错。
				if(_entityIDAliasIDList.Count <= aliasID)
					return 0;
				
				id = _entityIDAliasIDList[aliasID];
			}
			
			return id;
		}
		
		/*
			服务端使用优化的方式更新实体属性数据
		*/
		public void Client_onUpdatePropertysOptimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);
			if(eid == 0)
			{
				// An unresolved alias is a stale update; never create a cache entry for entity 0.
				// 别名无法解析时属于过期更新，不能为实体 0 创建缓存。
				stream.done();
				return;
			}

			onUpdatePropertys_(eid, stream);
		}
		
		/*
			服务端更新实体属性数据
		*/
		public void Client_onUpdatePropertys(MemoryStream stream)
		{
			Int32 eid = stream.readInt32();
			onUpdatePropertys_(eid, stream);
		}
		
		public void onUpdatePropertys_(Int32 eid, MemoryStream stream)
		{
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				MemoryStream entityMessage = null;
				if(_bufferedCreateEntityMessages.TryGetValue(eid, out entityMessage))
				{
					// CellApp messages can converge before EnterWorld; retain every complete property payload in wire order.
					// 多个 CellApp 消息可能在 EnterWorld 前汇聚，必须按线序保留每一条完整属性 payload。
					entityMessage.append(stream.data(), (UInt32)stream.rpos, stream.length());
					return;
				}

				MemoryStream stream1 = MemoryStream.createObject();
				// Normalize the cached header so optimized (1-byte alias) and normal (4-byte ID) updates replay identically.
				// 统一缓存头部，确保优化消息（1 字节别名）与普通消息（4 字节 ID）回放行为一致。
				stream1.writeInt32(eid);
				stream1.append(stream.data(), (UInt32)stream.rpos, stream.length());
				_bufferedCreateEntityMessages[eid] = stream1;
				return;
			}
			
			entity.onUpdatePropertys(stream);
		}

		/*
			服务端使用优化的方式调用实体方法
		*/
		public void Client_onRemoteMethodCallOptimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);
			onRemoteMethodCall_(eid, stream);
		}
		
		/*
			服务端调用实体方法
		*/
		public void Client_onRemoteMethodCall(MemoryStream stream)
		{
			Int32 eid = stream.readInt32();
			onRemoteMethodCall_(eid, stream);
		}
	
		public void onRemoteMethodCall_(Int32 eid, MemoryStream stream)
		{
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onRemoteMethodCall: entity(" + eid + ") not found!");
				return;
			}
			
			entity.onRemoteMethodCall(stream);
		}

		/*
			服务端通知一个实体进入了世界(如果实体是当前玩家则玩家第一次在一个space中创建了， 如果是其他实体则是其他实体进入了玩家的View)
		*/
		public void Client_onEntityEnterWorld(MemoryStream stream)
		{
			Int32 eid = stream.readInt32();
			if(entity_id > 0 && entity_id != eid)
				_entityIDAliasIDList.Add(eid);
			
			UInt16 uentityType;
			if(EntityDef.idmoduledefs.Count > 255)
				uentityType = stream.readUint16();
			else
				uentityType = stream.readUint8();
			
			sbyte isOnGround = 1;
			
			if(stream.length() > 0)
				isOnGround = stream.readInt8();
			
			string entityType = EntityDef.idmoduledefs[uentityType].name;
			// KBELog.DEBUG_MSG("KBEngine::Client_onEntityEnterWorld: " + entityType + "(" + eid + "), spaceID(" + KBEngineApp.app.spaceID + ")!");
			
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				MemoryStream entityMessage = null;
				if(!_bufferedCreateEntityMessages.TryGetValue(eid, out entityMessage))
				{
					KBELog.ERROR_MSG("KBEngine::Client_onEntityEnterWorld: entity(" + eid + ") not found!");
					return;
				}
				
				ScriptModule module = null;
				if(!EntityDef.moduledefs.TryGetValue(entityType, out module))
				{
					KBELog.ERROR_MSG("KBEngine::Client_onEntityEnterWorld: not found module(" + entityType + ")!");
					return;
				}
				
				Type runclass = module.entityScript;
				if(runclass == null)
					return;
				
				entity = null;
				try
				{
					entity = (Entity)Activator.CreateInstance(runclass);
					entity.id = eid;
					entity.className = entityType;
					entity.onGetCell();

					entities[eid] = entity;

					Client_onUpdatePropertys(entityMessage);
					_bufferedCreateEntityMessages.Remove(eid);
					entityMessage.reclaimObject();
					
					entity.isOnGround = isOnGround > 0;
					entity.onDirectionChanged(entity.direction);
					entity.onPositionChanged(entity.position);
									
					entity.__init__();
					entity.attachComponents();
					entity.inited = true;
					entity.inWorld = true;
					entity.enterWorld();
					
					if(_args.isOnInitCallPropertysSetMethods)
						entity.callPropertysSetMethods();
				}
				catch(Exception e)
				{
					KBELog.ERROR_MSG("KBEngine::Client_onEntityEnterWorld: init entity failed! eid(" + eid + "), entityType(" + entityType + "), error=" + e.ToString());
					if(entity != null)
					{
						entities.Remove(eid);
						entity.destroy();
					}
				}
			}
			else
			{
				if(!entity.inWorld)
				{
					// 安全起见， 这里清空一下
					// 如果服务端上使用giveClientTo切换控制权
					// 之前的实体已经进入世界， 切换后的实体也进入世界， 这里可能会残留之前那个实体进入世界的信息
					_entityIDAliasIDList.Clear();
					clearEntities(false);
					entities[entity.id] = entity;
				
					entity.onGetCell();
					
					entity.onDirectionChanged(entity.direction);
					entity.onPositionChanged(entity.position);				

					_entityServerPos = entity.position;
					entity.isOnGround = isOnGround > 0;
					entity.inWorld = true;
					entity.enterWorld();

					if(_args.isOnInitCallPropertysSetMethods)
						entity.callPropertysSetMethods();
				}
			}
		}

		/*
			服务端使用优化的方式通知一个实体离开了世界(如果实体是当前玩家则玩家离开了space， 如果是其他实体则是其他实体离开了玩家的View)
		*/
		public void Client_onEntityLeaveWorldOptimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);
			KBEngineApp.app.Client_onEntityLeaveWorld(eid);
		}

		/*
			服务端通知一个实体离开了世界(如果实体是当前玩家则玩家离开了space， 如果是其他实体则是其他实体离开了玩家的View)
		*/
		public void Client_onEntityLeaveWorld(Int32 eid)
		{
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onEntityLeaveWorld: entity(" + eid + ") not found!");
				return;
			}
			
			if(entity.inWorld)
				entity.leaveWorld();
			
			if(entity_id == eid)
			{
				clearSpace(false);
				entity.onLoseCell();
			}
			else
			{
				if(_controlledEntities.Remove(entity))
					Event.fireOut(EventOutTypes.onLoseControlledEntity, entity);

				entities.Remove(eid);
				entity.destroy();
				_entityIDAliasIDList.Remove(eid);
			}
		}

		/*
			服务端通知当前玩家进入了一个新的space
		*/
		public void Client_onEntityEnterSpace(MemoryStream stream)
		{
			Int32 eid = stream.readInt32();
			spaceID = stream.readUint32();
			
			sbyte isOnGround = 1;
			
			if(stream.length() > 0)
				isOnGround = stream.readInt8();
			
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onEntityEnterSpace: entity(" + eid + ") not found!");
				return;
			}
			
			entity.isOnGround = isOnGround > 0;
			_entityServerPos = entity.position;
			entity.enterSpace();
		}
		
		/*
			服务端通知当前玩家离开了space
		*/
		public void Client_onEntityLeaveSpace(Int32 eid)
		{
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onEntityLeaveSpace: entity(" + eid + ") not found!");
				return;
			}
			
			entity.leaveSpace();
			clearSpace(false);
		}
	
		/*
			账号创建返回结果
		*/
		public void Client_onCreateAccountResult(MemoryStream stream)
		{
			UInt16 retcode = stream.readUint16();
			byte[] datas = stream.readBlob();
			
			Event.fireOut(EventOutTypes.onCreateAccountResult, retcode, datas);
			
			if(retcode != 0)
			{
				KBELog.WARNING_MSG("KBEngine::Client_onCreateAccountResult: " + username + " create is failed! code=" + retcode + "(" + serverErr(retcode)+ ")!");
				return;
			}
	
			KBELog.DEBUG_MSG("KBEngine::Client_onCreateAccountResult: " + username + " create is successfully!");
		}

		/*
			告诉客户端：你当前负责（或取消）控制谁的位移同步
		*/
		public void Client_onControlEntity(Int32 eid, sbyte isControlled)
		{
			Entity entity = null;

			if (!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onControlEntity: entity(" + eid + ") not found!");
				return;
			}

			var isCont = isControlled != 0;
			if (isCont)
			{
				// 如果被控制者是玩家自己，那表示玩家自己被其它人控制了
				// 所以玩家自己不应该进入这个被控制列表
				if (player().id != entity.id)
				{
					_controlledEntities.Add(entity);
				}
			}
			else
			{
				_controlledEntities.Remove(entity);
			}
			
			entity.isControlled = isCont;
			
			try
			{
				entity.onControlled(isCont);
				Event.fireOut(EventOutTypes.onControlled, entity, isCont);
			}
			catch (Exception e)
			{
				KBELog.ERROR_MSG(string.Format("KBEngine::Client_onControlEntity: entity id = '{0}', is controlled = '{1}', error = '{1}'", eid, isCont, e));
			}
		}

		/*
			更新当前玩家的位置与朝向到服务端， 可以通过开关_syncPlayerMS关闭这个机制
		*/
		public void updatePlayerToServer()
		{
			if(_updatePlayerToServerPeroid <= 0.01f || spaceID == 0)
			{
				return;
			}

			var now = DateTime.Now;
			TimeSpan span = now - _lastUpdateToServerTime;

			if (span.Ticks < _updatePlayerToServerPeroid * _1MS_TO_100NS)
				return;
			
			Entity playerEntity = player();
			if (playerEntity == null || playerEntity.inWorld == false || playerEntity.isControlled)
				return;

			_lastUpdateToServerTime = now - (span - TimeSpan.FromTicks(Convert.ToInt64(_updatePlayerToServerPeroid * _1MS_TO_100NS)));
			
			KBVector3 position = playerEntity.position;
			KBVector3 direction = playerEntity.direction;
			
			bool posHasChanged = KBVector3.Distance(playerEntity._entityLastLocalPos, position) > 0.001f;
			bool dirHasChanged = KBVector3.Distance(playerEntity._entityLastLocalDir, direction) > 0.001f;
			
			if(posHasChanged || dirHasChanged)
			{
				playerEntity._entityLastLocalPos = position;
				playerEntity._entityLastLocalDir = direction;

				Bundle bundle = Bundle.createObject();
				bundle.newMessage(Messages.messages["Baseapp_onUpdateDataFromClient"]);
				bundle.writeFloat(position.x);
				bundle.writeFloat(position.y);
				bundle.writeFloat(position.z);
				
				double x = ((double)direction.x / 360 * (System.Math.PI * 2));
				double y = ((double)direction.y / 360 * (System.Math.PI * 2));
				double z = ((double)direction.z / 360 * (System.Math.PI * 2));
				
				// 根据弧度转角度公式会出现负数
				// unity会自动转化到0~360度之间，这里需要做一个还原
				if(x - System.Math.PI > 0.0)
					x -= System.Math.PI * 2;

				if(y - System.Math.PI > 0.0)
					y -= System.Math.PI * 2;
				
				if(z - System.Math.PI > 0.0)
					z -= System.Math.PI * 2;
				
				bundle.writeFloat((float)x);
				bundle.writeFloat((float)y);
				bundle.writeFloat((float)z);
				bundle.writeUint8((Byte)(playerEntity.isOnGround == true ? 1 : 0));
				bundle.writeUint32(spaceID);
				bundle.send(_networkSession);
			}

			// 开始同步所有被控制了的entity的位置
			for (int i = 0; i < _controlledEntities.Count; ++i)
			{
				var entity = _controlledEntities[i];
				position = entity.position;
				direction = entity.direction;

				posHasChanged = KBVector3.Distance(entity._entityLastLocalPos, position) > 0.001f;
				dirHasChanged = KBVector3.Distance(entity._entityLastLocalDir, direction) > 0.001f;

				if (posHasChanged || dirHasChanged)
				{
					entity._entityLastLocalPos = position;
					entity._entityLastLocalDir = direction;

					Bundle bundle = Bundle.createObject();
					bundle.newMessage(Messages.messages["Baseapp_onUpdateDataFromClientForControlledEntity"]);
					bundle.writeInt32(entity.id);
					bundle.writeFloat(position.x);
					bundle.writeFloat(position.y);
					bundle.writeFloat(position.z);

					double x = ((double)direction.x / 360 * (System.Math.PI * 2));
					double y = ((double)direction.y / 360 * (System.Math.PI * 2));
					double z = ((double)direction.z / 360 * (System.Math.PI * 2));
				
					// 根据弧度转角度公式会出现负数
					// unity会自动转化到0~360度之间，这里需要做一个还原
					if(x - System.Math.PI > 0.0)
						x -= System.Math.PI * 2;

					if(y - System.Math.PI > 0.0)
						y -= System.Math.PI * 2;
					
					if(z - System.Math.PI > 0.0)
						z -= System.Math.PI * 2;
					
					bundle.writeFloat((float)x);
					bundle.writeFloat((float)y);
					bundle.writeFloat((float)z);
					bundle.writeUint8((Byte)(entity.isOnGround == true ? 1 : 0));
					bundle.writeUint32(spaceID);
					bundle.send(_networkSession);
				}
			}
		}

		/*
			当前space添加了关于几何等信息的映射资源
			客户端可以通过这个资源信息来加载对应的场景
		*/
		public void addSpaceGeometryMapping(UInt32 uspaceID, string respath)
		{
			KBELog.DEBUG_MSG("KBEngine::addSpaceGeometryMapping: spaceID(" + uspaceID + "), respath(" + respath + ")!");
			
			isLoadedGeometry = true;
			spaceID = uspaceID;
			spaceResPath = respath;
			Event.fireOut(EventOutTypes.addSpaceGeometryMapping, spaceResPath);
		}

		public void clearSpace(bool isall)
		{
			_entityIDAliasIDList.Clear();
			_spacedatas.Clear();
			clearEntities(isall);
			isLoadedGeometry = false;
			spaceID = 0;
		}
		
		public void clearEntities(bool isall)
		{
			_controlledEntities.Clear();

			if (!isall)
			{
				Entity entity = player();
				
				foreach (KeyValuePair<Int32, Entity> dic in entities)  
				{ 
					if(dic.Key == entity.id)
						continue;
					
					if(dic.Value.inWorld)
						dic.Value.leaveWorld();
					
					dic.Value.destroy();
				}  
		
				entities.Clear();
				entities[entity.id] = entity;
			}
			else
			{
				foreach (KeyValuePair<Int32, Entity> dic in entities)  
				{ 
					if(dic.Value.inWorld)
						dic.Value.leaveWorld();

					dic.Value.destroy();
				}  
		
				entities.Clear();
			}
		}
		
		/*
			服务端初始化客户端的spacedata， spacedata请参考API
		*/
		public void Client_initSpaceData(MemoryStream stream)
		{
			clearSpace(false);
			spaceID = stream.readUint32();
			
			while(stream.length() > 0)
			{
				string key = stream.readString();
				string val = stream.readString();
				Client_setSpaceData(spaceID, key, val);
			}
			
			KBELog.DEBUG_MSG("KBEngine::Client_initSpaceData: spaceID(" + spaceID + "), size(" + _spacedatas.Count + ")!");
		}

		/*
			服务端设置客户端的spacedata， spacedata请参考API
		*/
		public void Client_setSpaceData(UInt32 spaceID, string key, string value)
		{
			KBELog.DEBUG_MSG("KBEngine::Client_setSpaceData: spaceID(" + spaceID + "), key(" + key + "), value(" + value + ")!");
			_spacedatas[key] = value;
			
			if(key == "_mapping")
				addSpaceGeometryMapping(spaceID, value);
			
			Event.fireOut(EventOutTypes.onSetSpaceData, spaceID, key, value);
		}

		/*
			服务端删除客户端的spacedata， spacedata请参考API
		*/
		public void Client_delSpaceData(UInt32 spaceID, string key)
		{
			KBELog.DEBUG_MSG("KBEngine::Client_delSpaceData: spaceID(" + spaceID + "), key(" + key + ")");
			_spacedatas.Remove(key);
			Event.fireOut(EventOutTypes.onDelSpaceData, spaceID, key);
		}
		
		public string getSpaceData(string key)
		{
			string val = "";
			
			if(!_spacedatas.TryGetValue(key, out val))
			{
				return "";
			}
			
			return val;
		}

		/*
			服务端通知强制销毁一个实体
		*/
		public void Client_onEntityDestroyed(Int32 eid)
		{
			KBELog.DEBUG_MSG("KBEngine::Client_onEntityDestroyed: entity(" + eid + ")");
			
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onEntityDestroyed: entity(" + eid + ") not found!");
				return;
			}
			
			if(entity.inWorld)
			{
				if(entity_id == eid)
					clearSpace(false);
				
				entity.leaveWorld();
			}

			if(_controlledEntities.Remove(entity))
				Event.fireOut(EventOutTypes.onLoseControlledEntity, entity);

			entities.Remove(eid);
			entity.destroy();
		}
		
		/*
			服务端更新玩家的基础位置， 客户端以这个基础位置加上便宜值计算出玩家周围实体的坐标
		*/
		public void Client_onUpdateBasePos(float x, float y, float z)
		{
			KBVector3 oldVector3 = new KBVector3(_entityServerPos.x, _entityServerPos.y, _entityServerPos.z);
			
			_entityServerPos.x = x;
			_entityServerPos.y = y;
			_entityServerPos.z = z;

			var entity = player();
			if (entity != null && entity.isControlled)
			{
				entity.position.Set(_entityServerPos.x, _entityServerPos.y, _entityServerPos.z);
				// Event.fireOut(EventOutTypes.updatePosition, entity);
				entity.onSmoothPositionChanged(oldVector3);
				entity.onUpdateVolatileData();
			}
		}
		
		public void Client_onUpdateBasePosXZ(float x, float z)
		{
			KBVector3 oldVector3 = new KBVector3(_entityServerPos.x, _entityServerPos.y, _entityServerPos.z);
			_entityServerPos.x = x;
			_entityServerPos.z = z;

			var entity = player();
			if (entity != null && entity.isControlled)
			{
				entity.position.x = _entityServerPos.x;
				entity.position.z = _entityServerPos.z;
				// Event.fireOut(EventOutTypes.updatePosition, entity);
				entity.onSmoothPositionChanged(oldVector3);
				entity.onUpdateVolatileData();
			}
		}

		public void Client_onUpdateBaseDir(MemoryStream stream)
		{
			float yaw, pitch, roll;
			yaw = stream.readFloat();
			pitch = stream.readFloat();
			roll = stream.readFloat();

			
			// yaw = stream.readFloat() * 360 / ((float)System.Math.PI * 2);
			// pitch = stream.readFloat() * 360 / ((float)System.Math.PI * 2);
			// roll = stream.readFloat() * 360 / ((float)System.Math.PI * 2);

			var entity = player();
			if (entity != null && entity.isControlled)
			{
				KBVector3 oldDirection = new KBVector3(entity.direction.x, entity.direction.y, entity.direction.z);
				entity.direction.Set(roll, pitch, yaw);
				// Event.fireOut(EventOutTypes.set_direction, entity);
				entity.onDirectionChanged(oldDirection);
				entity.onUpdateVolatileData();
			}
		}

		public void Client_onUpdateData(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onUpdateData: entity(" + eid + ") not found!");
				return;
			}
		}

		/*
			服务端强制设置了玩家的坐标 
			例如：在服务端使用avatar.position=(0,0,0), 或者玩家位置与速度异常时会强制拉回到一个位置
		*/
		public void Client_onSetEntityPosAndDir(MemoryStream stream)
		{
			Int32 eid = stream.readInt32();
			Entity entity = null;
			
			if(!entities.TryGetValue(eid, out entity))
			{
				KBELog.ERROR_MSG("KBEngine::Client_onSetEntityPosAndDir: entity(" + eid + ") not found!");
				return;
			}
			
			KBVector3 old_position = new KBVector3(entity.position.x, entity.position.y, entity.position.z);
			KBVector3 old_direction = new KBVector3(entity.direction.x, entity.direction.y, entity.direction.z);

			entity.position.x = stream.readFloat();
			entity.position.y = stream.readFloat();
			entity.position.z = stream.readFloat();
			
			entity.direction.x = stream.readFloat();
			entity.direction.y = stream.readFloat();
			entity.direction.z = stream.readFloat();

			entity._entityLastLocalPos = entity.position;
			entity._entityLastLocalDir = entity.direction;
			
			entity.onDirectionChanged(old_direction);
			entity.onPositionChanged(old_position);		
		}
		
		public void Client_onUpdateData_ypr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float y = stream.readFloat();
			float p = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, p, r, -1, false);
		}
		
		public void Client_onUpdateData_yp(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float y = stream.readFloat();
			float p = stream.readFloat();
			
			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, p, KBEMath.KBE_FLT_MAX, -1, false);
		}
		
		public void Client_onUpdateData_yr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float y = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, KBEMath.KBE_FLT_MAX, r, -1, false);
		}
		
		public void Client_onUpdateData_pr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float p = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, p, r, -1, false);
		}
		
		public void Client_onUpdateData_y(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float y = stream.readFloat();
			
			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, -1, false);
		}
		
		public void Client_onUpdateData_p(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);
			
			float p = stream.readFloat();
			
			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, p, KBEMath.KBE_FLT_MAX, -1, false);
		}
		
		public void Client_onUpdateData_r(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float r = stream.readFloat();
			
			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, r, -1, false);
		}
		
		public void Client_onUpdateData_xz(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 1, false);
		}
		
		public void Client_onUpdateData_xz_ypr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			float y = stream.readFloat();
			float p = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, y, p, r, 1, false);
		}
		
		public void Client_onUpdateData_xz_yp(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			float y = stream.readFloat();
			float p = stream.readFloat();
			
			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, y, p, KBEMath.KBE_FLT_MAX, 1, false);
		}
		
		public void Client_onUpdateData_xz_yr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			float y = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, y, KBEMath.KBE_FLT_MAX, r, 1, false);
		}
		
		public void Client_onUpdateData_xz_pr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			float p = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, KBEMath.KBE_FLT_MAX, p, r, 1, false);
		}
		
		public void Client_onUpdateData_xz_y(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			float yaw = stream.readFloat();

			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, yaw, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 1, false);
		}
		
		public void Client_onUpdateData_xz_p(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			float p = stream.readFloat();
			
			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, KBEMath.KBE_FLT_MAX, p, KBEMath.KBE_FLT_MAX, 1, false);
		}
		
		public void Client_onUpdateData_xz_r(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float z = stream.readFloat();

			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, KBEMath.KBE_FLT_MAX, z, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, r, 1, false);
		}
		
		public void Client_onUpdateData_xyz(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			_updateVolatileData(eid, x, y, z, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 0, false);
		}
		
		public void Client_onUpdateData_xyz_ypr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			float yaw = stream.readFloat();
			float p = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, y, z, yaw, p, r, 0, false);
		}
		
		public void Client_onUpdateData_xyz_yp(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			float yaw = stream.readFloat();
			float p = stream.readFloat();

			_updateVolatileData(eid, x, y, z, yaw, p, KBEMath.KBE_FLT_MAX, 0, false);
		}
		
		public void Client_onUpdateData_xyz_yr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			float yaw = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, y, z, yaw, KBEMath.KBE_FLT_MAX, r, 0, false);
		}
		
		public void Client_onUpdateData_xyz_pr(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			float p = stream.readFloat();
			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, y, z, KBEMath.KBE_FLT_MAX, p, r, 0, false);
		}
		
		public void Client_onUpdateData_xyz_y(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			float yaw = stream.readFloat();

			_updateVolatileData(eid, x, y, z, yaw, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 0, false);
		}
		
		public void Client_onUpdateData_xyz_p(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			float p = stream.readFloat();
			
			_updateVolatileData(eid, x, y, z, KBEMath.KBE_FLT_MAX, p, KBEMath.KBE_FLT_MAX, 0, false);
		}
		
		public void Client_onUpdateData_xyz_r(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);
			
			float x = stream.readFloat();
			float y = stream.readFloat();
			float z = stream.readFloat();

			float r = stream.readFloat();
			
			_updateVolatileData(eid, x, y, z, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, r, 0, false);
		}

		public void Client_onUpdateData_ypr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			SByte y = stream.readInt8();
			SByte p = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, p, r, -1, true);
		}

		public void Client_onUpdateData_yp_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			SByte y = stream.readInt8();
			SByte p = stream.readInt8();

			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, p, KBEMath.KBE_FLT_MAX, -1, true);
		}

		public void Client_onUpdateData_yr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			SByte y = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, KBEMath.KBE_FLT_MAX, r, -1, true);
		}

		public void Client_onUpdateData_pr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			SByte p = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, p, r, -1, true);
		}

		public void Client_onUpdateData_y_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			SByte y = stream.readInt8();

			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, y, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, -1, true);
		}

		public void Client_onUpdateData_p_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			SByte p = stream.readInt8();

			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, p, KBEMath.KBE_FLT_MAX, -1, true);
		}

		public void Client_onUpdateData_r_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			SByte r = stream.readInt8();

			_updateVolatileData(eid, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, r, -1, true);
		}

		public void Client_onUpdateData_xz_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();

			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 1, true);
		}

		public void Client_onUpdateData_xz_ypr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();

			SByte y = stream.readInt8();
			SByte p = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], y, p, r, 1, true);
		}

		public void Client_onUpdateData_xz_yp_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();

			SByte y = stream.readInt8();
			SByte p = stream.readInt8();

			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], y, p, KBEMath.KBE_FLT_MAX, 1, true);
		}

		public void Client_onUpdateData_xz_yr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();

			SByte y = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], y, KBEMath.KBE_FLT_MAX, r, 1, true);
		}

		public void Client_onUpdateData_xz_pr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();

			SByte p = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], KBEMath.KBE_FLT_MAX, p, r, 1, true);
		}

		public void Client_onUpdateData_xz_y_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);
			KBVector2 xz = stream.readPackXZ();
			SByte yaw = stream.readInt8();
			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], yaw, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 1, true);
		}

		public void Client_onUpdateData_xz_p_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();

			SByte p = stream.readInt8();

			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], KBEMath.KBE_FLT_MAX, p, KBEMath.KBE_FLT_MAX, 1, true);
		}

		public void Client_onUpdateData_xz_r_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();

			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], KBEMath.KBE_FLT_MAX, xz[1], KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, r, 1, true);
		}

		public void Client_onUpdateData_xyz_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			_updateVolatileData(eid, xz[0], y, xz[1], KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 0, true);
		}

		public void Client_onUpdateData_xyz_ypr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			SByte yaw = stream.readInt8();
			SByte p = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], y, xz[1], yaw, p, r, 0, true);
		}

		public void Client_onUpdateData_xyz_yp_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			SByte yaw = stream.readInt8();
			SByte p = stream.readInt8();

			_updateVolatileData(eid, xz[0], y, xz[1], yaw, p, KBEMath.KBE_FLT_MAX, 0, true);
		}

		public void Client_onUpdateData_xyz_yr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			SByte yaw = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], y, xz[1], yaw, KBEMath.KBE_FLT_MAX, r, 0, true);
		}

		public void Client_onUpdateData_xyz_pr_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			SByte p = stream.readInt8();
			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], y, xz[1], KBEMath.KBE_FLT_MAX, p, r, 0, true);
		}

		public void Client_onUpdateData_xyz_y_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			SByte yaw = stream.readInt8();
			_updateVolatileData(eid, xz[0], y, xz[1], yaw, KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, 0, true);
		}

		public void Client_onUpdateData_xyz_p_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			SByte p = stream.readInt8();

			_updateVolatileData(eid, xz[0], y, xz[1], KBEMath.KBE_FLT_MAX, p, KBEMath.KBE_FLT_MAX, 0, true);
		}

		public void Client_onUpdateData_xyz_r_optimized(MemoryStream stream)
		{
			Int32 eid = getViewEntityIDFromStream(stream);

			KBVector2 xz = stream.readPackXZ();
			float y = stream.readPackY();

			SByte r = stream.readInt8();

			_updateVolatileData(eid, xz[0], y, xz[1], KBEMath.KBE_FLT_MAX, KBEMath.KBE_FLT_MAX, r, 0, true);
		}

		private void _updateVolatileData(Int32 entityID, float x, float y, float z, float yaw, float pitch, float roll, sbyte isOnGround, bool isOptimized)
		{
			Entity entity = null;

			if(!entities.TryGetValue(entityID, out entity))
			{
				// 如果为0且客户端上一步是重登陆或者重连操作并且服务端entity在断线期间一直处于在线状态
				// 则可以忽略这个错误, 因为cellapp可能一直在向baseapp发送同步消息， 当客户端重连上时未等
				// 服务端初始化步骤开始则收到同步信息, 此时这里就会出错。
				KBELog.ERROR_MSG("KBEngine::_updateVolatileData: entity(" + entityID + ") not found!");
				return;
			}
			KBVector3 oldDirection = new KBVector3(entity.direction.x, entity.direction.y, entity.direction.z);
			// 小于0不设置
			if(isOnGround >= 0)
			{
				entity.isOnGround = (isOnGround > 0);
			}
		
			bool changeDirection = false;
			
			if(roll != KBEMath.KBE_FLT_MAX)
			{
				changeDirection = true;
				entity.direction.x = isOptimized ? KBEMath.int82angle((SByte)roll, false) : roll;
				// entity.direction.x = (isOptimized ? KBEMath.int82angle((SByte)roll, false) : roll) * 360 / ((float)System.Math.PI * 2);
			}

			if(pitch != KBEMath.KBE_FLT_MAX)
			{
				changeDirection = true;
				entity.direction.y = isOptimized ? KBEMath.int82angle((SByte)pitch, false) : pitch;
				// entity.direction.y = (isOptimized ? KBEMath.int82angle((SByte)pitch, false) : pitch) * 360 / ((float)System.Math.PI * 2);
			}
			
			if(yaw != KBEMath.KBE_FLT_MAX)
			{
				changeDirection = true;
				entity.direction.z = isOptimized ? KBEMath.int82angle((SByte)yaw, false) : yaw ;
				// entity.direction.z = (isOptimized ? KBEMath.int82angle((SByte)yaw, false) : yaw) * 360 / ((float)System.Math.PI * 2);
			}
			
			bool done = false;
			if(changeDirection == true)
			{
				// Event.fireOut(EventOutTypes.set_direction, entity);
				entity.onDirectionChanged(oldDirection);
				done = true;
			}
			
			bool positionChanged = x != KBEMath.KBE_FLT_MAX || y != KBEMath.KBE_FLT_MAX || z != KBEMath.KBE_FLT_MAX;
			if (x == KBEMath.KBE_FLT_MAX) x = isOptimized ? 0.0f : entity.position.x;
			if (y == KBEMath.KBE_FLT_MAX) y = isOptimized ? 0.0f : entity.position.y;
			if (z == KBEMath.KBE_FLT_MAX) z = isOptimized ? 0.0f : entity.position.z;

			if (positionChanged)
			{
				KBVector3 pos = isOptimized ? new KBVector3(x + _entityServerPos.x, y + _entityServerPos.y, z + _entityServerPos.z) : new KBVector3(x, y, z);
				KBVector3 oldVector3 = new KBVector3(entity.position.x, entity.position.y, entity.position.z);

				entity.position = pos;
				done = true;
				// Event.fireOut(EventOutTypes.updatePosition, entity);
				
				entity.onSmoothPositionChanged(oldVector3);
			}
			
			if(done)
				entity.onUpdateVolatileData();
		}
		
		/*
			服务端通知流数据下载开始
			请参考API手册关于onStreamDataStarted
		*/
		public void Client_onStreamDataStarted(Int16 id, UInt32 datasize, string descr)
		{
			Event.fireOut(EventOutTypes.onStreamDataStarted, id, datasize, descr);
		}
		
		public void Client_onStreamDataRecv(MemoryStream stream)
		{
			Int16 resID = stream.readInt16();
			byte[] datas = stream.readBlob();
			Event.fireOut(EventOutTypes.onStreamDataRecv, resID, datas);
		}
		
		public void Client_onStreamDataCompleted(Int16 id)
		{
			Event.fireOut(EventOutTypes.onStreamDataCompleted, id);
		}
	}
	

	public class KBEngineAppThread : KBEngineApp
	{
		/*
			KBEngine处理线程
		*/
		public class KBEThread
		{

			KBEngineApp app_;
			public volatile bool over = false;
			
			public KBEThread(KBEngineApp app)
			{
				this.app_ = app;
			}

			public void run()
			{
				KBELog.INFO_MSG("KBEThread::run()");
				over = false;

				try
				{
					this.app_.process();
				}
				catch (Exception e)
				{
					KBELog.ERROR_MSG(e.ToString());
				}
				
				over = true;
				KBELog.INFO_MSG("KBEThread::end()");
			}
		}
	
		private Thread _t = null;
		public KBEThread kbethread = null;

		// 主循环频率
		public static int threadUpdateHZ = 10;

		// 主循环周期ms 优化去掉循环中做除法
		private static float threadUpdatePeriod = 1000f / threadUpdateHZ;
		
		// 插件是否退出
		private volatile bool _isbreak = false;
		
		private System.DateTime _lasttime = System.DateTime.Now;

		public KBEngineAppThread(KBEngineArgs args) : 
			base(args)
		{
		}

		public override bool initialize(KBEngineArgs args)
		{
			base.initialize(args);
			
			KBEngineAppThread.threadUpdateHZ = args.threadUpdateHZ;
			threadUpdatePeriod = 1000f / threadUpdateHZ;
			
			kbethread = new KBEThread(this);
			_t = new Thread(new ThreadStart(kbethread.run));
			_t.Start();
			
			return true;
		}

		public override void reset()
		{
			_isbreak = false;
			_lasttime = System.DateTime.Now;
			
			base.reset();
		}

		/*
			插件退出处理
		*/
		public void breakProcess()
		{
			_isbreak = true;
		}
		
		public bool isbreak()
		{
			return _isbreak;
		}
		
		public override void process()
		{
			while(!isbreak())
			{
				base.process();
				_thread_wait();
			}
			
			KBELog.WARNING_MSG("KBEngineAppThread::process(): break!");
		}
	
		/*
			防止占满CPU, 需要让线程等待一会
		*/
		void _thread_wait()
		{
			TimeSpan span = DateTime.Now - _lasttime; 
			
			int diff = (int)(threadUpdatePeriod - span.TotalMilliseconds);

			if(diff < 0)
				diff = 0;
			
			System.Threading.Thread.Sleep(diff);
			_lasttime = DateTime.Now;
		}
		
		public override void destroy()
		{
			KBELog.WARNING_MSG("KBEngineAppThread::destroy()");
			breakProcess();

			if (_t != null)
			{
				// 线程只执行非阻塞 process 与短暂 Sleep，协作退出可避免 Thread.Abort 在现代 .NET 上直接抛出平台异常。
				// The worker only performs nonblocking process calls and short sleeps, so cooperative exit avoids Thread.Abort platform failures on modern .NET.
				if (_t != Thread.CurrentThread && !_t.Join(TimeSpan.FromSeconds(5)))
					throw new TimeoutException("KBEngine worker thread did not stop within 5 seconds.");

				// destroy 可能由工作线程事件回调触发；该路径不能 Join 自身，break 标记会让当前 process 返回后自然退出。
				// destroy may run from a worker-thread event callback; that path cannot join itself, and the break flag exits naturally after the current process call returns.
				_t = null;
			}

			base.destroy();
		}
	}
} 
