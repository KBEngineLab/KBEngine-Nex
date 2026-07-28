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
    assertCondition(!/export\s+class\s+MemoryStream\b/.test(engineSource), "KBEngine.ts still defines MemoryStream");
    assertCondition(!/export\s+class\s+NetworkInterface\b/.test(engineSource), "KBEngine.ts still defines NetworkInterface");
    assertCondition(/import\s+type\s+\{\s*MemoryStream\s*\}\s+from\s+["']\.\/MemoryStream["']/.test(messagesSource),
        "Messages.ts does not use a type-only MemoryStream import");
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

function main()
{
    const sdkPath = process.argv[2] ? path.resolve(process.argv[2]) : "";
    const compiledPath = process.argv[3] ? path.resolve(process.argv[3]) : path.join(sdkPath, "dist");
    assertCondition(sdkPath.length > 0 && fs.existsSync(sdkPath), "Usage: node Program.js <generated-sdk-path> [compiled-sdk-path]");
    verifySourceDependencies(sdkPath);
    verifyRuntimeIdentity(compiledPath);
    console.log("TYPESCRIPT_MODULE_TEST_PASS root=true imports=true memory=true network=true int64=true roundtrip=true");
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
