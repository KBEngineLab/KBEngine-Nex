import { KBEPlatform } from "./WebSocketTransport";

export interface HostLifecycleCallbacks
{
    update(): void;
    updatePlayer(): void;
    processEvents(): void;
    pause(): void;
    resume(): void;
}

export interface HostLifecycleIntervals
{
    updateMS: number;
    updatePlayerMS: number;
    processEventsMS: number;
}

export interface HostLifecycleAdapter
{
    start(callbacks: HostLifecycleCallbacks, intervals: HostLifecycleIntervals): void;
    stop(): void;
}

abstract class HostLifecycleBase implements HostLifecycleAdapter
{
    protected callbacks: HostLifecycleCallbacks | undefined;
    protected paused = false;

    start(callbacks: HostLifecycleCallbacks, intervals: HostLifecycleIntervals): void
    {
        if(this.callbacks !== undefined)
            throw new Error("Host lifecycle adapter is already running.");
        this.callbacks = callbacks;
        try
        {
            this.startCore(intervals);
        }
        catch(error)
        {
            // 宿主 API 可能在部分定时器或事件注册完成后抛错，必须在传播原始异常前撤销已注册资源。
            // Host APIs can fail after registering some timers or events, so release partial resources before preserving the original failure.
            try
            {
                this.stopCore();
            }
            catch(_cleanupError)
            {
                // 启动异常最能说明根因，清理异常不能覆盖它；应用层仍会再调用一次幂等 stop。
                // The startup failure best identifies the root cause and must not be replaced by cleanup failure; the app layer also retries idempotent stop.
            }
            this.callbacks = undefined;
            this.paused = false;
            throw error;
        }
    }

    stop(): void
    {
        if(this.callbacks === undefined)
            return;
        try
        {
            this.stopCore();
        }
        finally
        {
            // 即使宿主清理 API 报错也要复位适配器状态，避免后续回调继续触达已销毁的应用。
            // Reset adapter state even when a host cleanup API fails so later callbacks cannot reach a disposed application.
            this.callbacks = undefined;
            this.paused = false;
        }
    }

    protected runUpdate = (): void =>
    {
        if(!this.paused)
            this.callbacks?.update();
    };

    protected runUpdatePlayer = (): void =>
    {
        if(!this.paused)
            this.callbacks?.updatePlayer();
    };

    protected runProcessEvents = (): void =>
    {
        if(!this.paused)
            this.callbacks?.processEvents();
    };

    protected notifyPause = (): void =>
    {
        if(this.paused || this.callbacks === undefined)
            return;
        this.paused = true;
        this.callbacks.pause();
    };

    protected notifyResume = (): void =>
    {
        if(!this.paused || this.callbacks === undefined)
            return;
        this.paused = false;
        this.callbacks.resume();
    };

    protected abstract startCore(intervals: HostLifecycleIntervals): void;
    protected abstract stopCore(): void;
}

interface TimerRuntime
{
    setInterval(callback: () => void, milliseconds: number): unknown;
    clearInterval(handle: unknown): void;
}

interface VisibilityDocument
{
    hidden: boolean;
    addEventListener(name: string, callback: () => void): void;
    removeEventListener(name: string, callback: () => void): void;
}

export class TypeScriptHostLifecycle extends HostLifecycleBase
{
    private handles: unknown[] = [];
    private readonly document: VisibilityDocument | undefined;

    constructor(
        private readonly timer: TimerRuntime = globalThis as unknown as TimerRuntime,
        documentOverride?: VisibilityDocument)
    {
        super();
        this.document = documentOverride ||
            (globalThis as unknown as { document?: VisibilityDocument }).document;
    }

    protected startCore(intervals: HostLifecycleIntervals): void
    {
        // 逐个保存句柄，使后续注册失败时仍能清理由本次启动已经创建的定时器。
        // Store each handle immediately so a later registration failure can still release timers created by this startup attempt.
        this.handles.push(this.timer.setInterval(this.runUpdate, intervals.updateMS));
        this.handles.push(this.timer.setInterval(this.runUpdatePlayer, intervals.updatePlayerMS));
        this.handles.push(this.timer.setInterval(this.runProcessEvents, intervals.processEventsMS));
        this.document?.addEventListener("visibilitychange", this.onVisibilityChanged);
        if(this.document?.hidden)
            this.notifyPause();
    }

    protected stopCore(): void
    {
        for(const handle of this.handles)
            this.timer.clearInterval(handle);
        this.handles = [];
        this.document?.removeEventListener("visibilitychange", this.onVisibilityChanged);
    }

    private onVisibilityChanged = (): void =>
    {
        if(this.document?.hidden)
            this.notifyPause();
        else
            this.notifyResume();
    };
}

interface CocosScheduler
{
    schedule(callback: () => void, target: object, interval: number, repeat: number, delay: number, paused: boolean): void;
    unscheduleAllForTarget(target: object): void;
}

interface CocosGame
{
    EVENT_HIDE: string;
    EVENT_SHOW: string;
    on(name: string, callback: () => void, target: object): void;
    off(name: string, callback: () => void, target: object): void;
}

interface CocosRuntime
{
    director: { getScheduler(): CocosScheduler };
    game: CocosGame;
}

export class CocosHostLifecycle extends HostLifecycleBase
{
    private readonly target = {};
    private readonly scheduler: CocosScheduler;

    constructor(private readonly cocos: CocosRuntime)
    {
        super();
        this.scheduler = cocos.director.getScheduler();
    }

    protected startCore(intervals: HostLifecycleIntervals): void
    {
        // Cocos 调度器使用秒并把回调所有权绑定到 target，销毁时可一次性解除全部任务。
        // Cocos schedules in seconds and binds callback ownership to a target so all tasks can be removed atomically during disposal.
        this.schedule(this.runUpdate, intervals.updateMS);
        this.schedule(this.runUpdatePlayer, intervals.updatePlayerMS);
        this.schedule(this.runProcessEvents, intervals.processEventsMS);
        this.cocos.game.on(this.cocos.game.EVENT_HIDE, this.notifyPause, this.target);
        this.cocos.game.on(this.cocos.game.EVENT_SHOW, this.notifyResume, this.target);
    }

    protected stopCore(): void
    {
        this.scheduler.unscheduleAllForTarget(this.target);
        this.cocos.game.off(this.cocos.game.EVENT_HIDE, this.notifyPause, this.target);
        this.cocos.game.off(this.cocos.game.EVENT_SHOW, this.notifyResume, this.target);
    }

    private schedule(callback: () => void, milliseconds: number): void
    {
        this.scheduler.schedule(callback, this.target, milliseconds / 1000, Number.MAX_SAFE_INTEGER, 0, false);
    }
}

interface LayaTimer
{
    loop(milliseconds: number, caller: object, callback: () => void): void;
    clearAll(caller: object): void;
}

interface LayaStage
{
    on(name: string, caller: object, callback: () => void): void;
    off(name: string, caller: object, callback: () => void): void;
}

interface LayaRuntime
{
    timer: LayaTimer;
    stage: LayaStage;
    Event: { BLUR: string; FOCUS: string };
}

export class LayaHostLifecycle extends HostLifecycleBase
{
    private readonly caller = {};

    constructor(private readonly laya: LayaRuntime)
    {
        super();
    }

    protected startCore(intervals: HostLifecycleIntervals): void
    {
        this.laya.timer.loop(intervals.updateMS, this.caller, this.runUpdate);
        this.laya.timer.loop(intervals.updatePlayerMS, this.caller, this.runUpdatePlayer);
        this.laya.timer.loop(intervals.processEventsMS, this.caller, this.runProcessEvents);
        this.laya.stage.on(this.laya.Event.BLUR, this.caller, this.notifyPause);
        this.laya.stage.on(this.laya.Event.FOCUS, this.caller, this.notifyResume);
    }

    protected stopCore(): void
    {
        this.laya.timer.clearAll(this.caller);
        this.laya.stage.off(this.laya.Event.BLUR, this.caller, this.notifyPause);
        this.laya.stage.off(this.laya.Event.FOCUS, this.caller, this.notifyResume);
    }
}

export class DefaultHostLifecycleFactory
{
    constructor(
        private readonly platform: KBEPlatform,
        private readonly runtime: Record<string, unknown> = globalThis as unknown as Record<string, unknown>)
    {
    }

    create(): HostLifecycleAdapter
    {
        if(this.platform === KBEPlatform.TypeScript)
            return new TypeScriptHostLifecycle(
                this.runtime as unknown as TimerRuntime,
                this.runtime.document as VisibilityDocument | undefined);
        if(this.platform === KBEPlatform.Cocos)
            return new CocosHostLifecycle(this.requireRuntime<CocosRuntime>("cc"));
        if(this.platform === KBEPlatform.Laya)
            return new LayaHostLifecycle(this.requireRuntime<LayaRuntime>("Laya"));
        throw new Error("Unsupported host platform: " + this.platform);
    }

    private requireRuntime<Runtime>(name: string): Runtime
    {
        const value = this.runtime[name];
        if(value === undefined || value === null)
            throw new Error(name + " host runtime is unavailable; provide KBEngineArgs.hostLifecycleAdapter explicitly.");
        return value as Runtime;
    }
}
