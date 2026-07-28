import { parseWebSocketFrame } from "../../../../res/sdk_templates/client/typescript/WebSocketFrameParser";

function assertCondition(condition: boolean, message: string): void
{
    if(!condition)
        throw new Error(message);
}

function makeFrame(messageID: number, body: Uint8Array, variable: boolean): ArrayBuffer
{
    const extended = variable && body.byteLength >= 65535;
    const headerLength = variable ? (extended ? 8 : 4) : 2;
    const frame = new ArrayBuffer(headerLength + body.byteLength);
    const view = new DataView(frame);
    view.setUint16(0, messageID, true);
    if(variable)
    {
        view.setUint16(2, extended ? 65535 : body.byteLength, true);
        if(extended)
            view.setUint32(4, body.byteLength, true);
    }
    new Uint8Array(frame, headerLength).set(body);
    return frame;
}

function verifyExtendedVariableMessage(): void
{
    const body = new Uint8Array(70000);
    let received = 0;
    const result = parseWebSocketFrame(
        makeFrame(7, body, true),
        messageID => messageID === 7 ? -1 : undefined,
        (_messageID, _bodyOffset, bodyLength) => received = bodyLength);
    assertCondition(result.ok && result.messages === 1 && received === body.byteLength,
        "The parser rejected a valid extended-length message.");
}

function verifyLargeVariableMessage(): void
{
    const body = new Uint8Array(60000);
    let expectedChecksum = 0;
    for(let index = 0; index < body.byteLength; ++index)
    {
        body[index] = index % 251;
        expectedChecksum += body[index];
    }

    let receivedChecksum = 0;
    const frame = makeFrame(7, body, true);
    const result = parseWebSocketFrame(
        frame,
        messageID => messageID === 7 ? -1 : undefined,
        (messageID, bodyOffset, bodyLength) =>
        {
            assertCondition(messageID === 7 && bodyLength === body.byteLength, "The large message boundary changed.");
            const bytes = new Uint8Array(frame, bodyOffset, bodyLength);
            for(const value of bytes)
                receivedChecksum += value;
        });

    assertCondition(result.ok && result.messages === 1, "The parser rejected a valid 60 KiB message.");
    assertCondition(receivedChecksum === expectedChecksum, "The 60 KiB message checksum changed.");
}

function verifyWholeFrameValidationIsAtomic(): void
{
    const frame = new Uint8Array(2 + 3 + 2 + 1);
    const view = new DataView(frame.buffer);
    view.setUint16(0, 1, true);
    frame.set([10, 11, 12], 2);
    view.setUint16(5, 2, true);
    frame[7] = 9;

    let dispatched = 0;
    const result = parseWebSocketFrame(
        frame.buffer,
        messageID => messageID === 1 ? 3 : messageID === 2 ? 2 : undefined,
        () => dispatched += 1);
    assertCondition(!result.ok, "A frame with a truncated tail was accepted.");
    assertCondition(dispatched === 0, "The valid prefix was dispatched before the malformed tail was rejected.");
}

function verifyMalformedFrames(): void
{
    const lookup = (messageID: number): number | undefined => messageID === 1 ? 3 : messageID === 2 ? -1 : undefined;
    const dispatch = (): void => { throw new Error("Malformed input must not dispatch."); };

    assertCondition(!parseWebSocketFrame(new ArrayBuffer(0), lookup, dispatch).ok, "An empty frame was accepted.");
    assertCondition(!parseWebSocketFrame(new Uint8Array([1]).buffer, lookup, dispatch).ok, "A truncated message id was accepted.");
    assertCondition(!parseWebSocketFrame(new Uint8Array([99, 0]).buffer, lookup, dispatch).ok, "An unknown message id was accepted.");
    assertCondition(!parseWebSocketFrame(new Uint8Array([2, 0, 1]).buffer, lookup, dispatch).ok, "A truncated variable length was accepted.");
    assertCondition(!parseWebSocketFrame(new Uint8Array([2, 0, 255, 255, 1, 2]).buffer, lookup, dispatch).ok,
        "A truncated extended length was accepted.");
    assertCondition(!parseWebSocketFrame(new Uint8Array([1, 0, 10]).buffer, lookup, dispatch).ok, "A body overrun was accepted.");
}

function main(): void
{
    verifyLargeVariableMessage();
    verifyExtendedVariableMessage();
    verifyWholeFrameValidationIsAtomic();
    verifyMalformedFrames();
    console.log("TYPESCRIPT_WEBSOCKET_FRAME_TEST_PASS large=true bytes=60000 extended=true atomic=true unknown=true truncated=true overrun=true");
}

try
{
    main();
}
catch(error)
{
    console.error("TYPESCRIPT_WEBSOCKET_FRAME_TEST_FAIL error=" + String(error));
    throw error;
}
