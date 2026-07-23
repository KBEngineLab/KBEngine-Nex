
import KBELog from "./KBELog";

/**
 * 单个事件监听项。
 *
 * m_object 是回调执行时绑定的 this 对象。
 * m_cbFunction 是实际调用的函数引用。
 *
 * 注意：这里直接保存 Function，而不是方法名字符串。
 * TypeScript/JavaScript 中函数可以作为一等对象保存，调用时通过 apply
 * 把 m_object 重新绑定为 this。
 */
class EventInfo
{
    m_object: object;
    m_cbFunction: Function;

    constructor(p_object: object, cbFunction: Function)
    {
        this.m_object = p_object;
        this.m_cbFunction = cbFunction;
    }
}

/**
 * 事件注册表。
 *
 * key 是事件名，例如 "onDisconnected"、"login"。
 * value 是监听该事件的所有回调。
 */
type EventMap = { [eventName: string]: Array<EventInfo> };

/**
 * 已触发但尚未真正执行的事件。
 *
 * info 保存某一个具体监听者。
 * eventName 和 params 保存触发时的事件名与参数。
 *
 * 这里是“按监听者”入队，而不是只按事件名入队。
 * 触发事件时会把当前已经注册的监听者快照放入队列，
 * 后续再注册的新监听者不会收到这次已经入队的事件。
 */
type FiredEvent = { info: EventInfo, eventName: string, params: any[] };


/**
 * KBE 事件中心。
 *
 * 事件分为两类：
 *
 * out: KBE 插件层 -> 表现层/业务层。
 * 常见用法是网络层、实体层收到服务器消息后，通知 UI 或游戏逻辑刷新状态。
 *
 * in: 表现层/业务层 -> KBE 插件层。
 * 常见用法是 UI 点击登录、创建账号等操作，抛给 KBE 插件层去组包并发送给服务器。
 *
 * 兼容旧接口：
 * Register / Deregister / Fire 仍然保留，但内部统一走 out 事件逻辑，
 * 等价于 registerOut / deregisterOut / fireOut。
 *
 * 暂停语义：
 * Pause 后，fireOut 和 fireAll 的 out 部分不会立即执行，会进入 out 队列。
 * processOutEvents 会在未暂停时处理 out 队列。
 * fireIn 和 processInEvents 不受 Pause 影响。
 */
export default class KBEEvent
{
    // out 事件注册表，保存 KBE -> TS 方向的监听者。
    private static _events_out: EventMap = {};

    // out 事件待处理队列。fireOut 被暂停或要求延迟执行时，事件先进入这里。
    private static _firedEvents_out: Array<FiredEvent> = [];

    // out 事件正在处理队列。processOutEvents 会先把 fired 搬到 doing，再逐个执行。
    // 这样执行过程中再次 fireOut 的事件会留到下一轮，避免重入导致顺序混乱。
    private static _doingEvents_out: Array<FiredEvent> = [];

    // in 事件注册表，保存 TS -> KBE 方向的监听者。
    private static _events_in: EventMap = {};

    // in 事件待处理队列。fireIn 触发的事件会进入这里，等待 processInEvents 消费。
    private static _firedEvents_in: Array<FiredEvent> = [];

    // in 事件正在处理队列。作用同 _doingEvents_out。
    private static _doingEvents_in: Array<FiredEvent> = [];

    /**
     * out 事件是否尽量立即执行。
     *
     * true:
     * fireOut 在未暂停时会直接调用监听者。
     *
     * false:
     * fireOut 总是先入队，等待 processOutEvents 统一处理。
     *
     * 注意：即使为 true，只要处于 Pause 状态，out 事件仍然会入队。
     */
    static outEventsImmediately = true;

    // 暂停标记。当前只用于控制 out 事件的即时执行和 out 队列消费。
    private static _isPause = false;

    // 防止 processOutEvents 被重入调用。
    private static _processOutEventsStarted = false;

    // 防止 processInEvents 被重入调用。
    private static _processInEventsStarted = false;

    /**
     * 暂停事件处理。
     *
     * Pause 后：
     * fireOut 不再立即调用监听者，而是把事件放入 out 队列。
     * fireAll 的 out 部分同样只入队。
     * processOutEvents 不会继续消费 out 队列。
     * processInEvents 不受 Pause 影响。
     */
    static Pause(): void
    {
        this._isPause = true;
    }

    /**
     * 恢复 out 事件处理。
     *
     * Resume 只主动处理 out 队列。
     * in 队列需要由调用方显式调用 processInEvents 处理。
     */
    static Resume(): void
    {
        this._isPause = false;
        this.processOutEvents();
    }

    /**
     * 当前是否处于暂停状态。
     */
    static isPaused(): boolean
    {
        return this._isPause;
    }

    /**
     * 旧接口：注册 out 事件监听。
     *
     * 为兼容已有 TS 模板和用户代码保留。
     * 内部等价于 registerOut。
     */
    static Register(eventName: string, p_object: object, cbFunction: Function): void
    {
        this.registerOut(eventName, p_object, cbFunction);
    }

    /**
     * 注册 out 事件监听。
     *
     * out = KBE 插件层 -> TS 表现层/业务层。
     * 例如实体进入世界、属性变化、连接断开等服务器侧消息通知。
     */
    static registerOut(eventName: string, p_object: object, cbFunction: Function): void
    {
        this.register(this._events_out, eventName, p_object, cbFunction);
    }

    /**
     * 注册 in 事件监听。
     *
     * in = TS 表现层/业务层 -> KBE 插件层。
     * 例如 UI 层触发 login/createAccount/logout，由 KBEngineApp 注册后处理。
     */
    static registerIn(eventName: string, p_object: object, cbFunction: Function): void
    {
        this.register(this._events_in, eventName, p_object, cbFunction);
    }

    /**
     * 通用注册逻辑。
     *
     * 注册前会先 deregister 一次，避免同一个对象和同一个函数重复注册。
     */
    private static register(events: EventMap, eventName: string, p_object: object, cbFunction: Function): void
    {
        this.deregister(events, eventName, p_object, cbFunction);

        let eventList: Array<EventInfo> = events[eventName];
        if(eventList === undefined)
        {
            eventList = [];
            events[eventName] = eventList;
        }

        eventList.push(new EventInfo(p_object, cbFunction));
    }

    /**
     * 旧接口：注销 out 事件监听。
     *
     * 为兼容已有代码保留。
     * 内部等价于 deregisterOut。
     */
    static Deregister(eventName: string, p_object: object|null, cbFunction: Function): void
    {
        this.deregisterOut(eventName, p_object, cbFunction);
    }

    /**
     * 注销 out 事件监听。
     *
     * 除了从注册表中移除，也会从 out 的 fired/doing 队列里移除已经排队的同一回调。
     * 这样对象销毁或取消监听后，不会再收到已经延迟入队的 out 事件。
     */
    static deregisterOut(eventName: string, p_object: object|null, cbFunction: Function): void
    {
        this.removeFiredEvent(this._firedEvents_out, p_object, eventName, cbFunction);
        this.removeFiredEvent(this._doingEvents_out, p_object, eventName, cbFunction);
        this.deregister(this._events_out, eventName, p_object, cbFunction);
    }

    /**
     * 注销 in 事件监听。
     *
     * 除了从注册表中移除，也会从 in 的 fired/doing 队列里移除已经排队的同一回调。
     */
    static deregisterIn(eventName: string, p_object: object|null, cbFunction: Function): void
    {
        this.removeFiredEvent(this._firedEvents_in, p_object, eventName, cbFunction);
        this.removeFiredEvent(this._doingEvents_in, p_object, eventName, cbFunction);
        this.deregister(this._events_in, eventName, p_object, cbFunction);
    }

    /**
     * 通用注销逻辑。
     *
     * 只移除匹配 eventName + p_object + cbFunction 的第一个监听项。
     */
    private static deregister(events: EventMap, eventName: string, p_object: object|null, cbFunction: Function): void
    {
        let eventList: Array<EventInfo> = events[eventName];
        if(eventList === undefined)
        {
            return;
        }

        for (let item of eventList)
        {
            if(p_object === item.m_object && item.m_cbFunction === cbFunction)
            {
                let index: number = eventList.indexOf(item);
                eventList.splice(index, 1);
                return;
            }
        }
    }

    /**
     * 旧接口：触发 out 事件。
     *
     * 为兼容已有 TS 模板保留。
     * 内部等价于 fireOut。
     */
    static Fire(eventName: string, ...params: any[]): void
    {
        this.fireOut(eventName, ...params);
    }

    /**
     * 触发 out 事件。
     *
     * outEventsImmediately 为 true 且当前未暂停时，立即调用所有 out 监听者。
     * 否则把本次触发对应的监听者快照放入 out 队列，等待 processOutEvents。
     */
    static fireOut(eventName: string, ...params: any[]): void
    {
        this.fire(this._events_out, this._firedEvents_out, eventName, params, this.outEventsImmediately && !this._isPause);
    }

    /**
     * 触发 in 事件。
     *
     * in 事件始终入队，不会在 fireIn 调用点直接执行。
     * 调用方需要在合适的时机调用 processInEvents。
     */
    static fireIn(eventName: string, ...params: any[]): void
    {
        this.fire(this._events_in, this._firedEvents_in, eventName, params, false);
    }

    /**
     * 同时触发 in 和 out 事件。
     *
     * in 部分：进入 in 队列，等待 processInEvents。
     * out 部分：进入 out 队列，等待 processOutEvents。
     *
     * 注意：fireAll 不做即时调用。
     */
    static fireAll(eventName: string, ...params: any[]): void
    {
        this.fire(this._events_in, this._firedEvents_in, eventName, params, false);
        this.fire(this._events_out, this._firedEvents_out, eventName, params, false);
    }

    /**
     * 通用触发逻辑。
     *
     * eventsImmediately 为 true 时立即调用当前注册表中的所有监听者。
     * eventsImmediately 为 false 时，把当前监听者快照放入 firedEvents。
     *
     * 入队时保存 EventInfo，而不是保存 eventName 后再查注册表，
     * 是为了保证事件触发时的监听者集合固定下来。
     */
    private static fire(events: EventMap, firedEvents: Array<FiredEvent>, eventName: string, params: any[], eventsImmediately: boolean): void
    {
        let eventList: Array<EventInfo> = events[eventName];
        if(eventList === undefined)
        {
            return;
        }

        if(eventsImmediately)
        {
            this.fireEventList(eventName, params, eventList);
        }
        else
        {
            for(let item of eventList)
            {
                firedEvents.push({ info: item, eventName, params });
            }
        }
    }

    /**
     * 调用一个事件名下的所有监听者。
     */
    private static fireEventList(eventName: string, params: any[], eventList: Array<EventInfo>): void
    {
        for(let item of eventList)
        {
            this.fireEventInfo(eventName, params, item);
        }
    }

    /**
     * 调用单个监听者。
     *
     * 参数数量和类型由 JavaScript 运行时处理，因此注册函数的形参与触发参数数量可以不完全一致。
     * 异常会被捕获并写入日志，避免一个监听者抛错影响后续事件循环。
     */
    private static fireEventInfo(eventName: string, params: any[], item: EventInfo): void
    {
        try
        {
            // 注意，传入参数和注册函数参数类型数量可以不一致，作为事件函数的参数类型检查没有作用
            item.m_cbFunction.apply(item.m_object, params);
        }
        catch(e)
        {
            KBELog.ERROR_MSG("Event::Fire(%s):%s", eventName, e);
        }
    }

    /**
     * 处理 out 事件队列。
     *
     * 会先把 _firedEvents_out 搬到 _doingEvents_out，再执行 _doingEvents_out。
     * 这样处理过程中再次触发的 out 事件会留在 _firedEvents_out，下一轮再执行。
     *
     * 如果当前处于 Pause 状态，不会消费 doing 队列。
     */
    static processOutEvents(){
        if(this._processOutEventsStarted) return;

        this._processOutEventsStarted = true;
        this.processFiredEvents(this._firedEvents_out, this._doingEvents_out, true);
        this._processOutEventsStarted = false;
    }

    /**
     * 处理 in 事件队列。
     *
     * in 事件由 fireIn/fireAll 放入队列。
     * processInEvents 不受 Pause 控制。
     */
    static processInEvents(){
        if(this._processInEventsStarted) return;

        this._processInEventsStarted = true;
        this.processFiredEvents(this._firedEvents_in, this._doingEvents_in, false);
        this._processInEventsStarted = false;
    }

    /**
     * 通用队列处理逻辑。
     *
     * firedEvents:
     * 新触发但尚未进入本轮执行的事件。
     *
     * doingEvents:
     * 本轮正在执行的事件快照。
     *
     * checkPause:
     * true 表示处理时受 Pause 控制。
     */
    private static processFiredEvents(firedEvents: Array<FiredEvent>, doingEvents: Array<FiredEvent>, checkPause: boolean): void
    {
        while(firedEvents.length > 0)
        {
            let eventObj = firedEvents.shift();
            if(eventObj)
            {
                doingEvents.push(eventObj);
            }
        }

        while(doingEvents.length > 0 && (!checkPause || !this._isPause))
        {
            let eventObj = doingEvents.shift();
            if(eventObj)
            {
                this.fireEventInfo(eventObj.eventName, eventObj.params, eventObj.info);
            }
        }
    }

    /**
     * 清理所有已触发但尚未处理完成的事件，并解除暂停状态。
     *
     * 只清理队列，不清理注册表。
     * 如果需要清理注册表，请使用 Deregister / deregisterOut / deregisterIn / DeregisterObject。
     */
    static clearFiredEvents(){
        this._firedEvents_out = [];
        this._doingEvents_out = [];
        this._firedEvents_in = [];
        this._doingEvents_in = [];
        this._isPause = false;
    }

    /**
     * 注销某个对象注册的所有事件。
     *
     * 会同时处理 out 和 in：
     * 1. 从 out/in 注册表中移除该对象的所有监听。
     * 2. 从 out/in 的 fired/doing 队列中移除该对象已经排队的回调。
     *
     * 常用于对象销毁、场景切换、KBEngineApp 卸载事件等场景。
     */
    static DeregisterObject(p_object: object): void
    {
        if(p_object === null)
        {
            KBELog.ERROR_MSG("Event::DeregisterObject:object cannot be null.");
            return;
        }

        let deleteCount: number = 0;
        deleteCount += this.deregisterObject(this._events_out, p_object);
        deleteCount += this.deregisterObject(this._events_in, p_object);
        this.removeAllFiredEvents(this._firedEvents_out, p_object);
        this.removeAllFiredEvents(this._doingEvents_out, p_object);
        this.removeAllFiredEvents(this._firedEvents_in, p_object);
        this.removeAllFiredEvents(this._doingEvents_in, p_object);

        KBELog.DEBUG_MSG("KBEEvent::DeregisterObject %s:delete count:%d.", p_object.toString(), deleteCount);
    }

    /**
     * 从指定注册表中移除某个对象的所有监听。
     *
     * 从后向前遍历，避免 splice 后数组下标变化导致漏删。
     */
    private static deregisterObject(events: EventMap, p_object: object): number
    {
        let deleteCount: number = 0;
        for(let key in events)
        {
            let eventList: Array<EventInfo> = events[key];
            for(let i = eventList.length - 1; i >= 0; i--)
            {
                if(eventList[i].m_object === p_object)
                {
                    eventList.splice(i, 1);
                    deleteCount += 1;
                }
            }
        }

        return deleteCount;
    }

    /**
     * 从指定队列中移除某个对象的某个事件回调。
     *
     * 用于 deregisterOut / deregisterIn，防止已经入队但尚未执行的回调继续触发。
     */
    private static removeFiredEvent(firedEvents: Array<FiredEvent>, p_object: object|null, eventName: string, cbFunction: Function): void
    {
        for(let i = firedEvents.length - 1; i >= 0; i--)
        {
            let eventObj = firedEvents[i];
            if(eventObj.eventName === eventName && eventObj.info.m_object === p_object && eventObj.info.m_cbFunction === cbFunction)
            {
                firedEvents.splice(i, 1);
            }
        }
    }

    /**
     * 从指定队列中移除某个对象的所有回调。
     *
     * 用于 DeregisterObject。
     */
    private static removeAllFiredEvents(firedEvents: Array<FiredEvent>, p_object: object): void
    {
        for(let i = firedEvents.length - 1; i >= 0; i--)
        {
            if(firedEvents[i].info.m_object === p_object)
            {
                firedEvents.splice(i, 1);
            }
        }
    }
}


// KBE-Plugin fire-out events(KBE => TS):
export class EventOutTypes {
    // ------------------------------------账号相关------------------------------------

    
    /** Create account feedback results. 
    <para> param1(uint16): retcode. server_errors</para>
    <para> param2(bytes): datas. If you use third-party account system, the system may fill some of the third-party additional datas. </para> */
    static onCreateAccountResult = "onCreateAccountResult";

    
    /** Response from binding account Email request.
    <para> param1(uint16): retcode. server_errors</para> */
    static onBindAccountEmail = "onBindAccountEmail";

    
    /** Response from a new password request.
    <para> param1(uint16): retcode. server_errors</para> */
    static onNewPassword = "onNewPassword";

    
    /** Response from a reset password request.
    <para> param1(uint16): retcode. server_errors</para> */
    static onResetPassword = "onResetPassword";

    // ------------------------------------连接相关------------------------------------
    
    /** Kicked of the current server.
    <para> param1(uint16): retcode. server_errors</para> */
    static onKicked = "onKicked";

    
    /** Disconnected from the server. */
    static onDisconnected = "onDisconnected";

    
    /** Status of connection server.
    <para> param1(bool): success or fail</para> */
    static onConnectionState = "onConnectionState";

    // ------------------------------------logon相关------------------------------------
    
    /** Engine version mismatch.
    <para> param1(string): clientVersion
    <para> param2(string): serverVersion */
    static onVersionNotMatch = "onVersionNotMatch";

    
    /** script version mismatch.
    <para> param1(string): clientScriptVersion
    <para> param2(string): serverScriptVersion */
    static onScriptVersionNotMatch = "onScriptVersionNotMatch";

    
    /** Login failed.
    <para> param1(uint16): retcode. server_errors</para> */
    static onLoginFailed = "onLoginFailed";

    
    /** Login to baseapp. */
    static onLoginBaseapp = "onLoginBaseapp";

    
    /** Login baseapp failed.
    <para> param1(uint16): retcode. server_errors</para> */
    static onLoginBaseappFailed = "onLoginBaseappFailed";

    
    /** Relogin to baseapp. */
    static onReloginBaseapp = "onReloginBaseapp";

    
    /** Relogin baseapp success. */
    static onReloginBaseappSuccessfully = "onReloginBaseappSuccessfully";

    
    /** Relogin baseapp failed.
    <para> param1(uint16): retcode. server_errors</para> */
    static onReloginBaseappFailed = "onReloginBaseappFailed";

    //------------------------------------实体cell相关事件------------------------------------

    
    /** Entity enter the client-world.
    <para> param1: Entity</para> */
    static onEnterWorld = "onEnterWorld";

    
    /** Entity leave the client-world.
    <para> param1: Entity</para> */
    static onLeaveWorld = "onLeaveWorld";

    
    /** Player enter the new space.
    <para> param1: Entity</para> */
    static onEnterSpace = "onEnterSpace";

    
    /** Player leave the space.
    <para> param1: Entity</para> */
    static onLeaveSpace = "onLeaveSpace";

    
    /** Sets the current position of the entity.
    <para> param1: Entity</para> */
    static set_position = "set_position";

    
    /** Sets the current direction of the entity.
    <para> param1: Entity</para> */
    static set_direction = "set_direction";

    
    /** The entity position is updated, you can smooth the moving entity to new location.
    <para> param1: Entity</para> */
    static updatePosition = "updatePosition";

    
    /** The current space is specified by the geometry mapping.
    Popular said is to load the specified Map Resources.
    <para> param1(string): resPath</para> */
    static addSpaceGeometryMapping = "addSpaceGeometryMapping";

    
    /** Server spaceData set data.
    <para> param1(int32): spaceID</para>
    <para> param2(string): key</para>
    <para> param3(string): value</para> */
    static onSetSpaceData = "onSetSpaceData";

    
    /** Start downloading data.
    <para> param1(int32): rspaceID</para>
    <para> param2(string): key</para> */
    static onDelSpaceData = "onDelSpaceData";

    
    /** Triggered when the entity is controlled or out of control.
    <para> param1: Entity</para>
    <para> param2(bool): isControlled</para> */
    static onControlled = "onControlled";

    
    /** Lose controlled entity.
    <para> param1: Entity</para> */
    static onLoseControlledEntity = "onLoseControlledEntity";

    // ------------------------------------数据下载相关------------------------------------
    
    /** Start downloading data.
    <para> param1(uint16): resouce id</para>
    <para> param2(uint32): data size</para>
    <para> param3(string): description</para> */
    static onStreamDataStarted = "onStreamDataStarted";

    
    /** Receive data.
    <para> param1(uint16): resouce id</para>
    <para> param2(bytes): datas</para> */
    static onStreamDataRecv = "onStreamDataRecv";

    
    /** The downloaded data is completed.
    <para> param1(uint16): resouce id</para> */
    static onStreamDataCompleted = "onStreamDataCompleted";
};


//KBE-Plugin fire-in events(TS => KBE):

export class EventInTypes {
    
    /** Create new account.
    <para> param1(string): accountName</para>
    <para> param2(string): password</para>
    <para> param3(bytes): datas Datas by user defined. Data will be recorded into the KBE account database, you can access the datas through the script layer. If you use third-party account system, datas will be submitted to the third-party system.</para> */
    static createAccount = "createAccount";

    
    /** Login to server.
    <para> param1(string): accountName</para>
    <para> param2(string): password</para>
    <para> param3(bytes): datas Datas by user defined. Data will be recorded into the KBE account database, you can access the datas through the script layer. If you use third-party account system, datas will be submitted to the third-party system.</para> */
    static login = "login";

    
    /** Logout to baseapp, called when exiting the client. */
    static logout = "logout";

    
    /** Relogin to baseapp. */
    static reloginBaseapp = "reloginBaseapp";

    
    /** Reset password.
    <para> param1(string): accountName</para> */
    static resetPassword = "resetPassword";

    
    /** Request to set up a new password for the account. Note: account must be online.
    <para> param1(string): old_password</para>
    <para> param2(string): new_password</para> */
    static newPassword = "newPassword";

    
    /** Request server binding account Email.
    <para> param1(string): emailAddress</para> */
    static bindAccountEmail = "bindAccountEmail";
};
