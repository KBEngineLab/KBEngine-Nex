export interface MonotonicClock
{
    now(): number;
}

interface ClockRuntime
{
    performance?: { now(): number };
}

export class DefaultMonotonicClock implements MonotonicClock
{
    private readonly source: () => number;
    private lastValue: number;

    constructor(runtime: ClockRuntime = globalThis as unknown as ClockRuntime)
    {
        const performance = runtime.performance;
        // performance.now() 不受系统墙钟校时影响；旧宿主缺失该 API 时保留 Date.now() 兼容路径并钳制时间倒退。
        // performance.now() is immune to wall-clock adjustments; legacy hosts fall back to Date.now() with backward movement clamped.
        this.source = performance !== undefined && typeof performance.now === "function"
            ? performance.now.bind(performance)
            : Date.now;
        const initialValue = this.source();
        this.lastValue = Number.isFinite(initialValue) ? initialValue : 0;
    }

    now(): number
    {
        const value = this.source();
        if(Number.isFinite(value) && value > this.lastValue)
            this.lastValue = value;
        return this.lastValue;
    }
}

export class HeartbeatState
{
    // 状态对象只表达时间与请求生命周期，不持有 transport；发送、关闭和公开断线通知仍由 KBEngineApp 负责。
    // This state object models only timing and request lifecycle without owning a transport; KBEngineApp remains responsible for sends, closes, and public disconnect events.
    private lastAttemptTime: number;
    private pending = false;

    constructor(
        private readonly clock: MonotonicClock = new DefaultMonotonicClock(),
        private readonly intervalMS: number = 15000)
    {
        if(!Number.isFinite(intervalMS) || intervalMS <= 0)
            throw new Error("Heartbeat interval must be a positive finite number.");
        this.lastAttemptTime = this.clock.now();
    }

    get replyPending(): boolean
    {
        return this.pending;
    }

    timestamp(): number
    {
        return this.clock.now();
    }

    isDue(now: number): boolean
    {
        return now - this.lastAttemptTime > this.intervalMS;
    }

    markAttempt(now: number, sent: boolean): void
    {
        // 失败尝试也推进周期，避免 transport 暂时不可用时每个宿主 Tick 都重建 Bundle 和刷错误日志。
        // Failed attempts also advance the interval so a temporarily unavailable transport does not rebuild a Bundle and log on every host tick.
        this.lastAttemptTime = now;
        this.pending = sent;
    }

    markReply(): void
    {
        this.pending = false;
    }

    reset(now: number = this.clock.now()): void
    {
        this.lastAttemptTime = now;
        this.pending = false;
    }

    scheduleImmediate(now: number = this.clock.now()): void
    {
        // 宿主恢复不能继承暂停前的 pending；下一次调度应立即探测连接，但不在恢复回调内直接发送网络包。
        // Host resume must not inherit pre-suspension pending state; the next scheduled update probes immediately without sending inside the resume callback.
        this.lastAttemptTime = now - this.intervalMS - 1;
        this.pending = false;
    }
}
