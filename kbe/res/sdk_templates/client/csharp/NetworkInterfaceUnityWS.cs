using System;
using System.Net;
using System.Collections.Generic;
using System.Net.Sockets;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using NativeWebSocket;

using MessageLengthEx = System.UInt32;
namespace KBEngine
{
    /// <summary>
    /// 网络模块
    /// 处理连接、收发数据
    /// </summary>
    public class NetworkInterfaceUnityWS : NetworkInterfaceBase
    {
        private WebSocket _webSocket = null;
        private MessageReaderUnityWS _messageReader = new();
        private WebSocketOpenEventHandler _openHandler = null;
        private WebSocketErrorEventHandler _errorHandler = null;
        private WebSocketCloseEventHandler _closeHandler = null;
        private WebSocketMessageEventHandler _messageHandler = null;

        private ConnectState _state = null;
        ~NetworkInterfaceUnityWS()
        {
            KBELog.DEBUG_MSG("NetworkInterfaceUnityWS::~NetworkInterfaceUnityWS(), destructed!!!");
            reset();
        }

        public override bool valid()
        {
            WebSocket webSocket = _webSocket;
            return webSocket != null && (webSocket.State == WebSocketState.Open || webSocket.State == WebSocketState.Connecting);
        }

        public WebSocket GetWebSocket()
        {
            return _webSocket;
        }

        protected override bool isCurrentConnection(ConnectState state)
        {
            return Object.ReferenceEquals(state.networkInterface, this) && Object.ReferenceEquals(state.webSocket, _webSocket);
        }

        public override void  connectTo(string ip, int port, ConnectCallback callback, object userData, Dictionary<string, string> domainMapping, Dictionary<int, int> portMapping)
        {
            if (valid())
                throw new InvalidOperationException("Have already connected!");

            // if (!(new Regex(@"((?:(?:25[0-5]|2[0-4]\d|((1\d{2})|([1-9]?\d)))\.){3}(?:25[0-5]|2[0-4]\d|((1\d{2})|([1-9]?\d))))")).IsMatch(ip))
            // {
            //     IPHostEntry ipHost = Dns.GetHostEntry(ip);
            //     ip = ipHost.AddressList[0].ToString();
            // }
            
            ip = domainMapping.ContainsKey(ip) ? domainMapping[ip] : ip;
            port = portMapping.ContainsKey(port) ? portMapping[port] : port;

            if (KBEngineApp.app.getInitArgs().enableWSS)
            {
                ip = "wss://" + ip;
            }
            else
            {
                ip = "ws://" + ip;
            }
            
            _webSocket = createWebSocket( ip + ":" + port);

            ConnectState state = new ConnectState();
            _state = state;
            state.connectIP = ip;
            state.connectPort = port;
            state.connectCB = callback;
            state.userData = userData;
            state.webSocket = _webSocket;
            state.networkInterface = this;

            KBELog.DEBUG_MSG("connect to " + ip + ":" + port + " ...");
            connected = false;

            // 先注册一个事件回调，该事件在当前线程触发
            Event.registerIn("_onConnectionState", this, "_onConnectionState");

            // asyncConnectMethod.BeginInvoke(state, new AsyncCallback(this._asyncConnectCB), state);
            _ = ConnectAsync(state);
        }


        protected override async Task ConnectAsync(ConnectState state)
        {
            try
            {
                await OnWebSocketAsyncConnect(state);

                // 这里直接调用回调函数
                onAsyncConnectCB(state);

                KBELog.DEBUG_MSG($"NetworkInterfaceBase::ConnectAsync(), connect to '{state.connectIP}:{state.connectPort}' finished. error = '{state.error}'");

                if (state.error.Length > 0)
                    Event.fireIn("_onConnectionState", new object[] { state });
            }
            catch (Exception ex)
            {
                KBELog.ERROR_MSG($"NetworkInterfaceBase::ConnectAsync() error: {ex}");
                state.error = ex.ToString();
                Event.fireIn("_onConnectionState", new object[] { state });
            }
        }

        protected override void onAsyncConnect(ConnectState state)
        {
            throw new NotImplementedException();
        }

        protected async Task OnWebSocketAsyncConnect(ConnectState state)
        {
            try
            {
                await state.webSocket.Connect();
            }
            catch (Exception e)
            {
                KBELog.ERROR_MSG(string.Format(
                    "NetworkInterfaceTCP::_asyncConnect(), connect to '{0}:{1}' fault! error = '{2}'", state.connectIP,
                    state.connectPort, e));
                state.error = e.ToString();
            }
        }


        protected override void releaseTransport()
        {
            WebSocket webSocket = _webSocket;
            _webSocket = null;
            if (webSocket != null)
            {
                if (_openHandler != null)
                    webSocket.OnOpen -= _openHandler;
                if (_errorHandler != null)
                    webSocket.OnError -= _errorHandler;
                if (_closeHandler != null)
                    webSocket.OnClose -= _closeHandler;
                if (_messageHandler != null)
                    webSocket.OnMessage -= _messageHandler;
                _ = CloseWebSocket(webSocket);
            }

            _openHandler = null;
            _errorHandler = null;
            _closeHandler = null;
            _messageHandler = null;

            base.releaseTransport();
        }

        private static async Task CloseWebSocket(WebSocket webSocket)
        {
            try
            {
                await webSocket.Close();
            }
            catch (Exception exception)
            {
                // 状态和事件已同步拆除，异步 close 失败只保留诊断，不能重新激活或重复通知旧连接。
                // State and handlers are detached synchronously; an asynchronous close failure is diagnostic only and must not reactivate or renotify the old connection.
                KBELog.WARNING_MSG("NetworkInterfaceUnityWS::CloseWebSocket(): " + exception);
            }
        }

        protected override Socket createSocket()
        {
            return null;
        }

        private WebSocket createWebSocket(String url)
        {
            WebSocket webSocket = new WebSocket(url);
            // 每个委托捕获自己的 transport；即使事件已经由底层排队，旧连接也不能借用后来连接的 _state。
            // Each delegate captures its own transport, so even an event already queued by the backend cannot reuse a later connection's _state.
            _openHandler = () => OnOpenHandler(webSocket);
            _errorHandler = error => OnErrorHandler(webSocket, error);
            _closeHandler = code => OnCloseHandler(webSocket, code);
            _messageHandler = bytes => OnMessageHandler(webSocket, bytes);
            webSocket.OnOpen += _openHandler;
            webSocket.OnError += _errorHandler;
            webSocket.OnClose += _closeHandler;
            webSocket.OnMessage += _messageHandler;
            
            return webSocket;
        }
        
        

        protected override PacketReceiverBase createPacketReceiver()
        {
            return new PacketReceiverUnityWS(this);
        }

        protected override PacketSenderBase createPacketSender()
        {
            return new PacketSenderUnityWS(this);
        }


        
        
        private void OnOpenHandler(WebSocket source)
        {
            if (!Object.ReferenceEquals(source, _webSocket))
                return;

            Event.fireIn("_onConnectionState", new object[] { _state });
        }

        private void OnErrorHandler(WebSocket source, string error)
        {
            if (!Object.ReferenceEquals(source, _webSocket))
                return;

            KBELog.ERROR_MSG("NetworkInterfaceUnityWS::Error! " + error);
            if (!connected)
            {
                _state.error = error;
                Event.fireIn("_onConnectionState", new object[] { _state });
                return;
            }

            close();
        }

        private void OnCloseHandler(WebSocket source, WebSocketCloseCode code)
        {
            if (!Object.ReferenceEquals(source, _webSocket))
                return;

            KBELog.ERROR_MSG("NetworkInterfaceUnityWS::Connection closed! code=" + code);
            if (!connected)
            {
                _state.error = "WebSocket closed while connecting: " + code;
                Event.fireIn("_onConnectionState", new object[] { _state });
                return;
            }

            close();
        }

        private void OnMessageHandler(WebSocket source, byte[] bytes)
        {
            if (!Object.ReferenceEquals(source, _webSocket))
                return;

            try
            {
                _messageReader.process(bytes, 0, (MessageLengthEx)bytes.Length);
            }
            catch (Exception e)
            {
                KBELog.ERROR_MSG(e.ToString());
            }
        }
    }
}
