
import KBELog from "./KBELog";
import KBEEvent from "./Event";
import { MemoryStream } from "./KBEngine";
import Messages, { Message } from "./Messages";
import { parseWebSocketFrame } from "./WebSocketFrameParser";

export default class NetworkInterface
{
    private socket: WebSocket | undefined;
    private onOpenCB: Function | undefined;

    get IsGood(): boolean
    {
        return this.socket != undefined && this.socket.readyState === WebSocket.OPEN;
    }

    ConnectTo(addr: string, callbackFunc?: (event:Event)=>any)
    {
        try
        {
            this.socket = new WebSocket(addr);
        }
        catch(e)
        {
            KBELog.ERROR_MSG("NetworkInterface::Connect:Init socket error:" + e);
            KBEEvent.fireAll("onConnectionState", false);
            return;
        }

        this.socket.binaryType = "arraybuffer";

        this.socket.onerror = this.onerror;
        this.socket.onclose = this.onclose;
        this.socket.onmessage = this.onmessage;
        this.socket.onopen = this.onopen;
        if(callbackFunc)
        {
            this.onOpenCB = callbackFunc;
        }
    }

    Close(notifyDisconnected: boolean = false)
    {
        const socket = this.socket;
        if(socket === undefined)
            return;

        KBELog.DEBUG_MSG("NetworkInterface::Close on good:" + this.IsGood)
        this.ReleaseSocket(socket);

        try
        {
            socket.close();
        }
        catch(e)
        {
            KBELog.ERROR_MSG("NetworkInterface::Close error:%s.", e);
        }

        if(notifyDisconnected)
            KBEEvent.fireAll("onDisconnected");
    }

    Send(buffer: ArrayBuffer)
    {
        if(!this.IsGood)
        {
            KBELog.ERROR_MSG("NetworkInterface::Send:socket is unavailable.");
            return;
        }

        try
        {
            KBELog.DEBUG_MSG("NetworkInterface::Send buffer length:[%d].", buffer.byteLength);
            this.socket!.send(buffer);
        }
        catch(e)
        {
            KBELog.ERROR_MSG("NetworkInterface::Send error:%s.", e);
        }
    }

    private onopen = (event: Event) =>
    {
        KBELog.DEBUG_MSG("NetworkInterface::onopen:success!");
        if(this.onOpenCB)
        {
            this.onOpenCB(event as MessageEvent);
            this.onOpenCB = undefined;
        }
    }
    
    private onerror = (event: Event) =>
    {
        KBELog.DEBUG_MSG("NetworkInterface::onerror:...!");
        KBEEvent.fireIn("onNetworkError", event as MessageEvent);
    }

    private onmessage = (event: MessageEvent) =>
    {
        const sourceSocket = (event.currentTarget || event.target) as WebSocket | null;
        if(sourceSocket === null || this.socket !== sourceSocket)
            return;
        if(!(event.data instanceof ArrayBuffer))
        {
            this.RejectProtocolFrame(sourceSocket, "expected ArrayBuffer payload");
            return;
        }

        const data = event.data;
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
            this.RejectProtocolFrame(sourceSocket, result.error || "malformed KBEngine frame");
    }

    private RejectProtocolFrame(socket: WebSocket, reason: string): void
    {
        if(this.socket !== socket)
            return;

        // 协议错误先摘除回调和当前引用，再关闭旧 socket 并通知一次，避免 close 回调重入或误伤同步重登录的新连接。
        // Detach callbacks and the current reference before closing a protocol-failed socket so close reentry cannot duplicate notification or damage a synchronous relogin.
        KBELog.ERROR_MSG("NetworkInterface::onmessage: rejected malformed WebSocket message: " + reason);
        this.ReleaseSocket(socket);
        try
        {
            socket.close(1002, "Malformed KBEngine frame");
        }
        catch(error)
        {
            KBELog.ERROR_MSG("NetworkInterface::onmessage: protocol close failed: " + error);
        }
        KBEEvent.fireAll("onDisconnected");
    }

    private onclose = (event: CloseEvent) =>
    {
        KBELog.DEBUG_MSG("NetworkInterface::onclose:...!");

        // close 回调可能晚于下一次 ConnectTo；只允许产生该事件的 socket 清理自身，避免陈旧事件破坏重登录连接。
        // A close callback may arrive after the next ConnectTo; only the socket that emitted it may clean itself up, protecting the relogin connection from stale events.
        const closedSocket = (event.currentTarget || event.target) as WebSocket | null;
        if(closedSocket === null || this.socket !== closedSocket)
            return;

        this.ReleaseSocket(closedSocket);
        KBEEvent.fireAll("onDisconnected");
    }

    private ReleaseSocket(socket: WebSocket): void
    {
        // 先解除回调并清除当前引用，再关闭或发布事件，避免同步业务回调创建的新连接被旧生命周期覆盖。
        // Detach callbacks and clear the current reference before closing or publishing events so synchronous application callbacks cannot have their new connection overwritten.
        socket.onerror = null;
        socket.onclose = null;
        socket.onmessage = null;
        socket.onopen = null;
        if(this.socket === socket)
            this.socket = undefined;
        this.onOpenCB = undefined;
    }
}
