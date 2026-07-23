namespace KBEngine
{
  	 
	using System; 
	using System.Collections; 
	using System.Collections.Generic;
	using System.Threading;

    /// <summary>
    /// KBE-Plugin fire-out events(KBE => Unity):
    /// </summary>
    public class EventOutTypes
    {
        // ------------------------------------账号相关------------------------------------

        /// <summary>
        /// Create account feedback results.
        /// <para> param1(uint16): retcode. // server_errors</para>
        /// <para> param2(bytes): datas. // If you use third-party account system, the system may fill some of the third-party additional datas. </para>
        /// </summary>
        public const string onCreateAccountResult = "onCreateAccountResult";

        /// <summary>
        // Response from binding account Email request.
        // <para> param1(uint16): retcode. // server_errors</para>
        /// </summary>
        public const string onBindAccountEmail = "onBindAccountEmail";

        /// <summary>
        // Response from a new password request.
        // <para> param1(uint16): retcode. // server_errors</para>
        /// </summary>
        public const string onNewPassword = "onNewPassword";

        /// <summary>
        // Response from a reset password request.
        // <para> param1(uint16): retcode. // server_errors</para>
        /// </summary>
        public const string onResetPassword = "onResetPassword";

        // ------------------------------------连接相关------------------------------------
        /// <summary>
        /// Kicked of the current server.
        /// <para> param1(uint16): retcode. // server_errors</para>
        /// </summary>
        public const string onKicked = "onKicked";

        /// <summary>
        /// Disconnected from the server.
        /// </summary>
        public const string onDisconnected = "onDisconnected";

        /// <summary>
        /// Status of connection server.
        /// <para> param1(bool): success or fail</para>
        /// </summary>
        public const string onConnectionState = "onConnectionState";

        // ------------------------------------logon相关------------------------------------
        /// <summary>
        /// Engine version mismatch.
        /// <para> param1(string): clientVersion</para>
        /// <para> param2(string): serverVersion</para>
        /// </summary>
        public const string onVersionNotMatch = "onVersionNotMatch";

        /// <summary>
        /// script version mismatch.
        /// <para> param1(string): clientScriptVersion</para>
        /// <para> param2(string): serverScriptVersion</para>
        /// </summary>
        public const string onScriptVersionNotMatch = "onScriptVersionNotMatch";

        /// <summary>
        /// Login failed.
        /// <para> param1(uint16): retcode. // server_errors</para>
        /// <para> param2(bytes): serverdatas. // server_data</para>
        /// </summary>
        public const string onLoginFailed = "onLoginFailed";

        /// <summary>
        /// Login to baseapp.
        /// </summary>
        public const string onLoginBaseapp = "onLoginBaseapp";

        /// <summary>
        /// Login baseapp failed.
        /// <para> param1(uint16): retcode. // server_errors</para>
        /// </summary>
        public const string onLoginBaseappFailed = "onLoginBaseappFailed";

        /// <summary>
        /// Relogin to baseapp.
        /// </summary>
        public const string onReloginBaseapp = "onReloginBaseapp";

        /// <summary>
        /// Relogin baseapp success.
        /// </summary>
        public const string onReloginBaseappSuccessfully = "onReloginBaseappSuccessfully";

        /// <summary>
        /// Relogin baseapp failed.
        /// <para> param1(uint16): retcode. // server_errors</para>
        /// </summary>
        public const string onReloginBaseappFailed = "onReloginBaseappFailed";

        // ------------------------------------实体cell相关事件------------------------------------

        /// <summary>
        /// Entity enter the client-world.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string onEnterWorld = "onEnterWorld";

        /// <summary>
        /// Entity leave the client-world.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string onLeaveWorld = "onLeaveWorld";

        /// <summary>
        /// Player enter the new space.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string onEnterSpace = "onEnterSpace";

        /// <summary>
        /// Player leave the space.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string onLeaveSpace = "onLeaveSpace";

        /// <summary>
        /// Sets the current position of the entity.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string set_position = "set_position";

        /// <summary>
        /// Sets the current direction of the entity.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string set_direction = "set_direction";

        /// <summary>
        /// The entity position is updated, you can smooth the moving entity to new location.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string updatePosition = "updatePosition";

        /// <summary>
        /// The current space is specified by the geometry mapping.
        /// Popular said is to load the specified Map Resources.
        /// <para> param1(string): resPath</para>
        /// </summary>
        public const string addSpaceGeometryMapping = "addSpaceGeometryMapping";

        /// <summary>
        /// Server spaceData set data.
        /// <para> param1(int32): spaceID</para>
        /// <para> param2(string): key</para>
        /// <para> param3(string): value</para>
        /// </summary>
        public const string onSetSpaceData = "onSetSpaceData";

        /// <summary>
        /// Start downloading data.
        /// <para> param1(int32): rspaceID</para>
        /// <para> param2(string): key</para>
        /// </summary>
        public const string onDelSpaceData = "onDelSpaceData";

        /// <summary>
        /// Triggered when the entity is controlled or out of control.
        /// <para> param1: Entity</para>
        /// <para> param2(bool): isControlled</para>
        /// </summary>
        public const string onControlled = "onControlled";

        /// <summary>
        /// Lose controlled entity.
        /// <para> param1: Entity</para>
        /// </summary>
        public const string onLoseControlledEntity = "onLoseControlledEntity";

        // ------------------------------------数据下载相关------------------------------------
        /// <summary>
        /// Start downloading data.
        /// <para> param1(uint16): resouce id</para>
        /// <para> param2(uint32): data size</para>
        /// <para> param3(string): description</para>
        /// </summary>
        public const string onStreamDataStarted = "onStreamDataStarted";

        /// <summary>
        /// Receive data.
        /// <para> param1(uint16): resouce id</para>
        /// <para> param2(bytes): datas</para>
        /// </summary>
        public const string onStreamDataRecv = "onStreamDataRecv";

        /// <summary>
        /// The downloaded data is completed.
        /// <para> param1(uint16): resouce id</para>
        /// </summary>
        public const string onStreamDataCompleted = "onStreamDataCompleted";
    };

    /// <summary>
    /// KBE-Plugin fire-in events(Unity => KBE):
    /// </summary>
    public class EventInTypes
    {
        /// <summary>
        /// Create new account.
        /// <para> param1(string): accountName</para>
        /// <para> param2(string): password</para>
        /// <para> param3(bytes): datas // Datas by user defined. Data will be recorded into the KBE account database, you can access the datas through the script layer. If you use third-party account system, datas will be submitted to the third-party system.</para>
        /// </summary>
        public const string createAccount = "createAccount";

        /// <summary>
        /// Login to server.
        /// <para> param1(string): accountName</para>
        /// <para> param2(string): password</para>
        /// <para> param3(bytes): datas // Datas by user defined. Data will be recorded into the KBE account database, you can access the datas through the script layer. If you use third-party account system, datas will be submitted to the third-party system.</para>
        /// </summary>
        public const string login = "login";

        /// <summary>
        /// Logout to baseapp, called when exiting the client.
        /// </summary>
        public const string logout = "logout";

        /// <summary>
        /// Relogin to baseapp.
        /// </summary>
        public const string reloginBaseapp = "reloginBaseapp";

        /// <summary>
        /// Reset password.
        /// <para> param1(string): accountName</para>
        /// </summary>
        public const string resetPassword = "resetPassword";

        /// <summary>
        /// Request to set up a new password for the account. Note: account must be online.
        /// <para> param1(string): old_password</para>
        /// <para> param2(string): new_password</para>
        /// </summary>
        public const string newPassword = "newPassword";

        /// <summary>
        /// Request server binding account Email.
        /// <para> param1(string): emailAddress</para>
        /// </summary>
        public const string bindAccountEmail = "bindAccountEmail";
    };

    /// <summary>
    /// 事件模块。
    /// <para>KBEngine 插件层与 Unity3D 表现层通过事件进行解耦交互。</para>
    /// <para>事件分为 out 和 in 两个方向：</para>
    /// <para>out = KBE 插件层 -> Unity 表现层。常用于网络层、实体层收到服务器消息后通知 UI 或业务逻辑。</para>
    /// <para>in = Unity 表现层 -> KBE 插件层。常用于 UI 点击登录、创建账号、登出等操作后通知插件层组包并发送到服务器。</para>
    /// <para>暂停语义：pause/resume 只控制 out 事件的即时执行和 out 队列消费；processInEvents 不受 pause 控制。</para>
    /// <para>队列语义：fire 时会按当前监听者生成 EventObj 快照，放入 fired 队列；process 时先搬到 doing 队列再执行，避免处理过程中再次 fire 导致重入顺序混乱。</para>
	/// </summary>
    public class Event
    {
        /// <summary>
        /// 单个事件监听项。
        /// <para>obj 是回调所属对象。</para>
        /// <para>funcname 是回调函数名，用于注销和日志输出。</para>
        /// <para>method 是通过反射找到的 MethodInfo，真正触发事件时会 Invoke 它。</para>
        /// </summary>
		public struct Pair
		{
			public object obj;
			public string funcname;
			public System.Reflection.MethodInfo method;
        };
		
        /// <summary>
        /// 已触发但尚未执行的事件对象。
        /// <para>info 保存某一个具体监听者。</para>
        /// <para>eventname 和 args 保存触发时的事件名与参数。</para>
        /// <para>这里按监听者入队，而不是只按事件名入队；这样可以固定触发瞬间的监听者快照。</para>
        /// </summary>
		public struct EventObj
		{
			public Pair info;
            public string eventname;
            public object[] args;
		};
		
        /// <summary>
        /// out 事件注册表。
        /// <para>key 是事件名，value 是监听该事件的所有回调。</para>
        /// <para>out = KBE 插件层 -> Unity 表现层。</para>
        /// </summary>
    	static Dictionary<string, List<Pair>> events_out = new Dictionary<string, List<Pair>>();
		
        /// <summary>
        /// out 事件是否尽量立即执行。
        /// <para>true：fireOut 在未 pause 时直接调用监听者。</para>
        /// <para>false：fireOut 总是进入 firedEvents_out，等待 processOutEvents 统一处理。</para>
        /// <para>注意：即使为 true，只要当前处于 pause 状态，out 事件仍会入队。</para>
        /// </summary>
		public static bool outEventsImmediately = true;

        /// <summary>
        /// out 事件待处理队列。fireOut 被暂停或要求延迟执行时，事件先进入这里。
        /// </summary>
		static LinkedList<EventObj> firedEvents_out = new LinkedList<EventObj>();

        /// <summary>
        /// out 事件正在处理队列。
        /// <para>processOutEvents 会先把 firedEvents_out 搬到 doingEvents_out，再逐个执行。</para>
        /// <para>执行过程中再次 fireOut 的事件会留在 firedEvents_out，下一轮再执行。</para>
        /// </summary>
		static LinkedList<EventObj> doingEvents_out = new LinkedList<EventObj>();
		
        /// <summary>
        /// in 事件注册表。
        /// <para>key 是事件名，value 是监听该事件的所有回调。</para>
        /// <para>in = Unity 表现层 -> KBE 插件层。</para>
        /// </summary>
    	static Dictionary<string, List<Pair>> events_in = new Dictionary<string, List<Pair>>();
		
        /// <summary>
        /// in 事件待处理队列。fireIn 触发的事件会进入这里，等待 processInEvents 消费。
        /// </summary>
		static LinkedList<EventObj> firedEvents_in = new LinkedList<EventObj>();

        /// <summary>
        /// in 事件正在处理队列。作用同 doingEvents_out。
        /// <para>processInEvents 不受 pause 控制。</para>
        /// </summary>
		static LinkedList<EventObj> doingEvents_in = new LinkedList<EventObj>();

        /// <summary>
        /// 暂停标记。
        /// <para>只控制 out 事件的即时执行和 out 队列消费。</para>
        /// <para>fireIn 与 processInEvents 不受该标记控制。</para>
        /// </summary>
		static bool _isPauseOut = false;

		public Event()
		{
		}
		
        /// <summary>
        /// 清理所有事件注册和已触发队列。
        /// <para>会清空 out/in 注册表，并调用 clearFiredEvents 清空所有待处理事件。</para>
        /// </summary>
		public static void clear()
		{
			events_out.Clear();
			events_in.Clear();
			clearFiredEvents();
		}

        /// <summary>
        /// 清理所有已触发但尚未处理完成的事件，并解除 pause 状态。
        /// <para>只清理 fired/doing 队列，不清理 out/in 注册表。</para>
        /// </summary>
		public static void clearFiredEvents()
		{
			monitor_Enter(events_out);
			firedEvents_out.Clear();
			monitor_Exit(events_out);
			
			doingEvents_out.Clear();
			
			monitor_Enter(events_in);
			firedEvents_in.Clear();
			monitor_Exit(events_in);
			
			doingEvents_in.Clear();
			
			_isPauseOut = false;
		}
		
        /// <summary>
        /// 暂停 out 事件处理。
        /// <para>pause 后，fireOut 不再立即调用监听者，而是把事件放入 out 队列。</para>
        /// <para>fireAll 的 out 部分同样只入队。</para>
        /// <para>processOutEvents 不会继续消费 out 队列。</para>
        /// <para>processInEvents 不受 pause 影响。</para>
        /// </summary>
		public static void pause()
		{
			_isPauseOut = true;
		}

        /// <summary>
        /// 恢复 out 事件处理。
        /// <para>与 pause 对应：解除暂停后会立即调用 processOutEvents 处理 out 队列。</para>
        /// <para>注意：resume 只主动处理 out 队列，不主动处理 in 队列。</para>
        /// </summary>
		public static void resume()
		{
			_isPauseOut = false;
			processOutEvents();
		}

        /// <summary>
        /// 当前是否处于 out 事件暂停状态。
        /// </summary>
		public static bool isPause()
		{
			return _isPauseOut;
		}

        /// <summary>
        /// 进入临界区。
        /// <para>KBEngineApp.app 为空时跳过锁，保持旧模板在未初始化阶段的行为。</para>
        /// </summary>
		public static void monitor_Enter(object obj)
		{
			if(KBEngineApp.app == null)
				return;
			
			Monitor.Enter(obj);
		}

        /// <summary>
        /// 退出临界区。
        /// <para>必须与 monitor_Enter 成对使用。</para>
        /// </summary>
		public static void monitor_Exit(object obj)
		{
			if(KBEngineApp.app == null)
				return;
			
			Monitor.Exit(obj);
		}
		
        /// <summary>
        /// 判断指定 out 事件是否存在至少一个注册项。
        /// </summary>
		public static bool hasRegisterOut(string eventname)
		{
			return _hasRegister(events_out, eventname);
		}

        /// <summary>
        /// 判断指定 in 事件是否存在至少一个注册项。
        /// </summary>
		public static bool hasRegisterIn(string eventname)
		{
			return _hasRegister(events_in, eventname);
		}
		
        /// <summary>
        /// 通用注册查询逻辑。
        /// </summary>
		private static bool _hasRegister(Dictionary<string, List<Pair>> events, string eventname)
		{
			bool has = false;
			
			monitor_Enter(events);
			has = events.ContainsKey(eventname);
			monitor_Exit(events);
			
			return has;
		}

        /// <summary>
		///	注册监听由kbe插件抛出的事件。(out = kbe->render)
		///	通常由渲染表现层来注册, 例如：监听角色血量属性的变化， 如果UI层注册这个事件，
		///	事件触发后就可以根据事件所附带的当前血量值来改变角色头顶的血条值。
        /// </summary>
        public static bool registerOut(string eventname, object obj, string funcname)
		{
			return register(events_out, eventname, obj, funcname);
		}

        /// <summary>
		///	注册监听由kbe插件抛出的事件。(out = kbe->render)
		///	通常由渲染表现层来注册, 例如：监听角色血量属性的变化， 如果UI层注册这个事件，
		///	事件触发后就可以根据事件所附带的当前血量值来改变角色头顶的血条值。
        /// </summary>
        public static bool registerOut(string eventname, Action handler)
        {
            return registerOut(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
		///	注册监听由kbe插件抛出的事件。(out = kbe->render)
		///	通常由渲染表现层来注册, 例如：监听角色血量属性的变化， 如果UI层注册这个事件，
		///	事件触发后就可以根据事件所附带的当前血量值来改变角色头顶的血条值。
        /// </summary>
        public static bool registerOut<T1>(string eventname, Action<T1> handler)
        {
            return registerOut(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
		///	注册监听由kbe插件抛出的事件。(out = kbe->render)
		///	通常由渲染表现层来注册, 例如：监听角色血量属性的变化， 如果UI层注册这个事件，
		///	事件触发后就可以根据事件所附带的当前血量值来改变角色头顶的血条值。
        /// </summary>
        public static bool registerOut<T1, T2>(string eventname, Action<T1, T2> handler)
        {
            return registerOut(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
		///	注册监听由kbe插件抛出的事件。(out = kbe->render)
		///	通常由渲染表现层来注册, 例如：监听角色血量属性的变化， 如果UI层注册这个事件，
		///	事件触发后就可以根据事件所附带的当前血量值来改变角色头顶的血条值。
        /// </summary>
        public static bool registerOut<T1, T2, T3>(string eventname, Action<T1, T2, T3> handler)
        {
            return registerOut(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
		///	注册监听由kbe插件抛出的事件。(out = kbe->render)
		///	通常由渲染表现层来注册, 例如：监听角色血量属性的变化， 如果UI层注册这个事件，
		///	事件触发后就可以根据事件所附带的当前血量值来改变角色头顶的血条值。
        /// </summary>
        public static bool registerOut<T1, T2, T3, T4>(string eventname, Action<T1, T2, T3, T4> handler)
        {
            return registerOut(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 注册监听由渲染表现层抛出的事件(in = render->kbe)
		/// 通常由kbe插件层来注册， 例如：UI层点击登录， 此时需要触发一个事件给kbe插件层进行与服务端交互的处理。
        /// </summary>
        public static bool registerIn(string eventname, object obj, string funcname)
		{
			return register(events_in, eventname, obj, funcname);
		}

        /// <summary>
        /// 注册监听由渲染表现层抛出的事件(in = render->kbe)
		/// 通常由kbe插件层来注册， 例如：UI层点击登录， 此时需要触发一个事件给kbe插件层进行与服务端交互的处理。
        /// </summary>
        public static bool registerIn(string eventname, Action handler)
        {
            return registerIn(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 注册监听由渲染表现层抛出的事件(in = render->kbe)
		/// 通常由kbe插件层来注册， 例如：UI层点击登录， 此时需要触发一个事件给kbe插件层进行与服务端交互的处理。
        /// </summary>
        public static bool registerIn<T1>(string eventname, Action<T1> handler)
        {
            return registerIn(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 注册监听由渲染表现层抛出的事件(in = render->kbe)
		/// 通常由kbe插件层来注册， 例如：UI层点击登录， 此时需要触发一个事件给kbe插件层进行与服务端交互的处理。
        /// </summary>
        public static bool registerIn<T1, T2>(string eventname, Action<T1, T2> handler)
        {
            return registerIn(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 注册监听由渲染表现层抛出的事件(in = render->kbe)
		/// 通常由kbe插件层来注册， 例如：UI层点击登录， 此时需要触发一个事件给kbe插件层进行与服务端交互的处理。
        /// </summary>
        public static bool registerIn<T1, T2, T3>(string eventname, Action<T1, T2, T3> handler)
        {
            return registerIn(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 注册监听由渲染表现层抛出的事件(in = render->kbe)
		/// 通常由kbe插件层来注册， 例如：UI层点击登录， 此时需要触发一个事件给kbe插件层进行与服务端交互的处理。
        /// </summary>
        public static bool registerIn<T1, T2, T3, T4>(string eventname, Action<T1, T2, T3, T4> handler)
        {
            return registerIn(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 通用注册逻辑。
        /// <para>注册前会先 deregister 一次，避免同一个对象和同一个函数重复注册。</para>
        /// <para>通过 obj.GetType().GetMethod(funcname) 查找回调方法，找不到时注册失败。</para>
        /// </summary>
        private static bool register(Dictionary<string, List<Pair>> events, string eventname, object obj, string funcname)
		{
			deregister(events, eventname, obj, funcname);
			List<Pair> lst = null;
			
			Pair pair = new Pair();
			pair.obj = obj;
			pair.funcname = funcname;
			pair.method = obj.GetType().GetMethod(funcname);
			
			if(pair.method == null)
			{
				KBELog.ERROR_MSG("Event::register: " + obj + "not found method[" + funcname + "]");
				return false;
			}
			
			monitor_Enter(events);
			if(!events.TryGetValue(eventname, out lst))
			{
				lst = new List<Pair>();
				lst.Add(pair);
				//KBELog.DEBUG_MSG("Event::register: event(" + eventname + ")!");
				events.Add(eventname, lst);
				monitor_Exit(events);
				return true;
			}
			
			//KBELog.DEBUG_MSG("Event::register: event(" + eventname + ")!");
			lst.Add(pair);
			monitor_Exit(events);
			return true;
		}

        /// <summary>
        /// 注销 out 事件监听。
        /// <para>除了从 out 注册表中移除，也会从 out 的 fired 队列里移除已经排队的同一回调。</para>
        /// <para>这样对象销毁或取消监听后，不会再收到已经延迟入队的 out 事件。</para>
        /// </summary>
		public static bool deregisterOut(string eventname, object obj, string funcname)
		{
            removeFiredEventOut(obj, eventname, funcname);
            return deregister(events_out, eventname, obj, funcname);
		}
  
        /// <summary>
        /// 注销 out 事件监听。
        /// <para>Action 重载会使用 handler.Target 和 handler.Method.Name 定位注册项。</para>
        /// </summary>
        public static bool deregisterOut(string eventname, Action handler)
        {
            return deregisterOut(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 注销 in 事件监听。
        /// <para>除了从 in 注册表中移除，也会从 in 的 fired 队列里移除已经排队的同一回调。</para>
        /// </summary>
        public static bool deregisterIn(string eventname, object obj, string funcname)
		{
            removeFiredEventIn(obj, eventname, funcname);
            return deregister(events_in, eventname, obj, funcname);
		}

        /// <summary>
        /// 注销 in 事件监听。
        /// <para>Action 重载会使用 handler.Target 和 handler.Method.Name 定位注册项。</para>
        /// </summary>
        public static bool deregisterIn(string eventname, Action handler)
        {
            return deregisterIn(eventname, handler.Target, handler.Method.Name);
        }

        /// <summary>
        /// 通用注销逻辑。
        /// <para>只移除匹配 eventname + obj + funcname 的第一个监听项。</para>
        /// </summary>
        private static bool deregister(Dictionary<string, List<Pair>> events, string eventname, object obj, string funcname)
		{
			monitor_Enter(events);
			List<Pair> lst = null;
			
			if(!events.TryGetValue(eventname, out lst))
			{
				monitor_Exit(events);
				return false;
			}
			
			for(int i=0; i<lst.Count; i++)
			{
				if(obj == lst[i].obj && lst[i].funcname == funcname)
				{
					//KBELog.DEBUG_MSG("Event::deregister: event(" + eventname + ":" + funcname + ")!");
					lst.RemoveAt(i);
					monitor_Exit(events);
					return true;
				}
			}
			
			monitor_Exit(events);
			return false;
		}

        /// <summary>
        /// 注销某个对象注册的所有 out 事件。
        /// <para>会同时清理 out fired 队列中属于该对象的待执行事件。</para>
        /// </summary>
		public static bool deregisterOut(object obj)
		{
            removeAllFiredEventOut(obj);
			return deregister(events_out, obj);
		}

        /// <summary>
        /// 注销某个对象注册的所有 in 事件。
        /// <para>会同时清理 in fired 队列中属于该对象的待执行事件。</para>
        /// </summary>
		public static bool deregisterIn(object obj)
		{
            removeAllFiredEventIn(obj);
			return deregister(events_in, obj);
		}
		
        /// <summary>
        /// 从指定注册表中移除某个对象的所有监听。
        /// <para>从后向前遍历每个事件列表，避免删除元素后数组下标变化导致漏删。</para>
        /// </summary>
		private static bool deregister(Dictionary<string, List<Pair>> events, object obj)
		{
			monitor_Enter(events);
			
			var iter = events.GetEnumerator();
			while (iter.MoveNext())
			{
				List<Pair> lst = iter.Current.Value;
				// 从后往前遍历，以避免中途删除的问题
				for (int i = lst.Count - 1; i >= 0; i--)
				{
					if (obj == lst[i].obj)
					{
						//KBELog.DEBUG_MSG("Event::deregister: event(" + e.Key + ":" + lst[i].funcname + ")!");
						lst.RemoveAt(i);
					}
				}
			}
			
			monitor_Exit(events);
			return true;
		}

        /// <summary>
        /// kbe插件触发事件(out = kbe->render)
		/// 通常由渲染表现层来注册, 例如：监听角色血量属性的变化， 如果UI层注册这个事件，
		/// 事件触发后就可以根据事件所附带的当前血量值来改变角色头顶的血条值。
		/// </summary>
		public static void fireOut(string eventname, params object[] args)
		{
			fire_(events_out, firedEvents_out, eventname, args, outEventsImmediately);
		}

        /// <summary>
        /// 渲染表现层抛出事件(in = render->kbe)
		/// 通常由kbe插件层来注册， 例如：UI层点击登录， 此时需要触发一个事件给kbe插件层进行与服务端交互的处理。
        /// <para>fireIn 永远不会立即调用监听者，而是进入 in 队列，等待 processInEvents 处理。</para>
		/// </summary>
		public static void fireIn(string eventname, params object[] args)
		{
			fire_(events_in, firedEvents_in, eventname, args, false);
		}

        /// <summary>
        /// 触发kbe插件和渲染表现层都能够收到的事件
        /// <para>in 部分进入 in 队列，等待 processInEvents。</para>
        /// <para>out 部分进入 out 队列，等待 processOutEvents。</para>
        /// <para>fireAll 不做即时调用，便于统一控制触发时机。</para>
        /// </summary>
        public static void fireAll(string eventname, params object[] args)
		{
			fire_(events_in, firedEvents_in, eventname, args, false);
			fire_(events_out, firedEvents_out, eventname, args, false);
		}
		
        /// <summary>
        /// 通用触发逻辑。
        /// <para>eventsImmediately 为 true 且当前未 pause 时，立即调用当前注册表中的所有监听者。</para>
        /// <para>否则把当前监听者快照放入 firedEvents。</para>
        /// <para>入队时保存 Pair，而不是只保存 eventname 后再查注册表，是为了固定事件触发瞬间的监听者集合。</para>
        /// </summary>
		private static void fire_(Dictionary<string, List<Pair>> events, LinkedList<EventObj> firedEvents, string eventname, object[] args, bool eventsImmediately)
		{
			monitor_Enter(events);
			List<Pair> lst = null;
			
			if(!events.TryGetValue(eventname, out lst))
			{
				//if(events == events_in)
				//	KBELog.WARNING_MSG("Event::fireIn: event(" + eventname + ") not found!");
				//else
				//	KBELog.WARNING_MSG("Event::fireOut: event(" + eventname + ") not found!");
				
				monitor_Exit(events);
				return;
			}
			
			if(eventsImmediately && !_isPauseOut)
			{
				for(int i=0; i<lst.Count; i++)
				{
					Pair info = lst[i];

					try
					{
						info.method.Invoke (info.obj, args);
					}
					catch (Exception e)
					{
						KBELog.ERROR_MSG("Event::fire_: event=" + info.method.DeclaringType.FullName + "::" + info.funcname + "\n" + e.ToString());
					}
				}
			}
			else
			{
				for(int i=0; i<lst.Count; i++)
				{
					EventObj eobj = new EventObj();
					eobj.info = lst[i];
                    eobj.eventname = eventname;
                    eobj.args = args;
					firedEvents.AddLast(eobj);
				}
			}

			monitor_Exit(events);
		}
		
        /// <summary>
        /// 处理 out 事件队列。
        /// <para>会先把 firedEvents_out 搬到 doingEvents_out，再执行 doingEvents_out。</para>
        /// <para>这样处理过程中再次 fireOut 的事件会留在 firedEvents_out，下一轮再执行。</para>
        /// <para>如果当前处于 pause 状态，不会消费 doingEvents_out。</para>
        /// </summary>
		public static void processOutEvents()
		{
			monitor_Enter(events_out);

			if(firedEvents_out.Count > 0)
			{
				var iter = firedEvents_out.GetEnumerator();
				while (iter.MoveNext())
				{
					doingEvents_out.AddLast(iter.Current);
				}

				firedEvents_out.Clear();
			}

			monitor_Exit(events_out);

			while (doingEvents_out.Count > 0 && !_isPauseOut) 
			{

				EventObj eobj = doingEvents_out.First.Value;

				//Debug.Log("processOutEvents:" + eobj.info.funcname + "(" + eobj.info + ")");
				//foreach(object v in eobj.args)
				//{
				//	Debug.Log("processOutEvents:args=" + v);
				//}
				try
				{
					eobj.info.method.Invoke (eobj.info.obj, eobj.args);
				}
	            catch (Exception e)
	            {
	            	KBELog.ERROR_MSG("Event::processOutEvents: event=" + eobj.info.method.DeclaringType.FullName + "::" + eobj.info.funcname + "\n" + e.ToString());
	            }
            
				if(doingEvents_out.Count > 0)
					doingEvents_out.RemoveFirst();
			}
		}
		
        /// <summary>
        /// 处理 in 事件队列。
        /// <para>会先把 firedEvents_in 搬到 doingEvents_in，再执行 doingEvents_in。</para>
        /// <para>与 out 不同，processInEvents 不受 pause 控制。</para>
        /// <para>处理过程中再次 fireIn 的事件会留在 firedEvents_in，下一轮再执行。</para>
        /// </summary>
		public static void processInEvents()
		{
			monitor_Enter(events_in);

			if(firedEvents_in.Count > 0)
			{
				var iter = firedEvents_in.GetEnumerator();
				while (iter.MoveNext())
				{
					doingEvents_in.AddLast(iter.Current);
				}

				firedEvents_in.Clear();
			}

			monitor_Exit(events_in);

			while (doingEvents_in.Count > 0) 
			{
				
				EventObj eobj = doingEvents_in.First.Value;
				
				//Debug.Log("processInEvents:" + eobj.info.funcname + "(" + eobj.info + ")");
				//foreach(object v in eobj.args)
				//{
				//	Debug.Log("processInEvents:args=" + v);
				//}
				try
				{
					eobj.info.method.Invoke (eobj.info.obj, eobj.args);
				}
	            catch (Exception e)
	            {
	            	KBELog.ERROR_MSG("Event::processInEvents: event=" + eobj.info.method.DeclaringType.FullName + "::" + eobj.info.funcname + "\n" + e.ToString());
	            }
	            
				if(doingEvents_in.Count > 0)
					doingEvents_in.RemoveFirst();
			}
		}

        /// <summary>
        /// 清理某个对象在 in fired 队列中的所有待执行事件。
        /// </summary>
        public static void removeAllFiredEventIn(object obj)
        {
            removeFiredEvent(firedEvents_in, obj);
        }

        /// <summary>
        /// 清理某个对象在 out fired 队列中的所有待执行事件。
        /// </summary>
        public static void removeAllFiredEventOut(object obj)
        {
            removeFiredEvent(firedEvents_out, obj);
        }

        /// <summary>
        /// 清理某个对象在 in fired 队列中的指定待执行事件。
        /// </summary>
        public static void removeFiredEventIn(object obj, string eventname, string funcname)
        {
            removeFiredEvent(firedEvents_in, obj, eventname, funcname);
        }

        /// <summary>
        /// 清理某个对象在 out fired 队列中的指定待执行事件。
        /// </summary>
        public static void removeFiredEventOut(object obj, string eventname, string funcname)
        {
            removeFiredEvent(firedEvents_out, obj, eventname, funcname);
        }

        /// <summary>
        /// 从指定 fired 队列中移除待执行事件。
        /// <para>eventname 和 funcname 为空时，会移除 obj 对应的全部事件。</para>
        /// <para>eventname 和 funcname 非空时，只移除事件名、函数名和对象都匹配的事件。</para>
        /// <para>使用循环查找并删除，直到队列中再也没有匹配项。</para>
        /// </summary>
        public static void removeFiredEvent(LinkedList<EventObj> firedEvents, object obj, string eventname="", string funcname="")
        {
            monitor_Enter(firedEvents);
           
            while(true)
            {
                bool found = false;
                foreach(EventObj eobj in firedEvents)
                {
                    if( ((eventname == "" && funcname == "") || (eventname == eobj.eventname && funcname == eobj.info.funcname))
                        && eobj.info.obj == obj)
                    {
                        firedEvents.Remove(eobj);
                        found = true;
                        break;
                    }
                }

                if (!found)
                    break;
            }
           
            monitor_Exit(firedEvents);
        }
    
    }
} 
