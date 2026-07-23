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

        private ConnectState _state = null;
        ~NetworkInterfaceUnityWS()
        {
            KBELog.DEBUG_MSG("NetworkInterfaceUnityWS::~NetworkInterfaceUnityWS(), destructed!!!");
            reset();
        }

        public override bool valid()
        {
            return ((_webSocket != null) && (_webSocket.State == WebSocketState.Open || _webSocket.State == WebSocketState.Connecting));
        }

        public WebSocket GetWebSocket()
        {
            return _webSocket;
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

            }
            catch (Exception ex)
            {
                KBELog.ERROR_MSG($"NetworkInterfaceBase::ConnectAsync() error: {ex}");
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


        public override void reset()
        {
            _ = UninstallEvent();
            base.reset();
        }

        public override void close()
        {
            _ = UninstallEvent();
            base.close();
        }

        private async Task UninstallEvent()
        {
            
            
            // 主动关闭连接
            if (_webSocket != null)
            {
                _webSocket.OnOpen -= OnOpenHandler;
                _webSocket.OnError -= OnErrorHandler;
                _webSocket.OnClose -= OnCloseHandler;
                _webSocket.OnMessage -= OnMessageHandler;
                
                await _webSocket.Close();
                _webSocket = null;
            }
        }

        protected override Socket createSocket()
        {
            return null;
        }

        private WebSocket createWebSocket(String url)
        {
            WebSocket webSocket = new WebSocket(url);
            webSocket.OnOpen += OnOpenHandler;
            webSocket.OnError += OnErrorHandler;
            webSocket.OnClose += OnCloseHandler;
            webSocket.OnMessage += OnMessageHandler;
            
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


        
        
        private void OnOpenHandler()
        {
            Event.fireIn("_onConnectionState", new object[] { _state });
        }

        private void OnErrorHandler(string e)
        {
            KBELog.ERROR_MSG("NetworkInterfaceUnityWS::Error! " + e);
            close();
        }

        private void OnCloseHandler(WebSocketCloseCode code)
        {
            KBELog.ERROR_MSG("NetworkInterfaceUnityWS::Connection closed! code=" + code);
            close();
        }

        private void OnMessageHandler(byte[] bytes)
        {
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