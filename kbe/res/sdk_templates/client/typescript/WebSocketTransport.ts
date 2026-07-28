export enum KBEPlatform
{
    TypeScript = "typescript",
    Cocos = "cocos",
    Laya = "laya"
}

// 平台描述引擎宿主生命周期，渠道描述实际网络运行时；二者正交，不能按组合复制协议实现。
// Platform describes engine-host lifecycle while channel describes the network runtime; they remain orthogonal to avoid duplicated protocol implementations.
export enum KBEChannel
{
    Auto = "auto",
    Web = "web",
    WeChat = "wechat",
    Douyin = "douyin"
}

export interface WebSocketTransport
{
    readonly isOpen: boolean;
    // 返回底层已接受但尚未完成发送的字节数，使 NetworkInterface 能在提交完整 Bundle 前实施背压。
    // Reports bytes accepted but not yet sent by the native layer so NetworkInterface can apply backpressure before submitting a complete Bundle.
    readonly pendingBytes?: number;
    onOpen: ((event: unknown) => void) | undefined;
    onMessage: ((data: unknown, event: unknown) => void) | undefined;
    onError: ((event: unknown) => void) | undefined;
    onClose: ((event: unknown) => void) | undefined;
    send(data: ArrayBuffer, onError: (error: unknown) => void): void;
    close(code?: number, reason?: string, onError?: (error: unknown) => void): void;
    release(): void;
}

export interface WebSocketTransportFactory
{
    create(address: string): WebSocketTransport;
}

interface BrowserWebSocketLike
{
    readyState: number;
    readonly bufferedAmount: number;
    binaryType: string;
    onopen: ((event: unknown) => void) | null;
    onmessage: ((event: { data: unknown }) => void) | null;
    onerror: ((event: unknown) => void) | null;
    onclose: ((event: unknown) => void) | null;
    send(data: ArrayBuffer): void;
    close(code?: number, reason?: string): void;
}

interface BrowserWebSocketConstructor
{
    new(address: string): BrowserWebSocketLike;
    readonly OPEN: number;
}

interface MiniProgramSocketTask
{
    onOpen(callback: (event: unknown) => void): void;
    onMessage(callback: (event: { data: unknown }) => void): void;
    onError(callback: (event: unknown) => void): void;
    onClose(callback: (event: unknown) => void): void;
    send(options: { data: ArrayBuffer; success?: () => void; fail?: (error: unknown) => void }): void;
    close(options?: { code?: number; reason?: string; success?: () => void; fail?: (error: unknown) => void }): void;
}

interface MiniProgramSocketApi
{
    connectSocket(options: { url: string }): MiniProgramSocketTask;
}

abstract class TransportBase implements WebSocketTransport
{
    onOpen: ((event: unknown) => void) | undefined;
    onMessage: ((data: unknown, event: unknown) => void) | undefined;
    onError: ((event: unknown) => void) | undefined;
    onClose: ((event: unknown) => void) | undefined;

    abstract get isOpen(): boolean;
    abstract send(data: ArrayBuffer, onError: (error: unknown) => void): void;
    abstract close(code?: number, reason?: string, onError?: (error: unknown) => void): void;

    release(): void
    {
        // 底层任务可能不支持移除监听器，清空转发目标即可隔离迟到回调并避免持有 NetworkInterface。
        // A native task may not support listener removal; clearing forwarding targets isolates stale callbacks and releases NetworkInterface references.
        this.onOpen = undefined;
        this.onMessage = undefined;
        this.onError = undefined;
        this.onClose = undefined;
    }
}

export class BrowserWebSocketTransport extends TransportBase
{
    private readonly socket: BrowserWebSocketLike;
    private readonly openState: number;

    constructor(address: string, constructorOverride?: BrowserWebSocketConstructor)
    {
        super();
        const socketConstructor = constructorOverride ||
            (globalThis as unknown as { WebSocket?: BrowserWebSocketConstructor }).WebSocket;
        if(socketConstructor === undefined)
            throw new Error("The WebSocket constructor is unavailable in this runtime.");

        this.openState = socketConstructor.OPEN;
        this.socket = new socketConstructor(address);
        this.socket.binaryType = "arraybuffer";
        this.socket.onopen = event => this.onOpen?.(event);
        this.socket.onmessage = event => this.onMessage?.(event.data, event);
        this.socket.onerror = event => this.onError?.(event);
        this.socket.onclose = event => this.onClose?.(event);
    }

    get isOpen(): boolean
    {
        return this.socket.readyState === this.openState;
    }

    get pendingBytes(): number
    {
        return this.socket.bufferedAmount;
    }

    send(data: ArrayBuffer, onError: (error: unknown) => void): void
    {
        try
        {
            this.socket.send(data);
        }
        catch(error)
        {
            onError(error);
        }
    }

    close(code?: number, reason?: string, _onError?: (error: unknown) => void): void
    {
        this.socket.close(code, reason);
    }

    release(): void
    {
        this.socket.onopen = null;
        this.socket.onmessage = null;
        this.socket.onerror = null;
        this.socket.onclose = null;
        super.release();
    }
}

class MiniProgramWebSocketTransport extends TransportBase
{
    private readonly task: MiniProgramSocketTask;
    private opened = false;
    private bufferedBytes = 0;

    constructor(address: string, api: MiniProgramSocketApi)
    {
        super();
        this.task = api.connectSocket({ url: address });
        this.task.onOpen(event =>
        {
            this.opened = true;
            this.onOpen?.(event);
        });
        this.task.onMessage(event => this.onMessage?.(event.data, event));
        this.task.onError(event => this.onError?.(event));
        this.task.onClose(event =>
        {
            this.opened = false;
            this.onClose?.(event);
        });
    }

    get isOpen(): boolean
    {
        return this.opened;
    }

    get pendingBytes(): number
    {
        return this.bufferedBytes;
    }

    send(data: ArrayBuffer, onError: (error: unknown) => void): void
    {
        // SocketTask 的发送错误通过异步 fail 回调返回，不能只依赖同步 try/catch。
        // SocketTask reports send failures through an asynchronous fail callback, so synchronous try/catch is insufficient.
        let settled = false;
        this.bufferedBytes += data.byteLength;
        const settle = (): boolean =>
        {
            if(settled)
                return false;
            settled = true;
            this.bufferedBytes = Math.max(0, this.bufferedBytes - data.byteLength);
            return true;
        };
        try
        {
            this.task.send({
                data,
                success: settle,
                fail: error =>
                {
                    if(settle())
                        onError(error);
                }
            });
        }
        catch(error)
        {
            if(settle())
                onError(error);
        }
    }

    close(code?: number, reason?: string, onError?: (error: unknown) => void): void
    {
        this.opened = false;
        // 主动释放会先清空事件转发目标，因此关闭失败必须通过本次操作回调返回，不能依赖 onError 事件。
        // Active release clears event forwarding first, so close failures must return through this operation callback instead of relying on onError.
        this.task.close({ code, reason, fail: onError });
    }

    release(): void
    {
        this.bufferedBytes = 0;
        super.release();
    }
}

export class DefaultWebSocketTransportFactory implements WebSocketTransportFactory
{
    constructor(
        private readonly channel: KBEChannel = KBEChannel.Auto,
        private readonly runtime: Record<string, unknown> = globalThis as unknown as Record<string, unknown>)
    {
    }

    create(address: string): WebSocketTransport
    {
        // 显式渠道具有确定性，Auto 仅在未配置时按微信、抖音、浏览器顺序探测可用 API。
        // An explicit channel is deterministic; Auto probes available APIs in WeChat, Douyin, then browser order only when no channel is configured.
        const channel = this.channel === KBEChannel.Auto ? this.detectChannel() : this.channel;
        if(channel === KBEChannel.WeChat)
            return new MiniProgramWebSocketTransport(address, this.requireMiniProgramApi("wx"));
        if(channel === KBEChannel.Douyin)
            return new MiniProgramWebSocketTransport(address, this.requireMiniProgramApi("tt"));
        if(channel === KBEChannel.Web)
            return new BrowserWebSocketTransport(address, this.runtime.WebSocket as BrowserWebSocketConstructor | undefined);
        throw new Error("Unsupported WebSocket channel: " + channel);
    }

    private detectChannel(): KBEChannel
    {
        if(this.hasConnectSocket("wx"))
            return KBEChannel.WeChat;
        if(this.hasConnectSocket("tt"))
            return KBEChannel.Douyin;
        if(typeof this.runtime.WebSocket === "function")
            return KBEChannel.Web;
        throw new Error("No supported WebSocket runtime was detected.");
    }

    private hasConnectSocket(name: string): boolean
    {
        const value = this.runtime[name] as { connectSocket?: unknown } | undefined;
        return value !== undefined && typeof value.connectSocket === "function";
    }

    private requireMiniProgramApi(name: string): MiniProgramSocketApi
    {
        if(!this.hasConnectSocket(name))
            throw new Error(name + ".connectSocket is unavailable in this runtime.");
        return this.runtime[name] as unknown as MiniProgramSocketApi;
    }
}
