"use strict";

const fs = require("fs");
const path = require("path");

function assertCondition(condition, message)
{
    if(!condition)
        throw new Error(message);
}

function readSource(sdkPath, fileName)
{
    const filePath = path.join(sdkPath, fileName);
    assertCondition(fs.existsSync(filePath), "Generated TypeScript SDK is missing " + fileName);
    return fs.readFileSync(filePath, "utf8");
}

function loadModule(compiledPath, fileName)
{
    const filePath = path.join(compiledPath, fileName);
    assertCondition(fs.existsSync(filePath), "Compiled TypeScript SDK is missing " + fileName);
    return require(filePath);
}

function verifySourceDependencies(sdkPath)
{
    const engineSource = readSource(sdkPath, "KBEngine.ts");
    const memorySource = readSource(sdkPath, "MemoryStream.ts");
    const networkSource = readSource(sdkPath, "NetworkInterface.ts");
    const int64Source = readSource(sdkPath, "Int64.ts");
    const messagesSource = readSource(sdkPath, "Messages.ts");
    const entityDefSource = readSource(sdkPath, "EntityDef.ts");
    const dataTypesSource = readSource(sdkPath, "DataTypes.ts");
    const kbeTypesSource = readSource(sdkPath, "KBETypes.ts");
    const transportSource = readSource(sdkPath, "WebSocketTransport.ts");
    const heartbeatSource = readSource(sdkPath, "HeartbeatState.ts");
    const aggregateImport = /from\s+["']\.\/KBEngine["']/;

    // 协议核心和网络实现不能反向依赖兼容聚合入口，否则 re-export 会重新形成运行时初始化环。
    // Protocol core and network implementations must not depend on the compatibility aggregate or its re-exports recreate a runtime initialization cycle.
    assertCondition(!aggregateImport.test(memorySource), "MemoryStream.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(networkSource), "NetworkInterface.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(int64Source), "Int64.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(messagesSource), "Messages.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(entityDefSource), "EntityDef.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(dataTypesSource), "DataTypes.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(kbeTypesSource), "KBETypes.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(transportSource), "WebSocketTransport.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(heartbeatSource), "HeartbeatState.ts imports KBEngine.ts");
    assertCondition(!/export\s+class\s+MemoryStream\b/.test(engineSource), "KBEngine.ts still defines MemoryStream");
    assertCondition(!/export\s+class\s+NetworkInterface\b/.test(engineSource), "KBEngine.ts still defines NetworkInterface");
    assertCondition(!/export\s+namespace\s+DataTypes\b/.test(engineSource), "KBEngine.ts still defines DataTypes");
    assertCondition(/import\s+\{\s*DataTypes\s*\}\s+from\s+["']\.\/DataTypes["']/.test(entityDefSource),
        "EntityDef.ts does not import the canonical DataTypes module");
    assertCondition(/import\s+type\s+\{\s*MemoryStream\s*\}\s+from\s+["']\.\/MemoryStream["']/.test(messagesSource),
        "Messages.ts does not use a type-only MemoryStream import");

    const clientMessageClasses = [...messagesSource.matchAll(/export\s+class\s+Message_(Client_[A-Za-z0-9_]+)/g)];
    const generatedHandlers = [...messagesSource.matchAll(/export\s+class\s+Message_(Client_[A-Za-z0-9_]+)[\s\S]*?RequireDispatchTarget\("([^"]+)"\)\.([A-Za-z0-9_]+)\(/g)];
    assertCondition(clientMessageClasses.length > 0 && generatedHandlers.length === clientMessageClasses.length,
        "Not every generated client message uses the dispatch target");
    for(const match of generatedHandlers)
        assertCondition(match[1] === match[2] && match[2] === match[3], "Generated message dispatch target changed for " + match[1]);
}

function verifyMessageDispatch(compiledPath)
{
    const Messages = loadModule(compiledPath, "Messages.js").default;
    const calls = [];
    const target = {
        probe(value)
        {
            calls.push({ owner: this, value });
        }
    };

    Messages.BindDispatchTarget(target);
    Messages.RequireDispatchTarget("probe").probe(7);
    assertCondition(calls.length === 1 && calls[0].owner === target && calls[0].value === 7,
        "Message dispatch did not preserve target identity or arguments");

    let replacementRejected = false;
    try
    {
        Messages.BindDispatchTarget({});
    }
    catch(_error)
    {
        replacementRejected = true;
    }
    assertCondition(replacementRejected, "Message dispatch target was replaced without an explicit unbind");

    Messages.UnbindDispatchTarget({});
    Messages.RequireDispatchTarget("probe").probe(8);
    assertCondition(calls.length === 2, "A stale owner unbound the active message dispatch target");

    Messages.UnbindDispatchTarget(target);
    let unboundRejected = false;
    try
    {
        Messages.RequireDispatchTarget("probe").probe(9);
    }
    catch(_error)
    {
        unboundRejected = true;
    }
    assertCondition(unboundRejected, "Message dispatch succeeded without a bound target");
}

function createTransport()
{
    return {
        isOpen: false,
        pendingBytes: 0,
        onOpen: undefined,
        onMessage: undefined,
        onError: undefined,
        onClose: undefined,
        sent: [],
        closed: [],
        released: false,
        sendError: undefined,
        sendThrows: undefined,
        sendCallbacks: [],
        send(data, onError)
        {
            this.sendCallbacks.push(onError);
            if(this.sendThrows !== undefined)
                throw this.sendThrows;
            this.sent.push(data);
            if(this.sendError !== undefined)
                onError(this.sendError);
        },
        close(code, reason, _onError)
        {
            this.closed.push({ code, reason });
        },
        release()
        {
            this.released = true;
            this.onOpen = undefined;
            this.onMessage = undefined;
            this.onError = undefined;
            this.onClose = undefined;
        }
    };
}

function verifyNetworkCloseLifecycle(compiledPath)
{
    const NetworkInterface = loadModule(compiledPath, "NetworkInterface.js").default;
    const KBEEvent = loadModule(compiledPath, "Event.js").default;
    const transports = Array.from({ length: 8 }, () => createTransport());
    let transportIndex = 0;
    const network = new NetworkInterface({ create: () => transports[transportIndex++] });
    const counts = { disconnected: 0, connectionFailed: 0, networkError: 0 };
    const observer = {
        onDisconnected() { counts.disconnected += 1; },
        onConnectionState(success) { if(!success) counts.connectionFailed += 1; },
        onNetworkError() { counts.networkError += 1; }
    };
    KBEEvent.registerOut("onDisconnected", observer, observer.onDisconnected);
    KBEEvent.registerOut("onConnectionState", observer, observer.onConnectionState);
    KBEEvent.registerIn("onNetworkError", observer, observer.onNetworkError);

    try
    {
        const first = transports[0];
        network.ConnectTo("ws://first");
        const staleOpen = first.onOpen;
        const staleError = first.onError;
        const staleClose = first.onClose;
        first.isOpen = true;
        first.onOpen({});
        network.Send(new ArrayBuffer(1));
        const staleSendFailure = first.sendCallbacks[0];

        const second = transports[1];
        network.ConnectTo("ws://second");
        second.isOpen = true;
        second.onOpen({});
        staleOpen({});
        staleError("stale error");
        staleClose({});
        staleSendFailure("stale send failure");
        KBEEvent.processInEvents();
        KBEEvent.processOutEvents();
        assertCondition(network.IsGood && first.released && first.closed.length === 1 && counts.disconnected === 0,
            "Connection replacement leaked the old transport or published a false disconnect");

        const secondError = second.onError;
        const secondClose = second.onClose;
        secondError("active error");
        secondClose({});
        secondError("duplicate active error");
        KBEEvent.processInEvents();
        KBEEvent.processOutEvents();
        assertCondition(!network.IsGood && second.released && second.closed.length === 1 &&
            counts.disconnected === 1 && counts.networkError === 1,
            "Active transport error did not close and notify exactly once");

        const connecting = transports[2];
        network.ConnectTo("ws://connecting");
        connecting.onClose({});
        KBEEvent.processOutEvents();
        assertCondition(counts.connectionFailed === 1 && counts.disconnected === 1,
            "A pre-open close was reported as an established-connection disconnect");

        const malformed = transports[3];
        network.ConnectTo("ws://malformed-lifecycle");
        malformed.isOpen = true;
        malformed.onOpen({});
        malformed.onMessage(new ArrayBuffer(0), {});
        KBEEvent.processOutEvents();
        assertCondition(counts.disconnected === 2 && malformed.closed.length === 1 &&
            malformed.closed[0].code === 1002,
            "Protocol rejection did not consume one established disconnect notification");

        const passive = transports[4];
        network.ConnectTo("ws://passive-close");
        passive.isOpen = true;
        passive.onOpen({});
        const passiveClose = passive.onClose;
        network.Close(true);
        passiveClose({});
        KBEEvent.processOutEvents();
        assertCondition(counts.disconnected === 3 && passive.closed.length === 1,
            "Explicit passive close did not notify exactly once");

        const active = transports[5];
        network.ConnectTo("ws://active-close");
        active.isOpen = true;
        active.onOpen({});
        const activeClose = active.onClose;
        network.Close();
        activeClose({});
        KBEEvent.processOutEvents();
        assertCondition(counts.disconnected === 3 && active.closed.length === 1,
            "Active close published a passive disconnect notification");

        const reentrant = transports[6];
        const replacement = transports[7];
        let replacementOpens = 0;
        network.ConnectTo("ws://reentrant", () =>
            network.ConnectTo("ws://replacement", () => replacementOpens += 1));
        reentrant.isOpen = true;
        reentrant.onOpen({});
        replacement.isOpen = true;
        replacement.onOpen({});
        assertCondition(replacementOpens === 1 && network.IsGood,
            "Reentrant open callback cleared the replacement connection callback");
        network.Close();

        const reused = createTransport();
        const reusedNetwork = new NetworkInterface({ create: () => reused });
        reusedNetwork.ConnectTo("ws://reused-first");
        reused.isOpen = true;
        const reusedStaleError = reused.onError;
        const reusedStaleClose = reused.onClose;
        reused.onOpen({});
        reusedNetwork.Send(new ArrayBuffer(1));
        const reusedStaleSendFailure = reused.sendCallbacks[0];
        reusedNetwork.ConnectTo("ws://reused-second");
        reused.isOpen = true;
        reused.onOpen({});
        reusedStaleError("reused stale error");
        reusedStaleClose({});
        reusedStaleSendFailure("reused stale send failure");
        assertCondition(reusedNetwork.IsGood,
            "A stale callback matched a reused transport object from a newer connection generation");
        reusedNetwork.Send(new ArrayBuffer(1));
        const activeAsyncFailure = reused.sendCallbacks[1];
        activeAsyncFailure("active async send failure");
        activeAsyncFailure("duplicate async send failure");
        KBEEvent.processInEvents();
        KBEEvent.processOutEvents();
        assertCondition(!reusedNetwork.IsGood && reused.closed.length === 2 &&
            counts.disconnected === 4 && counts.networkError === 2,
            "Active asynchronous send failure did not close and notify exactly once");
    }
    finally
    {
        KBEEvent.DeregisterObject(observer);
    }
}

function verifyBundleSendResult(compiledPath)
{
    const { Bundle } = loadModule(compiledPath, "KBEngine.js");
    const fixedMessage = { id: 1, msglen: 1 };
    const variableMessage = { id: 2, msglen: -1 };
    const submissions = [];
    const acceptedBundle = new Bundle();
    acceptedBundle.NewMessage(fixedMessage);
    acceptedBundle.WriteUint8(9);
    acceptedBundle.NewMessage(variableMessage);
    acceptedBundle.WriteBlob(new Uint8Array([10, 11, 12]));
    const accepted = acceptedBundle.Send({
        SendBatch(buffers)
        {
            submissions.push(buffers);
            return true;
        }
    });
    assertCondition(accepted && submissions.length === 1 && submissions[0].length === 1,
        "Bundle did not submit all messages as one atomic batch");
    const frame = submissions[0][0];
    const parsed = [];
    const parseResult = loadModule(compiledPath, "WebSocketFrameParser.js").parseWebSocketFrame(
        frame,
        messageID => messageID === 1 ? 1 : messageID === 2 ? -1 : undefined,
        (messageID, bodyOffset, bodyLength) => parsed.push({
            messageID,
            body: Array.from(new Uint8Array(frame, bodyOffset, bodyLength))
        }));
    assertCondition(parseResult.ok && parseResult.messages === 2 &&
        JSON.stringify(parsed) === JSON.stringify([
            { messageID: 1, body: [9] },
            { messageID: 2, body: [3, 0, 0, 0, 10, 11, 12] }
        ]), "Bundle changed the fixed or variable message wire layout");

    for(const bodyLength of [60000, 70000])
    {
        const body = new Uint8Array(bodyLength);
        let expectedChecksum = 0;
        for(let index = 0; index < body.length; ++index)
        {
            body[index] = index % 251;
            expectedChecksum += body[index];
        }
        let payload;
        const largeBundle = new Bundle();
        largeBundle.NewMessage(variableMessage);
        largeBundle.WriteBlob(body);
        assertCondition(largeBundle.Send({ SendBatch(buffers) { payload = buffers[0]; return true; } }),
            "Bundle rejected a valid large message");
        const view = new DataView(payload);
        const encodedBodyLength = bodyLength + 4;
        const extended = encodedBodyLength >= 65535;
        const payloadOffset = extended ? 8 : 4;
        assertCondition(view.getUint16(2, true) === (extended ? 65535 : encodedBodyLength) &&
            (!extended || view.getUint32(4, true) === encodedBodyLength),
            "Bundle encoded the large variable-message length incorrectly");
        assertCondition(view.getUint32(payloadOffset, true) === bodyLength,
            "Bundle changed the large BLOB length prefix");
        assertCondition(payload.byteLength === payloadOffset + 4 + bodyLength,
            "Bundle retained duplicate or trailing bytes after a large message");
        let actualChecksum = 0;
        for(const value of new Uint8Array(payload, payloadOffset + 4, bodyLength))
            actualChecksum += value;
        assertCondition(actualChecksum === expectedChecksum, "Bundle truncated or changed a large BLOB body");

        let reusedPayload;
        largeBundle.NewMessage(fixedMessage);
        largeBundle.WriteUint8(7);
        assertCondition(largeBundle.Send({ SendBatch(buffers) { reusedPayload = buffers[0]; return true; } }) &&
            reusedPayload.byteLength === 3,
            "A consumed large Bundle retained its previous payload when reused");
    }

    const rejectedBundle = new Bundle();
    rejectedBundle.NewMessage(fixedMessage);
    rejectedBundle.WriteUint8(1);
    const rejected = rejectedBundle.Send({ SendBatch: () => false });
    assertCondition(!rejected, "Bundle did not propagate a rejected network submission");
}

function verifyNetworkSendBackpressure(compiledPath)
{
    const NetworkInterface = loadModule(compiledPath, "NetworkInterface.js").default;
    const transport = createTransport();
    const network = new NetworkInterface({ create: () => transport });
    network.ConfigureSendQueueLimit(8);
    network.ConnectTo("ws://backpressure");
    transport.isOpen = true;
    transport.onOpen({});

    assertCondition(network.SendBatch([new Uint8Array([1, 2]).buffer, new Uint8Array([3, 4, 5]).buffer]) &&
        transport.sent.length === 1 &&
        JSON.stringify(Array.from(new Uint8Array(transport.sent[0]))) === JSON.stringify([1, 2, 3, 4, 5]),
        "NetworkInterface did not coalesce one batch into one ordered transport send");

    transport.pendingBytes = 6;
    assertCondition(!network.SendBatch([new ArrayBuffer(3)]) && transport.sent.length === 1 &&
        transport.closed.length === 1 && transport.released,
        "NetworkInterface did not atomically reject a batch that exceeded pending-byte capacity");

    const oversized = createTransport();
    const oversizedNetwork = new NetworkInterface({ create: () => oversized });
    oversizedNetwork.ConfigureSendQueueLimit(8);
    oversizedNetwork.ConnectTo("ws://oversized");
    oversized.isOpen = true;
    oversized.onOpen({});
    assertCondition(!oversizedNetwork.SendBatch([new ArrayBuffer(9)]) && oversized.sent.length === 0 &&
        oversized.closed.length === 1, "NetworkInterface partially submitted an oversized batch");

    const empty = createTransport();
    const emptyNetwork = new NetworkInterface({ create: () => empty });
    emptyNetwork.ConnectTo("ws://empty");
    empty.isOpen = true;
    empty.onOpen({});
    assertCondition(!emptyNetwork.SendBatch([]) && empty.closed.length === 0 && !empty.released,
        "NetworkInterface closed a healthy connection for an empty caller batch");
}

function verifyTransportLifecycle(compiledPath)
{
    const NetworkInterface = loadModule(compiledPath, "NetworkInterface.js").default;
    const first = createTransport();
    const second = createTransport();
    const malformed = createTransport();
    const transports = [first, second, malformed];
    const addresses = [];
    const network = new NetworkInterface({
        create(address)
        {
            addresses.push(address);
            return transports[addresses.length - 1];
        }
    });

    let opens = 0;
    network.ConnectTo("ws://first", () => opens += 1);
    const staleOpen = first.onOpen;
    first.isOpen = true;
    first.onOpen({});
    assertCondition(network.IsGood && opens === 1, "Custom transport open state was not observed");

    const accepted = network.Send(new ArrayBuffer(6));
    assertCondition(accepted && first.sent.length === 1 && first.sent[0].byteLength === 6,
        "NetworkInterface did not report a successful synchronous transport submission");
    first.sendError = "send-failed";
    const rejected = network.Send(new ArrayBuffer(1));
    assertCondition(!rejected, "NetworkInterface accepted a synchronous transport send failure");
    network.Close();
    assertCondition(first.released && first.closed.length === 1 && !network.IsGood,
        "NetworkInterface did not release and close the active transport");

    network.ConnectTo("ws://second", () => opens += 1);
    staleOpen({});
    assertCondition(opens === 1, "A stale transport callback affected the replacement connection");
    second.isOpen = true;
    second.onOpen({});
    assertCondition(opens === 2 && addresses.join(",") === "ws://first,ws://second",
        "The replacement transport did not receive the expected address or open callback");
    network.Close();

    network.ConnectTo("ws://malformed");
    malformed.isOpen = true;
    malformed.onMessage(new ArrayBuffer(0), {});
    assertCondition(malformed.released && malformed.closed.length === 1,
        "A malformed frame did not release and close its transport exactly once");
    assertCondition(malformed.closed[0].code === 1002 && malformed.closed[0].reason === "Malformed KBEngine frame",
        "A malformed frame did not preserve the WebSocket protocol close contract");
}

function verifyRuntimeIdentity(compiledPath)
{
    const engine = loadModule(compiledPath, "KBEngine.js");
    const memory = loadModule(compiledPath, "MemoryStream.js");
    const int64 = loadModule(compiledPath, "Int64.js");
    const network = loadModule(compiledPath, "NetworkInterface.js");
    const dataTypes = loadModule(compiledPath, "DataTypes.js");
    const kbeTypes = loadModule(compiledPath, "KBETypes.js");
    const transport = loadModule(compiledPath, "WebSocketTransport.js");
    const heartbeat = loadModule(compiledPath, "HeartbeatState.js");

    assertCondition(engine.MemoryStream === memory.MemoryStream, "MemoryStream re-export changed class identity");
    assertCondition(engine.NetworkInterface === network.default, "NetworkInterface re-export changed class identity");
    assertCondition(engine.DataTypes === dataTypes.DataTypes, "DataTypes compatibility re-export changed namespace identity");
    assertCondition(engine.KBETypes === kbeTypes, "KBETypes compatibility re-export changed module identity");
    assertCondition(engine.KBEChannel === transport.KBEChannel, "KBEChannel compatibility re-export changed enum identity");
    assertCondition(engine.KBEPlatform === transport.KBEPlatform, "KBEPlatform compatibility re-export changed enum identity");
    assertCondition(engine.HeartbeatState === heartbeat.HeartbeatState,
        "HeartbeatState compatibility re-export changed class identity");
    assertCondition(engine.DataTypes.KB_INT64 === int64.KB_INT64, "KB_INT64 compatibility alias changed class identity");
    assertCondition(engine.DataTypes.KB_UINT64 === int64.KB_UINT64, "KB_UINT64 compatibility alias changed class identity");

    const stream = new memory.MemoryStream(16);
    stream.WriteInt64(int64.KB_INT64.fromBigInt(-1234567890123n));
    stream.WriteUint64(int64.KB_UINT64.fromBigInt(1234567890123n));
    stream.rpos = 0;
    assertCondition(stream.ReadInt64().toBigInt() === -1234567890123n, "Signed 64-bit protocol roundtrip changed");
    assertCondition(stream.ReadUint64().toBigInt() === 1234567890123n, "Unsigned 64-bit protocol roundtrip changed");
}

function verifySchemaBinding(compiledPath)
{
    const EntityDef = loadModule(compiledPath, "EntityDef.js").default;
    const dataTypes = loadModule(compiledPath, "DataTypes.js").DataTypes;
    const originalWarn = console.warn;

    // 未注册业务实体脚本的 warning 与 schema 类型绑定无关，只在本夹具的初始化窗口内静默。
    // Warnings about unregistered project entity scripts are unrelated to schema binding and are silenced only during fixture initialization.
    console.warn = () => {};
    try
    {
        EntityDef.reset();
    }
    finally
    {
        console.warn = originalWarn;
    }

    const customArray = EntityDef.datatypes.get("AVATAR_LIST");
    assertCondition(EntityDef.datatypes.size > 0, "EntityDef did not initialize its data type registry");
    assertCondition(customArray instanceof dataTypes.DATATYPE_BASE,
        "Custom schema data type does not inherit the canonical DataTypes base");
}

function verifyAppDispatchLifecycle(compiledPath)
{
    const engine = loadModule(compiledPath, "KBEngine.js");
    const Messages = loadModule(compiledPath, "Messages.js").default;
    const injectedTransport = createTransport();
    const replacementTransport = createTransport();
    const injectedTransports = [injectedTransport, replacementTransport];
    let injectedFactoryCalls = 0;
    let lifecycleCallbacks;
    let lifecycleIntervals;
    let lifecycleStops = 0;
    let heartbeatNow = 1000;
    const originalDebug = console.debug;
    const originalWarn = console.warn;
    let app;

    // 生成 SDK 未注册项目实体脚本时会输出预期 warning；这里只静默生命周期夹具噪声，error 仍保持可见。
    // A generated SDK warns before project entity scripts are registered; silence only lifecycle fixture noise while keeping errors visible.
    console.debug = () => {};
    console.warn = () => {};
    try
    {
        const args = new engine.KBEngineArgs();
        args.channel = engine.KBEChannel.Douyin;
        args.webSocketTransportFactory = {
            create()
            {
                injectedFactoryCalls += 1;
                return injectedTransports[injectedFactoryCalls - 1];
            }
        };
        args.hostLifecycleAdapter = {
            start(callbacks, intervals)
            {
                lifecycleCallbacks = callbacks;
                lifecycleIntervals = intervals;
            },
            stop()
            {
                lifecycleStops += 1;
            }
        };
        args.heartbeatClock = { now: () => heartbeatNow };
        app = engine.KBEngineApp.Create(args);
        assertCondition(Messages.RequireDispatchTarget("Client_onAppActiveTickCB") === app,
            "KBEngineApp.Create did not bind the message dispatch target");
        assertCondition(lifecycleCallbacks !== undefined && lifecycleIntervals.updateMS === args.updateTick &&
            lifecycleIntervals.updatePlayerMS === args.syncPlayerMS && lifecycleIntervals.processEventsMS === 50,
            "KBEngineApp did not configure the injected host lifecycle adapter");
        lifecycleCallbacks.pause();
        heartbeatNow = 500000;
        lifecycleCallbacks.resume();
        assertCondition(app.heartbeatState.isDue(heartbeatNow) && !app.heartbeatState.replyPending,
            "Host resume did not clear pending state and schedule an immediate heartbeat");
        app.networkInterface.ConnectTo("ws://injected");
        assertCondition(injectedFactoryCalls === 1,
            "KBEngineArgs explicit transport factory did not override the selected channel");
        injectedTransport.isOpen = true;
        injectedTransport.sendThrows = "expected synchronous send failure";
        lifecycleCallbacks.update();
        assertCondition(!app.heartbeatState.replyPending && injectedTransport.sent.length === 0 &&
            injectedTransport.released && injectedTransport.closed.length === 1,
            "A synchronously rejected heartbeat entered pending state or retained its failed transport");

        app.networkInterface.ConnectTo("ws://replacement");
        replacementTransport.isOpen = true;
        replacementTransport.onOpen({});
        lifecycleCallbacks.processEvents();
        assertCondition(app.networkInterface.IsGood && injectedFactoryCalls === 2,
            "A queued old transport error closed the replacement connection");
        heartbeatNow += 15001;
        lifecycleCallbacks.update();
        assertCondition(app.heartbeatState.replyPending && replacementTransport.sent.length === 1,
            "An accepted heartbeat did not enter pending state");
        app.Client_onAppActiveTickCB();
        assertCondition(!app.heartbeatState.replyPending, "Heartbeat acknowledgement did not clear pending state");

        heartbeatNow += 15001;
        lifecycleCallbacks.update();
        heartbeatNow += 15001;
        lifecycleCallbacks.update();
        assertCondition(replacementTransport.released && replacementTransport.closed.length === 1,
            "An unanswered heartbeat did not close the transport exactly once");
    }
    finally
    {
        // 失败路径也必须清理定时器和派发目标，否则测试进程会掩盖真正断言并持续驻留。
        // Failure paths must also release timers and the dispatch target or the test process can hide the assertion by staying alive.
        if(engine.KBEngineApp.app !== undefined)
            engine.KBEngineApp.Destroy();
        console.debug = originalDebug;
        console.warn = originalWarn;
    }
    assertCondition(lifecycleStops === 1, "KBEngineApp.Destroy did not stop the host lifecycle adapter exactly once");

    let destroyUnbound = false;
    try
    {
        Messages.RequireDispatchTarget("Client_onAppActiveTickCB");
    }
    catch(_error)
    {
        destroyUnbound = true;
    }
    assertCondition(destroyUnbound, "KBEngineApp.Destroy did not unbind the message dispatch target");

    let failedLifecycleStops = 0;
    const failedArgs = new engine.KBEngineArgs();
    failedArgs.hostLifecycleAdapter = {
        start()
        {
            throw new Error("expected lifecycle startup failure");
        },
        stop()
        {
            failedLifecycleStops += 1;
        }
    };
    let startupRejected = false;
    try
    {
        engine.KBEngineApp.Create(failedArgs);
    }
    catch(_error)
    {
        startupRejected = true;
    }
    assertCondition(startupRejected && failedLifecycleStops === 1 && engine.KBEngineApp.app === undefined,
        "Failed lifecycle startup retained host resources or the KBEngineApp singleton");

    let failedCreateUnbound = false;
    try
    {
        Messages.RequireDispatchTarget("Client_onAppActiveTickCB");
    }
    catch(_error)
    {
        failedCreateUnbound = true;
    }
    assertCondition(failedCreateUnbound, "Failed KBEngineApp creation retained the message dispatch target");

    let retryStops = 0;
    const retryArgs = new engine.KBEngineArgs();
    retryArgs.hostLifecycleAdapter = {
        start()
        {
        },
        stop()
        {
            retryStops += 1;
        }
    };
    engine.KBEngineApp.Create(retryArgs);
    engine.KBEngineApp.Destroy();
    assertCondition(retryStops === 1 && engine.KBEngineApp.app === undefined,
        "KBEngineApp could not be created and destroyed after a lifecycle startup failure");

    const stopFailureArgs = new engine.KBEngineArgs();
    stopFailureArgs.hostLifecycleAdapter = {
        start()
        {
        },
        stop()
        {
            throw new Error("expected lifecycle stop failure");
        }
    };
    engine.KBEngineApp.Create(stopFailureArgs);
    let stopRejected = false;
    try
    {
        engine.KBEngineApp.Destroy();
    }
    catch(_error)
    {
        stopRejected = true;
    }
    assertCondition(stopRejected && engine.KBEngineApp.app === undefined,
        "Lifecycle stop failure retained the KBEngineApp singleton");

    let failedDestroyUnbound = false;
    try
    {
        Messages.RequireDispatchTarget("Client_onAppActiveTickCB");
    }
    catch(_error)
    {
        failedDestroyUnbound = true;
    }
    assertCondition(failedDestroyUnbound, "Lifecycle stop failure retained the message dispatch target");
}

function main()
{
    const sdkPath = process.argv[2] ? path.resolve(process.argv[2]) : "";
    const compiledPath = process.argv[3] ? path.resolve(process.argv[3]) : path.join(sdkPath, "dist");
    assertCondition(sdkPath.length > 0 && fs.existsSync(sdkPath), "Usage: node Program.js <generated-sdk-path> [compiled-sdk-path]");
    verifySourceDependencies(sdkPath);
    verifyRuntimeIdentity(compiledPath);
    verifySchemaBinding(compiledPath);
    verifyMessageDispatch(compiledPath);
    verifyTransportLifecycle(compiledPath);
    verifyNetworkCloseLifecycle(compiledPath);
    verifyNetworkSendBackpressure(compiledPath);
    verifyBundleSendResult(compiledPath);
    verifyAppDispatchLifecycle(compiledPath);
    console.log("TYPESCRIPT_MODULE_TEST_PASS root=true imports=true schema=true schema-bind=true memory=true network=true transport=true stale-transport=true generation=true reused=true close-idempotent=true async-send-close=true preopen=true reentrant-open=true int64=true roundtrip=true dispatch=true lifecycle=true app-lifecycle=true heartbeat=true send-result=true bundle-atomic=true large-send=true backpressure=true");
}

try
{
    main();
}
catch(error)
{
    console.error("TYPESCRIPT_MODULE_TEST_FAIL error=" + String(error));
    process.exitCode = 1;
}
