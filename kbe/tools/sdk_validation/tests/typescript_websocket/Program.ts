import { parseWebSocketFrame } from "../../../../res/sdk_templates/client/typescript/WebSocketFrameParser";
import {
    DefaultWebSocketTransportFactory,
    KBEChannel
} from "../../../../res/sdk_templates/client/typescript/WebSocketTransport";

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

interface MockSocketTask
{
    callbacks: {
        open?: (event: unknown) => void;
        message?: (event: { data: unknown }) => void;
        error?: (event: unknown) => void;
        close?: (event: unknown) => void;
    };
    sent: ArrayBuffer[];
    closeOptions: { code?: number; reason?: string } | undefined;
    closeFailure: unknown;
    sendFailure: unknown;
    onOpen(callback: (event: unknown) => void): void;
    onMessage(callback: (event: { data: unknown }) => void): void;
    onError(callback: (event: unknown) => void): void;
    onClose(callback: (event: unknown) => void): void;
    send(options: { data: ArrayBuffer; fail?: (error: unknown) => void }): void;
    close(options?: { code?: number; reason?: string; fail?: (error: unknown) => void }): void;
}

function createSocketTask(): MockSocketTask
{
    return {
        callbacks: {},
        sent: [],
        closeOptions: undefined,
        closeFailure: undefined,
        sendFailure: undefined,
        onOpen(callback): void { this.callbacks.open = callback; },
        onMessage(callback): void { this.callbacks.message = callback; },
        onError(callback): void { this.callbacks.error = callback; },
        onClose(callback): void { this.callbacks.close = callback; },
        send(options): void
        {
            this.sent.push(options.data);
            if(this.sendFailure !== undefined)
                options.fail?.(this.sendFailure);
        },
        close(options): void
        {
            this.closeOptions = options;
            if(this.closeFailure !== undefined)
                options?.fail?.(this.closeFailure);
        }
    };
}

function verifyMiniProgramTransport(channel: KBEChannel, runtimeName: "wx" | "tt"): void
{
    const task = createSocketTask();
    let connectedAddress = "";
    const runtime: Record<string, unknown> = {
        [runtimeName]: {
            connectSocket(options: { url: string }): MockSocketTask
            {
                connectedAddress = options.url;
                return task;
            }
        }
    };
    const transport = new DefaultWebSocketTransportFactory(channel, runtime).create("wss://example.invalid/kbe");
    assertCondition(connectedAddress === "wss://example.invalid/kbe", runtimeName + " transport changed the address.");
    assertCondition(!transport.isOpen, runtimeName + " transport started in an open state.");

    let opened = 0;
    let received: unknown;
    let sendError: unknown;
    transport.onOpen = () => opened += 1;
    transport.onMessage = data => received = data;
    task.callbacks.open?.({});
    task.callbacks.message?.({ data: new ArrayBuffer(3) });
    assertCondition(opened === 1 && transport.isOpen, runtimeName + " open callback did not update state.");
    assertCondition(received instanceof ArrayBuffer, runtimeName + " did not forward binary data.");

    task.sendFailure = "send-failed";
    transport.send(new ArrayBuffer(5), error => sendError = error);
    assertCondition(task.sent.length === 1 && task.sent[0].byteLength === 5, runtimeName + " did not send the binary payload.");
    assertCondition(sendError === "send-failed", runtimeName + " ignored the asynchronous send failure.");

    let closeError: unknown;
    task.closeFailure = "close-failed";
    transport.close(1002, "protocol", error => closeError = error);
    assertCondition(!transport.isOpen, runtimeName + " remained open after close.");
    assertCondition(task.closeOptions?.code === 1002 && task.closeOptions.reason === "protocol",
        runtimeName + " did not preserve the close code and reason.");
    assertCondition(closeError === "close-failed", runtimeName + " ignored the asynchronous close failure.");

    transport.release();
    task.callbacks.open?.({});
    task.callbacks.message?.({ data: new ArrayBuffer(7) });
    assertCondition(opened === 1 && (received as ArrayBuffer).byteLength === 3,
        runtimeName + " forwarded stale callbacks after release.");
}

function verifyChannelSelection(): void
{
    const wechatTask = createSocketTask();
    const douyinTask = createSocketTask();
    let selected = "";
    const runtime: Record<string, unknown> = {
        wx: { connectSocket(): MockSocketTask { selected = "wx"; return wechatTask; } },
        tt: { connectSocket(): MockSocketTask { selected = "tt"; return douyinTask; } }
    };
    new DefaultWebSocketTransportFactory(KBEChannel.Auto, runtime).create("ws://auto");
    assertCondition(selected === "wx", "Auto channel selection did not use the documented WeChat priority.");

    let missingRuntimeRejected = false;
    try
    {
        new DefaultWebSocketTransportFactory(KBEChannel.Douyin, {}).create("ws://missing");
    }
    catch(_error)
    {
        missingRuntimeRejected = true;
    }
    assertCondition(missingRuntimeRejected, "An explicit unavailable mini-program channel was accepted.");
}

function verifyBrowserTransport(): void
{
    class MockBrowserSocket
    {
        static readonly OPEN = 1;
        readyState = MockBrowserSocket.OPEN;
        binaryType = "";
        onopen: ((event: unknown) => void) | null = null;
        onmessage: ((event: { data: unknown }) => void) | null = null;
        onerror: ((event: unknown) => void) | null = null;
        onclose: ((event: unknown) => void) | null = null;
        sent: ArrayBuffer[] = [];
        closed: { code?: number; reason?: string } | undefined;
        constructor(readonly address: string) {}
        send(data: ArrayBuffer): void { this.sent.push(data); }
        close(code?: number, reason?: string): void { this.closed = { code, reason }; }
    }

    const transport = new DefaultWebSocketTransportFactory(
        KBEChannel.Web,
        { WebSocket: MockBrowserSocket }).create("ws://browser");
    assertCondition(transport.isOpen, "Browser transport did not use the constructor OPEN state.");
    let sendError = false;
    transport.send(new ArrayBuffer(2), () => sendError = true);
    assertCondition(!sendError, "Browser transport reported a successful synchronous send as failed.");
    transport.release();
}

function main(): void
{
    verifyLargeVariableMessage();
    verifyExtendedVariableMessage();
    verifyWholeFrameValidationIsAtomic();
    verifyMalformedFrames();
    verifyMiniProgramTransport(KBEChannel.WeChat, "wx");
    verifyMiniProgramTransport(KBEChannel.Douyin, "tt");
    verifyChannelSelection();
    verifyBrowserTransport();
    console.log("TYPESCRIPT_WEBSOCKET_FRAME_TEST_PASS large=true bytes=60000 extended=true atomic=true unknown=true truncated=true overrun=true web=true wechat=true douyin=true stale=true async-send=true async-close=true");
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
