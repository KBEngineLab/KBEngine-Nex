#if GODOT
using System;
using System.Collections;
using Godot;
using KBEngine;

/*
    可以理解为插件的入口模块
    在这个入口中安装了需要监听的事件(installEvents)，同时初始化KBEngine(initKBEngine)
*/

public partial class GodotKBEMain : Node
{
    public KBEngineApp gameapp = null;

    // 在unity3d界面中可见选项
    [Export]
    public DEBUGLEVEL debugLevel = DEBUGLEVEL.DEBUG;
    [Export]
    public bool isMultiThreads = false;
    [Export]
    public string ip = "127.0.0.1";
    [Export]
	public int port = @{KBE_LOGIN_PORT};
    [Export]
    public KBEngineApp.CLIENT_TYPE clientType = KBEngineApp.CLIENT_TYPE.CLIENT_TYPE_MINI;

    [Export]
	public KBEngineApp.NETWORK_TYPE networkType = KBEngineApp.NETWORK_TYPE.KCP;
    [Export]
	public bool enableWSS = false;


    [Export]
    public KBEngineApp.NETWORK_ENCRYPT_TYPE networkEncryptType = KBEngineApp.NETWORK_ENCRYPT_TYPE.ENCRYPT_TYPE_NONE;
    
    [Export]
	public int syncPlayerMS = 1000 / @{KBE_UPDATEHZ};

    [Export]
	public int threadUpdateHZ = @{KBE_UPDATEHZ} * 2;
    [Export]
	public int serverHeartbeatTick = @{KBE_SERVER_EXTERNAL_TIMEOUT};
    [Export]
    public int TCP_SEND_BUFFER_MAX = (int)KBEngine.NetworkInterfaceBase.TCP_PACKET_MAX;
    [Export]
    public int TCP_RECV_BUFFER_MAX = (int)KBEngine.NetworkInterfaceBase.TCP_PACKET_MAX;
    [Export]
    public int UDP_SEND_BUFFER_MAX = (int)KBEngine.NetworkInterfaceBase.UDP_PACKET_MAX;
    [Export]
    public int UDP_RECV_BUFFER_MAX = (int)KBEngine.NetworkInterfaceBase.UDP_PACKET_MAX;
    [Export]
	public bool useAliasEntityID = @{KBE_USE_ALIAS_ENTITYID};
    [Export]
    public bool isOnInitCallPropertysSetMethods = true;


    public override void _Ready()
    {
        // DontDestroyOnLoad(transform.gameObject);
        
        GD.Print("clientapp::start()");
        
        installEvents();
        initKBEngine();
    }
    
    
    public virtual void installEvents()
    {
        KBEngine.Event.registerOut(EventOutTypes.onVersionNotMatch, this, "onVersionNotMatch");
        KBEngine.Event.registerOut(EventOutTypes.onScriptVersionNotMatch, this, "onScriptVersionNotMatch");
    }

    public override void _PhysicsProcess(double delta)
    {
        KBEUpdate();
    }

    public void onVersionNotMatch(string verInfo, string serVerInfo)
    {
        KBELog.INFO_MSG("onVersionNotMatch");
    }

    public void onScriptVersionNotMatch(string verInfo, string serVerInfo)
    {

        KBELog.INFO_MSG("onScriptVersionNotMatch");
    }

    public virtual void initKBEngine()
    {
        // 如果此处发生错误，请查看 Assets\Scripts\kbe_scripts\if_Entity_error_use______git_submodule_update_____kbengine_plugins_______open_this_file_and_I_will_tell_you.cs

        KBELog.DebugLevel = debugLevel;

        KBEngineArgs args = new KBEngineArgs();

        args.ip = ip;
        args.port = port;
        args.clientType = clientType;
		args.networkType = networkType;
		args.enableWSS =  enableWSS;
        args.networkEncryptType = networkEncryptType;
        args.syncPlayerMS = syncPlayerMS;
        args.threadUpdateHZ = threadUpdateHZ;
        args.serverHeartbeatTick = serverHeartbeatTick / 2;
        args.useAliasEntityID = useAliasEntityID;
        args.isOnInitCallPropertysSetMethods = isOnInitCallPropertysSetMethods;

        args.TCP_SEND_BUFFER_MAX = (UInt32)TCP_SEND_BUFFER_MAX;
        args.TCP_RECV_BUFFER_MAX = (UInt32)TCP_RECV_BUFFER_MAX;
        args.UDP_SEND_BUFFER_MAX = (UInt32)UDP_SEND_BUFFER_MAX;
        args.UDP_RECV_BUFFER_MAX = (UInt32)UDP_RECV_BUFFER_MAX;

        args.isMultiThreads = isMultiThreads;

        if (isMultiThreads)
            gameapp = new KBEngineAppThread(args);
        else
            gameapp = new KBEngineApp(args);
    }

    protected virtual void OnDestroy()
    {
        GD.Print("clientapp::OnDestroy(): begin");
        if (KBEngineApp.app != null)
        {
            KBEngineApp.app.destroy();
            KBEngineApp.app = null;
        }

        KBEngine.Event.clear();
        GD.Print("clientapp::OnDestroy(): end");
        
    }

    public virtual void KBEUpdate()
    {
        // 单线程模式必须自己调用
        if (!isMultiThreads)
            gameapp.process();

        KBEngine.Event.processOutEvents();
    }
}
#endif