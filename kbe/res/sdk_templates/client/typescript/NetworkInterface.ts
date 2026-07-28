
import KBELog from "./KBELog";
import KBEEvent from "./Event";
import { MemoryStream } from "./MemoryStream";
import Messages from "./Messages";
import type { Message } from "./Messages";
import { parseWebSocketFrame } from "./WebSocketFrameParser";
import {
    DefaultWebSocketTransportFactory,
    KBEChannel,
    WebSocketTransport,
    WebSocketTransportFactory
} from "./WebSocketTransport";

export default class NetworkInterface
{
    private transport: WebSocketTransport | undefined;
    // 对象身份隔离普通旧回调，连接代次进一步隔离复用同一 transport wrapper 的旧闭包。
    // Object identity isolates ordinary stale callbacks while the generation also isolates old closures when a transport wrapper is reused.
    private connectionGeneration = 0;
    private connectedGeneration: number | undefined;
    private onOpenCB: Function | undefined;
    private sendQueueMax = 256 * 1024;

    constructor(private transportFactory: WebSocketTransportFactory = new DefaultWebSocketTransportFactory(KBEChannel.Auto))
    {
    }

    ConfigureTransport(factory: WebSocketTransportFactory): void
    {
        if(this.transport !== undefined)
            throw new Error("NetworkInterface transport cannot be changed while a connection exists.");
        this.transportFactory = factory;
    }

    ConfigureSendQueueLimit(bytes: number): void
    {
        if(this.transport !== undefined)
            throw new Error("NetworkInterface send queue limit cannot be changed while a connection exists.");
        if(!Number.isSafeInteger(bytes) || bytes <= 0)
            throw new Error("NetworkInterface send queue limit must be a positive safe integer.");
        this.sendQueueMax = bytes;
    }

    get IsGood(): boolean
    {
        return this.transport !== undefined && this.transport.isOpen;
    }

    ConnectTo(addr: string, callbackFunc?: (event:Event)=>any): void
    {
        // 显式替换连接时先静默释放旧 transport，避免直接覆盖引用导致 socket 和迟到回调泄漏。
        // Explicit connection replacement silently releases the old transport first so overwriting the reference cannot leak its socket or stale callbacks.
        if(this.transport !== undefined)
            this.Close();
        this.onOpenCB = callbackFunc;
        const generation = ++this.connectionGeneration;

        try
        {
            this.transport = this.transportFactory.create(addr);
        }
        catch(e)
        {
            this.onOpenCB = undefined;
            KBELog.ERROR_MSG("NetworkInterface::Connect:Init socket error:" + e);
            KBEEvent.fireAll("onConnectionState", false);
            return;
        }

        const transport = this.transport;
        transport.onError = event => this.onerror(transport, generation, event);
        transport.onClose = event => this.onclose(transport, generation, event);
        transport.onMessage = (data, event) => this.onmessage(transport, generation, data, event);
        transport.onOpen = event => this.onopen(transport, generation, event);
    }

    Close(notifyDisconnected: boolean = false): void
    {
        const transport = this.transport;
        if(transport === undefined)
            return;

        const generation = this.connectionGeneration;
        KBELog.DEBUG_MSG("NetworkInterface::Close on good:" + this.IsGood)
        const wasConnected = this.ReleaseTransport(transport, generation);
        this.CloseTransport(transport, undefined, undefined, "NetworkInterface::Close");

        if(notifyDisconnected && wasConnected)
            KBEEvent.fireAll("onDisconnected");
    }

    Send(buffer: ArrayBuffer): boolean
    {
        return this.SendBatch([buffer]);
    }

    SendBatch(buffers: ReadonlyArray<ArrayBuffer>): boolean
    {
        if(!this.IsGood)
        {
            KBELog.ERROR_MSG("NetworkInterface::Send:socket is unavailable.");
            return false;
        }

        if(buffers.length === 0)
        {
            KBELog.ERROR_MSG("NetworkInterface::SendBatch:empty batch.");
            return false;
        }

        const transport = this.transport!;
        const generation = this.connectionGeneration;
        let totalBytes = 0;
        try
        {
            for(const buffer of buffers)
            {
                if(totalBytes > this.sendQueueMax - buffer.byteLength)
                    return this.RejectSend(transport, generation,
                        new Error("send batch exceeds queue limit " + this.sendQueueMax));
                totalBytes += buffer.byteLength;
            }

            const pendingBytes = transport.pendingBytes ?? 0;
            // 自定义传输可省略积压指标，但此时只能执行单批次上限，累计队列控制由该传输自行负责。
            // A custom transport may omit backlog metrics, but then only the per-batch limit applies and cumulative queue control is its responsibility.
            if(!Number.isSafeInteger(pendingBytes) || pendingBytes < 0)
                return this.RejectSend(transport, generation, new Error("transport reported invalid pending bytes"));
            if(pendingBytes > this.sendQueueMax - totalBytes)
                return this.RejectSend(transport, generation,
                    new Error("send queue limit exceeded: pending=" + pendingBytes + ", batch=" + totalBytes));

            let payload = buffers[0];
            if(buffers.length > 1)
            {
                payload = new ArrayBuffer(totalBytes);
                const output = new Uint8Array(payload);
                let offset = 0;
                for(const buffer of buffers)
                {
                    output.set(new Uint8Array(buffer), offset);
                    offset += buffer.byteLength;
                }
            }

            KBELog.DEBUG_MSG("NetworkInterface::SendBatch buffer length:[%d].", totalBytes);
            let synchronousError = false;
            let invokingTransport = true;
            transport.send(payload, error =>
            {
                if(invokingTransport)
                    synchronousError = true;
                if(!this.IsCurrent(transport, generation))
                    return;
                KBELog.ERROR_MSG("NetworkInterface::Send async error:%s.", error);
                this.FailTransport(transport, generation);
                KBEEvent.fireIn("onNetworkError", error as MessageEvent);
            });
            invokingTransport = false;
            // 返回值只表达底层 API 是否同步接受提交；小程序后续异步 fail 仍通过 onNetworkError 关闭当前连接。
            // The return value only reports synchronous submission acceptance; a later mini-program fail still closes the current connection through onNetworkError.
            return !synchronousError;
        }
        catch(e)
        {
            return this.RejectSend(transport, generation, e);
        }
    }

    private RejectSend(transport: WebSocketTransport, generation: number, error: unknown): false
    {
        KBELog.ERROR_MSG("NetworkInterface::SendBatch error:%s.", error);
        this.FailTransport(transport, generation);
        KBEEvent.fireIn("onNetworkError", error as MessageEvent);
        return false;
    }

    private onopen = (transport: WebSocketTransport, generation: number, event: unknown) =>
    {
        if(!this.IsCurrent(transport, generation))
            return;
        if(this.connectedGeneration === generation)
            return;
        this.connectedGeneration = generation;
        KBELog.DEBUG_MSG("NetworkInterface::onopen:success!");
        // 在执行用户回调前转移所有权，回调内同步重连时旧回调返回后不会清除新连接的 onOpenCB。
        // Transfer callback ownership before invocation so synchronous reconnection cannot have its new onOpenCB cleared when the old callback returns.
        const callback = this.onOpenCB;
        this.onOpenCB = undefined;
        callback?.(event as MessageEvent);
    }
    
    private onerror = (transport: WebSocketTransport, generation: number, event: unknown) =>
    {
        if(!this.IsCurrent(transport, generation))
            return;
        KBELog.DEBUG_MSG("NetworkInterface::onerror:...!");
        this.FailTransport(transport, generation);
        KBEEvent.fireIn("onNetworkError", event as MessageEvent);
    }

    private onmessage = (transport: WebSocketTransport, generation: number, data: unknown, _event: unknown) =>
    {
        if(!this.IsCurrent(transport, generation))
            return;
        if(!(data instanceof ArrayBuffer))
        {
            this.RejectProtocolFrame(transport, generation, "expected ArrayBuffer payload");
            return;
        }

        const stream = new MemoryStream(data);
        const result = parseWebSocketFrame(
            data,
            (messageID: number) => Messages.clientMessages[messageID]?.msglen,
            (messageID: number, bodyOffset: number, bodyLength: number) =>
            {
                const handler: Message = Messages.clientMessages[messageID];
                stream.rpos = bodyOffset;
                stream.wpos = bodyOffset + bodyLength;
                try
                {
                    handler.handleMessage(stream);
                }
                catch(error)
                {
                    // 业务 handler 异常与传输 framing 无关，保持既有记录并继续的语义，不能伪造协议断线。
                    // An application handler failure is unrelated to transport framing; preserve log-and-continue behavior instead of inventing a protocol disconnect.
                    KBELog.ERROR_MSG("NetworkInterface::onmessage: handleMessage exception! msg=" + handler.name + ", error=" + error);
                }
            });

        if(!result.ok)
            this.RejectProtocolFrame(transport, generation, result.error || "malformed KBEngine frame");
    }

    private RejectProtocolFrame(transport: WebSocketTransport, generation: number, reason: string): void
    {
        if(!this.IsCurrent(transport, generation))
            return;

        // 协议错误先摘除回调和当前引用，再关闭旧 socket 并通知一次，避免 close 回调重入或误伤同步重登录的新连接。
        // Detach callbacks and the current reference before closing a protocol-failed socket so close reentry cannot duplicate notification or damage a synchronous relogin.
        KBELog.ERROR_MSG("NetworkInterface::onmessage: rejected malformed WebSocket message: " + reason);
        const wasConnected = this.ReleaseTransport(transport, generation);
        this.CloseTransport(transport, 1002, "Malformed KBEngine frame", "NetworkInterface::onmessage: protocol close");
        this.NotifyPassiveTermination(wasConnected);
    }

    private onclose = (transport: WebSocketTransport, generation: number, _event: unknown) =>
    {
        // close 回调可能晚于下一次 ConnectTo；只有对象与代次都匹配的 socket 才能清理当前连接。
        // A close callback may arrive after the next ConnectTo; only a socket matching both object identity and generation may clean up the current connection.
        if(!this.IsCurrent(transport, generation))
            return;

        KBELog.DEBUG_MSG("NetworkInterface::onclose:...!");
        const wasConnected = this.ReleaseTransport(transport, generation);
        this.NotifyPassiveTermination(wasConnected);
    }

    private FailTransport(transport: WebSocketTransport, generation: number): void
    {
        if(!this.IsCurrent(transport, generation))
            return;

        const wasConnected = this.ReleaseTransport(transport, generation);
        this.CloseTransport(transport, undefined, undefined, "NetworkInterface::failure");
        this.NotifyPassiveTermination(wasConnected);
    }

    private NotifyPassiveTermination(wasConnected: boolean): void
    {
        // 只有成功 open 的连接拥有业务断线通知资格；连接阶段失败属于连接结果，不能伪造 onDisconnected。
        // Only a successfully opened connection owns disconnect-notification eligibility; setup failure is a connection result and must not invent onDisconnected.
        if(wasConnected)
            KBEEvent.fireAll("onDisconnected");
        else
            KBEEvent.fireAll("onConnectionState", false);
    }

    private CloseTransport(
        transport: WebSocketTransport,
        code: number | undefined,
        reason: string | undefined,
        context: string): void
    {
        try
        {
            transport.close(code, reason, error =>
                KBELog.ERROR_MSG(context + " async error:%s.", error));
        }
        catch(error)
        {
            KBELog.ERROR_MSG(context + " error:%s.", error);
        }
    }

    private IsCurrent(transport: WebSocketTransport, generation: number): boolean
    {
        return this.transport === transport && this.connectionGeneration === generation;
    }

    private ReleaseTransport(transport: WebSocketTransport, generation: number): boolean
    {
        // 先解除回调并清除当前引用，再关闭或发布事件，避免同步业务回调创建的新连接被旧生命周期覆盖。
        // Detach callbacks and clear the current reference before closing or publishing events so synchronous application callbacks cannot have their new connection overwritten.
        if(!this.IsCurrent(transport, generation))
            return false;

        const wasConnected = this.connectedGeneration === generation;
        this.transport = undefined;
        if(wasConnected)
            this.connectedGeneration = undefined;
        this.onOpenCB = undefined;
        try
        {
            transport.release();
        }
        catch(error)
        {
            // 自定义 transport 的清理异常不能恢复已经失效的当前连接引用。
            // A custom transport cleanup failure must not restore a current-connection reference that is already invalid.
            KBELog.ERROR_MSG("NetworkInterface::ReleaseTransport error:%s.", error);
        }
        return wasConnected;
    }
}
