
import KBELog from "./KBELog";
import KBEEvent from "./Event";
import { MemoryStream } from "./KBEngine";
import Messages, { Message } from "./Messages";

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

    Close()
    {
        try
        {
            KBELog.DEBUG_MSG("NetworkInterface::Close on good:" + this.IsGood)
            if(this.socket != undefined)
            {
                this.socket.close();
                this.socket.onclose = null;
                this.socket = undefined;
            }
        }
        catch(e)
        {
            KBELog.ERROR_MSG("NetworkInterface::Close error:%s.", e);
        }
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
        let data: ArrayBuffer = event.data;
        //KBELog.DEBUG_MSG("NetworkInterface::onmessage:...!" + data.byteLength);
        let stream: MemoryStream = new MemoryStream(data);
        stream.wpos = data.byteLength;

        while(stream.rpos < stream.wpos)
        {
            let msgID = stream.ReadUint16();
            //KBELog.DEBUG_MSG("NetworkInterface::onmessage:...!msgID:" + msgID);

            let handler: Message = Messages.clientMessages[msgID];
            if(!handler)
            {
                KBELog.ERROR_MSG("NetworkInterface::onmessage:message(%d) has not found.", msgID);
                // msgID已消费但无法确定body长度→ 跳出循环防止流位置错位
                break;
            }
            else
            {
                let msgLen = handler.msglen;
                if(msgLen === -1)
                {
                    msgLen = stream.ReadUint16();
                    if(msgLen === 65535)
                    {
                        msgLen = stream.ReadUint32();
                    }
                }

                let wpos = stream.wpos;
                let rpos = stream.rpos + msgLen;
                stream.wpos = rpos;
                try
                {
                    handler.handleMessage(stream);
                }
                catch(e)
                {
                    KBELog.ERROR_MSG("NetworkInterface::onmessage: handleMessage exception! msg=" + handler.name + ", error=" + e);
                }
                stream.wpos = wpos;
                stream.rpos = rpos;
            }
        }
    }

    private onclose = () =>
    {
        KBELog.DEBUG_MSG("NetworkInterface::onclose:...!");
        KBEEvent.fireAll("onDisconnected");
    }
}