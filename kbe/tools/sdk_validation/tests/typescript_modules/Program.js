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
    const aggregateImport = /from\s+["']\.\/KBEngine["']/;

    // 协议核心和网络实现不能反向依赖兼容聚合入口，否则 re-export 会重新形成运行时初始化环。
    // Protocol core and network implementations must not depend on the compatibility aggregate or its re-exports recreate a runtime initialization cycle.
    assertCondition(!aggregateImport.test(memorySource), "MemoryStream.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(networkSource), "NetworkInterface.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(int64Source), "Int64.ts imports KBEngine.ts");
    assertCondition(!aggregateImport.test(messagesSource), "Messages.ts imports KBEngine.ts");
    assertCondition(!/export\s+class\s+MemoryStream\b/.test(engineSource), "KBEngine.ts still defines MemoryStream");
    assertCondition(!/export\s+class\s+NetworkInterface\b/.test(engineSource), "KBEngine.ts still defines NetworkInterface");
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

function verifyRuntimeIdentity(compiledPath)
{
    const engine = loadModule(compiledPath, "KBEngine.js");
    const memory = loadModule(compiledPath, "MemoryStream.js");
    const int64 = loadModule(compiledPath, "Int64.js");
    const network = loadModule(compiledPath, "NetworkInterface.js");

    assertCondition(engine.MemoryStream === memory.MemoryStream, "MemoryStream re-export changed class identity");
    assertCondition(engine.NetworkInterface === network.default, "NetworkInterface re-export changed class identity");
    assertCondition(engine.DataTypes.KB_INT64 === int64.KB_INT64, "KB_INT64 compatibility alias changed class identity");
    assertCondition(engine.DataTypes.KB_UINT64 === int64.KB_UINT64, "KB_UINT64 compatibility alias changed class identity");

    const stream = new memory.MemoryStream(16);
    stream.WriteInt64(int64.KB_INT64.fromBigInt(-1234567890123n));
    stream.WriteUint64(int64.KB_UINT64.fromBigInt(1234567890123n));
    stream.rpos = 0;
    assertCondition(stream.ReadInt64().toBigInt() === -1234567890123n, "Signed 64-bit protocol roundtrip changed");
    assertCondition(stream.ReadUint64().toBigInt() === 1234567890123n, "Unsigned 64-bit protocol roundtrip changed");
}

function verifyAppDispatchLifecycle(compiledPath)
{
    const engine = loadModule(compiledPath, "KBEngine.js");
    const Messages = loadModule(compiledPath, "Messages.js").default;
    const originalDebug = console.debug;
    const originalWarn = console.warn;
    let app;

    // 生成 SDK 未注册项目实体脚本时会输出预期 warning；这里只静默生命周期夹具噪声，error 仍保持可见。
    // A generated SDK warns before project entity scripts are registered; silence only lifecycle fixture noise while keeping errors visible.
    console.debug = () => {};
    console.warn = () => {};
    try
    {
        app = engine.KBEngineApp.Create(new engine.KBEngineArgs());
        assertCondition(Messages.RequireDispatchTarget("Client_onAppActiveTickCB") === app,
            "KBEngineApp.Create did not bind the message dispatch target");
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
}

function main()
{
    const sdkPath = process.argv[2] ? path.resolve(process.argv[2]) : "";
    const compiledPath = process.argv[3] ? path.resolve(process.argv[3]) : path.join(sdkPath, "dist");
    assertCondition(sdkPath.length > 0 && fs.existsSync(sdkPath), "Usage: node Program.js <generated-sdk-path> [compiled-sdk-path]");
    verifySourceDependencies(sdkPath);
    verifyRuntimeIdentity(compiledPath);
    verifyMessageDispatch(compiledPath);
    verifyAppDispatchLifecycle(compiledPath);
    console.log("TYPESCRIPT_MODULE_TEST_PASS root=true imports=true memory=true network=true int64=true roundtrip=true dispatch=true lifecycle=true app-lifecycle=true");
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
