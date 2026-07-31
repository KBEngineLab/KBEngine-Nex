"use strict";

const fs = require("fs");
const path = require("path");

function delay(milliseconds) {
    return new Promise(resolve => setTimeout(resolve, milliseconds));
}

function writeJson(filePath, value) {
    // Windows does not replace a file that another process has just opened for
    // reading. A short partial JSON is tolerated by the controller, so write
    // directly and let the next tick publish the complete state.
    // Windows 不能替换刚被其他进程打开读取的文件；控制器会忽略短暂的
    // 半截 JSON，因此直接写入，下一 tick 会发布完整状态。
    fs.writeFileSync(filePath, `${JSON.stringify(value)}\n`, "utf8");
}

function readCommand(filePath, lastSequence) {
    try {
        const command = JSON.parse(fs.readFileSync(filePath, "utf8"));
        return Number.isInteger(command.sequence) && command.sequence > lastSequence ? command : null;
    }
    catch (error) {
        if (error && error.code === "ENOENT")
            return null;
        throw error;
    }
}

async function main() {
    if (process.argv.length !== 9) {
        throw new Error(
            "usage: authenticated_cell_rpc_probe.js <client-root> <host> <port> " +
            "<username> <avatar-name> <state-file> <command-file>"
        );
    }

    const clientRoot = path.resolve(process.argv[2]);
    const host = process.argv[3];
    const port = Number(process.argv[4]);
    const username = process.argv[5];
    const avatarName = process.argv[6];
    const stateFile = path.resolve(process.argv[7]);
    const commandFile = path.resolve(process.argv[8]);
    if (!Number.isInteger(port) || port <= 0 || port > 65535)
        throw new Error(`invalid LoginApp port: ${process.argv[4]}`);

    // Each role runs in a separate Node process because the generated SDK owns
    // a process-wide KBEngineApp singleton. This preserves real independent
    // authenticated Channels instead of simulating multiple principals.
    // 生成 SDK 使用进程级 KBEngineApp 单例，因此每个角色运行在独立 Node 进程中，
    // 以保留真实且相互独立的认证 Channel，而不是在单例内模拟多个主体。
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

    let failure = null;
    let avatarRequestStarted = false;

    class Account extends AccountBase {
        __init__() {
            super.__init__();
            if (this.IsPlayer() && !avatarRequestStarted) {
                avatarRequestStarted = true;
                this.baseEntityCall.reqAvatarList();
            }
        }

        onReqAvatarList(avatars) {
            if (avatars.length === 0)
                this.baseEntityCall.reqCreateAvatar(avatarName);
            else
                this.baseEntityCall.reqAvatarEnterGame(avatars[0].dbid);
        }

        onReqCreateAvatar(resultCode, avatars) {
            if (avatars.length === 0) {
                failure = `avatar creation returned code ${resultCode} without an Avatar`;
                return;
            }
            this.baseEntityCall.reqAvatarEnterGame(avatars[0].dbid);
        }

        onEnter(scene) {
            console.log(`CELL_RPC_PROBE_ACCOUNT_ENTER scene=${scene}`);
        }

        onReqRemoveAvatar() {
        }
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
    args.syncPlayerMS = 1000;

    const engine = KBEngineApp.Create(args);
    let lastSequence = 0;
    let stopping = false;
    try {
        engine.Login(username, "security-cell-rpc-probe", new Uint8Array(0));
        const deadline = Date.now() + 45000;
        while (!stopping && Date.now() < deadline) {
            if (failure !== null)
                throw new Error(failure);

            const player = engine.Player();
            if (player && player.inWorld && engine.spaceID > 0 && engine.networkInterface.IsGood) {
                const visibleAvatarIds = Object.values(engine.entities)
                    .filter(entity => entity.className === "Avatar" && entity.id !== player.id)
                    .map(entity => entity.id)
                    .sort((left, right) => left - right);
                writeJson(stateFile, {
                    entityID: player.id,
                    spaceID: engine.spaceID,
                    position: [player.position.x, player.position.y, player.position.z],
                    visibleAvatarIds,
                    lastSequence,
                });

                const command = readCommand(commandFile, lastSequence);
                if (command !== null) {
                    if (command.action === "rpc") {
                        const targetID = Number(command.targetID);
                        if (!Number.isInteger(targetID) || targetID <= 0)
                            throw new Error(`invalid RPC target: ${command.targetID}`);
                        const bundle = new Bundle();
                        bundle.NewMessage(Messages.messages["Baseapp_onRemoteCallCellMethodFromClient"]);
                        bundle.WriteUint32(targetID);
                        bundle.WriteUint16(0);
                        bundle.WriteUint16(17);
                        bundle.Send(engine.networkInterface);
                        console.log(
                            `CELL_RPC_PROBE_SENT source=${player.id} target=${targetID} sequence=${command.sequence}`
                        );
                    }
                    else if (command.action === "move") {
                        if (!Array.isArray(command.position) || command.position.length !== 3 ||
                            command.position.some(value => !Number.isFinite(value))) {
                            throw new Error("move command requires three finite coordinates");
                        }
                        [player.position.x, player.position.y, player.position.z] = command.position;
                        engine.UpdatePlayerToServer();
                        console.log(
                            `CELL_RPC_PROBE_MOVED entity=${player.id} sequence=${command.sequence}`
                        );
                    }
                    else if (command.action === "stop") {
                        stopping = true;
                    }
                    else {
                        throw new Error(`unknown command action: ${command.action}`);
                    }
                    lastSequence = command.sequence;
                }
            }

            await delay(50);
        }

        if (!stopping)
            throw new Error("Avatar did not complete the relationship probe within 45 seconds");
    }
    finally {
        KBEngineApp.Destroy();
        await delay(100);
    }
}

main().catch(error => {
    console.error(`CELL_RPC_PROBE_FAIL error=${error instanceof Error ? error.stack || error.message : String(error)}`);
    process.exitCode = 1;
});
