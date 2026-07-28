
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
    private onOpenCB: Function | undefined;

    constructor(private transportFactory: WebSocketTransportFactory = new DefaultWebSocketTransportFactory(KBEChannel.Auto))
    {
    }

    ConfigureTransport(factory: WebSocketTransportFactory): void
    {
        if(this.transport !== undefined)
            throw new Error("NetworkInterface transport cannot be changed while a connection exists.");
        this.transportFactory = factory;
    }

    get IsGood(): boolean
    {
        return this.transport !== undefined && this.transport.isOpen;
    }

    ConnectTo(addr: string, callbackFunc?: (event:Event)=>any)
    {
        try
        {
            this.transport = this.transportFactory.create(addr);
        }
        catch(e)
        {
            KBELog.ERROR_MSG("NetworkInterface::Connect:Init socket error:" + e);
            KBEEvent.fireAll("onConnectionState", false);
            return;
        }

        const transport = this.transport;
        transport.onError = event => this.onerror(transport, event);
        transport.onClose = event => this.onclose(transport, event);
        transport.onMessage = (data, event) => this.onmessage(transport, data, event);
        transport.onOpen = event => this.onopen(transport, event);
        if(callbackFunc)
        {
            this.onOpenCB = callbackFunc;
        }
    }

    Close(notifyDisconnected: boolean = false)
    {
        const transport = this.transport;
        if(transport === undefined)
            return;

        KBELog.DEBUG_MSG("NetworkInterface::Close on good:" + this.IsGood)
        this.ReleaseTransport(transport);

        try
        {
            transport.close(undefined, undefined, error =>
                KBELog.ERROR_MSG("NetworkInterface::Close async error:%s.", error));
        }
        catch(e)
        {
            KBELog.ERROR_MSG("NetworkInterface::Close error:%s.", e);
        }

        if(notifyDisconnected)
            KBEEvent.fireAll("onDisconnected");
    }

    Send(buffer: ArrayBuffer): boolean
    {
        if(!this.IsGood)
        {
            KBELog.ERROR_MSG("NetworkInterface::Send:socket is unavailable.");
            return false;
        }

        try
        {
            KBELog.DEBUG_MSG("NetworkInterface::Send buffer length:[%d].", buffer.byteLength);
            const transport = this.transport!;
            let synchronousError = false;
            let invokingTransport = true;
            transport.send(buffer, error =>
            {
                if(invokingTransport)
                    synchronousError = true;
                if(this.transport !== transport)
                    return;
                KBELog.ERROR_MSG("NetworkInterface::Send async error:%s.", error);
                KBEEvent.fireIn("onNetworkError", error as MessageEvent);
            });
            invokingTransport = false;
            // 返回值只表达底层 API 是否同步接受提交；小程序后续异步 fail 仍通过 onNetworkError 关闭当前连接。
            // The return value only reports synchronous submission acceptance; a later mini-program fail still closes the current connection through onNetworkError.
            return !synchronousError;
        }
        catch(e)
        {
            KBELog.ERROR_MSG("NetworkInterface::Send error:%s.", e);
            return false;
        }
    }

    private onopen = (transport: WebSocketTransport, event: unknown) =>
    {
        if(this.transport !== transport)
            return;
        KBELog.DEBUG_MSG("NetworkInterface::onopen:success!");
        if(this.onOpenCB)
        {
            this.onOpenCB(event as MessageEvent);
            this.onOpenCB = undefined;
        }
    }
    
    private onerror = (transport: WebSocketTransport, event: unknown) =>
    {
        if(this.transport !== transport)
            return;
        KBELog.DEBUG_MSG("NetworkInterface::onerror:...!");
        KBEEvent.fireIn("onNetworkError", event as MessageEvent);
    }

    private onmessage = (transport: WebSocketTransport, data: unknown, _event: unknown) =>
    {
        if(this.transport !== transport)
            return;
        if(!(data instanceof ArrayBuffer))
        {
            this.RejectProtocolFrame(transport, "expected ArrayBuffer payload");
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
            this.RejectProtocolFrame(transport, result.error || "malformed KBEngine frame");
    }

    private RejectProtocolFrame(transport: WebSocketTransport, reason: string): void
    {
        if(this.transport !== transport)
            return;

        // 协议错误先摘除回调和当前引用，再关闭旧 socket 并通知一次，避免 close 回调重入或误伤同步重登录的新连接。
        // Detach callbacks and the current reference before closing a protocol-failed socket so close reentry cannot duplicate notification or damage a synchronous relogin.
        KBELog.ERROR_MSG("NetworkInterface::onmessage: rejected malformed WebSocket message: " + reason);
        this.ReleaseTransport(transport);
        try
        {
            transport.close(1002, "Malformed KBEngine frame", error =>
                KBELog.ERROR_MSG("NetworkInterface::onmessage: protocol close failed: " + error));
        }
        catch(error)
        {
            KBELog.ERROR_MSG("NetworkInterface::onmessage: protocol close failed: " + error);
        }
        KBEEvent.fireAll("onDisconnected");
    }

    private onclose = (transport: WebSocketTransport, _event: unknown) =>
    {
        KBELog.DEBUG_MSG("NetworkInterface::onclose:...!");

        // close 回调可能晚于下一次 ConnectTo；只允许产生该事件的 socket 清理自身，避免陈旧事件破坏重登录连接。
        // A close callback may arrive after the next ConnectTo; only the socket that emitted it may clean itself up, protecting the relogin connection from stale events.
        if(this.transport !== transport)
            return;

        this.ReleaseTransport(transport);
        KBEEvent.fireAll("onDisconnected");
    }

    private ReleaseTransport(transport: WebSocketTransport): void
    {
        // 先解除回调并清除当前引用，再关闭或发布事件，避免同步业务回调创建的新连接被旧生命周期覆盖。
        // Detach callbacks and clear the current reference before closing or publishing events so synchronous application callbacks cannot have their new connection overwritten.
        transport.release();
        if(this.transport === transport)
            this.transport = undefined;
        this.onOpenCB = undefined;
    }
}
