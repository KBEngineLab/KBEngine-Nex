export interface WebSocketFrameParseResult
{
    ok: boolean;
    messages: number;
    error?: string;
}

export type WebSocketMessageLengthLookup = (messageID: number) => number | undefined;
export type WebSocketMessageDispatch = (messageID: number, bodyOffset: number, bodyLength: number) => void;

function readUint16(data: DataView, offset: number): number
{
    return data.getUint16(offset, true);
}

function readUint32(data: DataView, offset: number): number
{
    return data.getUint32(offset, true);
}

function walkFrame(
    buffer: ArrayBuffer,
    lookupLength: WebSocketMessageLengthLookup,
    dispatch?: WebSocketMessageDispatch): WebSocketFrameParseResult
{
    if(buffer.byteLength === 0)
        return { ok: false, messages: 0, error: "empty WebSocket message" };

    const data = new DataView(buffer);
    let offset = 0;
    let messages = 0;

    while(offset < buffer.byteLength)
    {
        if(buffer.byteLength - offset < 2)
            return { ok: false, messages, error: "truncated message id at offset " + offset };

        const messageID = readUint16(data, offset);
        offset += 2;

        let bodyLength = lookupLength(messageID);
        if(bodyLength === undefined)
            return { ok: false, messages, error: "unknown message id " + messageID + " at offset " + (offset - 2) };

        if(bodyLength === -1)
        {
            if(buffer.byteLength - offset < 2)
                return { ok: false, messages, error: "truncated message length for id " + messageID };

            bodyLength = readUint16(data, offset);
            offset += 2;
            if(bodyLength === 65535)
            {
                if(buffer.byteLength - offset < 4)
                    return { ok: false, messages, error: "truncated extended message length for id " + messageID };

                bodyLength = readUint32(data, offset);
                offset += 4;
            }
        }

        if(!Number.isSafeInteger(bodyLength) || bodyLength < 0)
            return { ok: false, messages, error: "invalid message length for id " + messageID };
        if(bodyLength > buffer.byteLength - offset)
            return { ok: false, messages, error: "message body exceeds WebSocket message for id " + messageID };

        if(dispatch)
            dispatch(messageID, offset, bodyLength);

        offset += bodyLength;
        messages += 1;
    }

    return { ok: true, messages };
}

export function parseWebSocketFrame(
    buffer: ArrayBuffer,
    lookupLength: WebSocketMessageLengthLookup,
    dispatch: WebSocketMessageDispatch): WebSocketFrameParseResult
{
    // 先验证整条 WebSocket message，再执行任何 RPC，避免后半帧畸形时产生不可回滚的部分业务状态。
    // Validate the complete WebSocket message before dispatching any RPC so a malformed tail cannot leave partial application state.
    const validation = walkFrame(buffer, lookupLength);
    if(!validation.ok)
        return validation;

    return walkFrame(buffer, lookupLength, dispatch);
}
