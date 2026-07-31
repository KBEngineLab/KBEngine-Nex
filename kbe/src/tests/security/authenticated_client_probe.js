"use strict";

const path = require("path");

function fail(message) {
    console.error(`AUTHENTICATED_CLIENT_PROBE_FAIL error=${message}`);
    process.exitCode = 1;
}

async function delay(milliseconds) {
    await new Promise(resolve => setTimeout(resolve, milliseconds));
}

async function main() {
    if (process.argv.length !== 5)
        throw new Error("usage: authenticated_client_probe.js <client-root> <host> <port>");

    const clientRoot = path.resolve(process.argv[2]);
    const host = process.argv[3];
    const port = Number(process.argv[4]);
    if (!Number.isInteger(port) || port <= 0 || port > 65535)
        throw new Error(`invalid LoginApp port: ${process.argv[4]}`);

    // Reuse the generated SDK's real WebSocket, handshake, protocol import and
    // LoginApp-to-BaseApp handoff. The probe only crafts the final guarded call.
    // 复用生成 SDK 的真实 WebSocket、握手、协议导入与 LoginApp 到 BaseApp 跳转；
    // 测试只构造最后一个需要验证主体绑定的请求。
    global.WebSocket = require(path.join(clientRoot, "node_modules", "ws"));
    const sdkRoot = path.join(clientRoot, "dist", "typescript-e2e-generated-v3");
    const { KBEngineApp, KBEngineArgs, Bundle } = require(path.join(sdkRoot, "KBEngine.js"));
    const Messages = require(path.join(sdkRoot, "Messages.js")).default;
    const ExportEntity = require(path.join(sdkRoot, "ExportEntity.js"));
    const { AccountBase } = require(path.join(sdkRoot, "AccountBase.js"));
    const { AvatarBase } = require(path.join(sdkRoot, "AvatarBase.js"));
    const { MotionBase } = require(path.join(sdkRoot, "MotionBase.js"));
    const { MonsterBase } = require(path.join(sdkRoot, "MonsterBase.js"));
    const { NPCBase } = require(path.join(sdkRoot, "NPCBase.js"));

    class Account extends AccountBase {
    }
    class Avatar extends AvatarBase {
    }
    class Motion extends MotionBase {
    }
    class Monster extends MonsterBase {
    }
    class NPC extends NPCBase {
    }

    ExportEntity.RegisterScript("Account", Account);
    ExportEntity.RegisterScript("Avatar", Avatar);
    ExportEntity.RegisterScript("Motion", Motion);
    ExportEntity.RegisterScript("Monster", Monster);
    ExportEntity.RegisterScript("NPC", NPC);

    const args = new KBEngineArgs();
    args.address = host === "0.0.0.0" || host === "::" ? "127.0.0.1" : host;
    args.port = port;
    args.clientType = 5;
    args.updateTick = 100;

    const password = "security-probe-password";
    const engine = KBEngineApp.Create(args);
    try {
        engine.Login("security_authenticated_probe", password, new Uint8Array(0));

        const deadline = Date.now() + 30000;
        while ((engine.currserver !== "baseapp" || engine.entity_id <= 0 ||
                !engine.networkInterface.IsGood) && Date.now() < deadline) {
            await delay(20);
        }

        if (engine.currserver !== "baseapp" || engine.entity_id <= 0 ||
            !engine.networkInterface.IsGood) {
            throw new Error("authenticated BaseApp proxy was not created within 30 seconds");
        }

        const proxyID = engine.entity_id;
        const requestedID = proxyID === 0x7fffffff ? proxyID - 1 : proxyID + 1;
        const bundle = new Bundle();
        bundle.NewMessage(Messages.messages["Baseapp_reqAccountBindEmail"]);
        bundle.WriteInt32(requestedID);
        bundle.WriteString(password);
        bundle.WriteString("security-probe@example.invalid");
        bundle.Send(engine.networkInterface);

        console.log(`AUTHENTICATED_CLIENT_PROBE_SENT requested=${requestedID} proxyID=${proxyID}`);
        await delay(500);
    }
    finally {
        KBEngineApp.Destroy();
        await delay(100);
    }
}

main().catch(error => fail(error instanceof Error ? error.stack || error.message : String(error)));
