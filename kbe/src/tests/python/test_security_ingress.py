#!/usr/bin/env python3

import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time


COMPONENT_IDS = {
    "dbmgr": 4000,
    "baseappmgr": 5000,
    "cellappmgr": 6000,
    "interfaces": 3000,
    "baseapp": 7001,
    "cellapp": 8001,
    "loginapp": 9000,
}

REGISTERED_PROBE_COMPONENT_IDS = {
    "dbmgr-registered-wrong-component-type": 9001,
    "dbmgr-registered-sender-channel-mismatch": 9002,
    "dbmgr-registered-invalid-entity-type": 9003,
    "dbmgr-registered-invalid-entity-range": 9004,
    "dbmgr-registered-invalid-db-interface": 9005,
    "dbmgr-registered-invalid-autoload-flag": 9006,
    "dbmgr-registered-truncated-create-account": 9007,
    "dbmgr-registered-invalid-account-type": 9008,
    "dbmgr-registered-oversized-create-account": 9009,
    "dbmgr-registered-truncated-account-login": 9010,
    "dbmgr-registered-oversized-account-login": 9011,
    "dbmgr-registered-truncated-interfaces-callback": 9012,
    "dbmgr-registered-invalid-bind-entity": 9013,
    "dbmgr-registered-invalid-callback-error": 9014,
    "dbmgr-registered-truncated-charge": 9015,
    "dbmgr-registered-zero-charge-dbid": 9016,
    "dbmgr-registered-empty-charge-id": 9017,
    "dbmgr-registered-oversized-charge-id": 9018,
    "dbmgr-registered-truncated-charge-callback": 9019,
    "dbmgr-registered-invalid-charge-error": 9020,
    "dbmgr-registered-empty-erase-key": 9021,
    "dbmgr-registered-oversized-erase-key": 9022,
}

PROBES = (
    (
        "dbmgr-invalid-global-data-type",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::onBroadcastGlobalDataChanged",
        r"Dbmgr::onBroadcastGlobalDataChanged: rejected componentType=6, dataType=255",
    ),
    (
        "dbmgr-zero-entity-dbid",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::removeEntity",
        r"Dbmgr::removeEntity: rejected componentID=7001, entityDBID=0",
    ),
    (
        "dbmgr-unregistered-create-account",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::reqCreateAccount",
        r"Dbmgr::reqCreateAccount: rejected sourceType=loginapp",
    ),
    (
        "dbmgr-unregistered-interfaces-callback",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::onCreateAccountCBFromInterfaces",
        r"Dbmgr::onCreateAccountCBFromInterfaces: rejected sourceType=interfaces",
    ),
    (
        "dbmgr-unregistered-charge",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::charge",
        r"Dbmgr::charge: rejected sourceType=baseapp",
    ),
    (
        "dbmgr-spoofed-id-allocation",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::onReqAllocEntityID",
        r"Dbmgr::onReqAllocEntityID: rejected componentType=6, componentID=7001",
    ),
    (
        "dbmgr-spoofed-account-query",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::queryAccount",
        r"Dbmgr::queryAccount: rejected componentID=7001, entityID=1",
    ),
    (
        "dbmgr-spoofed-raw-database-command",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::executeRawDatabaseCommand",
        r"Dbmgr::executeRawDatabaseCommand: rejected componentType=6, componentID=7001",
    ),
    (
        "dbmgr-truncated-raw-database-command",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::executeRawDatabaseCommand",
        r"Dbmgr::executeRawDatabaseCommand: rejected truncated payload",
    ),
    (
        "dbmgr-truncated-write-entity",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::writeEntity",
        r"Dbmgr::writeEntity: rejected incomplete fixed header",
    ),
    (
        "dbmgr-truncated-remove-entity",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::removeEntity",
        r"Dbmgr::removeEntity: rejected incomplete fixed header",
    ),
    (
        "dbmgr-truncated-delete-entity",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::deleteEntityByDBID",
        r"Dbmgr::deleteEntityByDBID: rejected incomplete fixed header",
    ),
    (
        "dbmgr-truncated-lookup-entity",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::lookUpEntityByDBID",
        r"Dbmgr::lookUpEntityByDBID: rejected incomplete fixed header",
    ),
    (
        "dbmgr-unregistered-stream-template",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::syncEntityStreamTemplate",
        r"Dbmgr::syncEntityStreamTemplate: rejected sourceType=baseapp",
    ),
    (
        "dbmgr-registration-zero-id",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::onRegisterNewApp",
        r"Dbmgr::onRegisterNewApp: rejected registration componentType=13, componentID=0",
    ),
    (
        "dbmgr-registration-live-binding-conflict",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::onRegisterNewApp",
        r"Dbmgr::onRegisterNewApp: rejected registration componentType=13, componentID=3000",
    ),
    (
        "dbmgr-registered-wrong-component-type",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::onReqAllocEntityID",
        r"Dbmgr::onReqAllocEntityID: rejected componentType=13, componentID=9001",
    ),
    (
        "dbmgr-registered-sender-channel-mismatch",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::queryAccount",
        r"Dbmgr::queryAccount: rejected componentID=7001, entityID=4242",
    ),
    (
        "dbmgr-registered-truncated-create-account",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::reqCreateAccount",
        r"Dbmgr::reqCreateAccount: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-invalid-account-type",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::reqCreateAccount",
        r"Dbmgr::reqCreateAccount: rejected fields, .*accountType=255",
    ),
    (
        "dbmgr-registered-oversized-create-account",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::reqCreateAccount",
        r"Dbmgr::reqCreateAccount: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-truncated-account-login",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::onAccountLogin",
        r"Dbmgr::onAccountLogin: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-oversized-account-login",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::onAccountLogin",
        r"Dbmgr::onAccountLogin: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-truncated-interfaces-callback",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::onCreateAccountCBFromInterfaces",
        r"Dbmgr::onCreateAccountCBFromInterfaces: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-invalid-callback-error",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::onCreateAccountCBFromInterfaces",
        r"Dbmgr::onCreateAccountCBFromInterfaces: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-invalid-bind-entity",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::accountReqBindMail",
        r"Dbmgr::accountReqBindMail: rejected fields, entityID=0",
    ),
    (
        "dbmgr-registered-truncated-charge",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::charge",
        r"Dbmgr::charge: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-zero-charge-dbid",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::charge",
        r"Dbmgr::charge: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-empty-charge-id",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::charge",
        r"Dbmgr::charge: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-oversized-charge-id",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::charge",
        r"Dbmgr::charge: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-truncated-charge-callback",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::onChargeCB",
        r"Dbmgr::onChargeCB: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-invalid-charge-error",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::onChargeCB",
        r"Dbmgr::onChargeCB: rejected malformed or oversized payload",
    ),
    (
        "dbmgr-registered-empty-erase-key",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::eraseClientReq",
        r"Dbmgr::eraseClientReq: rejected keySize=0",
    ),
    (
        "dbmgr-registered-oversized-erase-key",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::eraseClientReq",
        r"Dbmgr::eraseClientReq: rejected keySize=129",
    ),
    (
        "dbmgr-spoofed-active-tick",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::onAppActiveTick",
        r"ServerApp::onAppActiveTick: rejected componentType=6, componentID=7001",
    ),
    (
        "dbmgr-spoofed-kill-request",
        "dbmgr",
        "internal",
        "DBMGR_TYPE",
        "Dbmgr::reqKillServer",
        r"ServerApp::reqKillServer: rejected componentType=6, componentID=7001",
    ),
    (
        "baseappmgr-zero-forward",
        "baseappmgr",
        "internal",
        "BASEAPPMGR_TYPE",
        "Baseappmgr::forwardMessage",
        r"Baseappmgr::forwardMessage: rejected unbound senderComponent\(0\)",
    ),
    (
        "baseappmgr-spoofed-update",
        "baseappmgr",
        "internal",
        "BASEAPPMGR_TYPE",
        "Baseappmgr::updateBaseapp",
        r"Baseappmgr::updateBaseapp: rejected componentID\(7001\)",
    ),
    (
        "cellappmgr-zero-forward",
        "cellappmgr",
        "internal",
        "CELLAPPMGR_TYPE",
        "Cellappmgr::forwardMessage",
        r"Cellappmgr::forwardMessage: rejected unbound senderComponent\(0\)",
    ),
    (
        "cellappmgr-spoofed-update",
        "cellappmgr",
        "internal",
        "CELLAPPMGR_TYPE",
        "Cellappmgr::updateCellapp",
        r"Cellappmgr::updateCellapp: rejected componentID\(8001\)",
    ),
    (
        "baseapp-spoofed-callback",
        "baseapp",
        "internal",
        "BASEAPP_TYPE",
        "Baseapp::onCreateEntityAnywhereCallback",
        r"onCreateEntityAnywhereCallback: rejected unbound sourceComponent\(7001\)",
    ),
    (
        "baseapp-spoofed-dbmgr-init",
        "baseapp",
        "internal",
        "BASEAPP_TYPE",
        "Baseapp::onDbmgrInitCompleted",
        r"Baseapp::onDbmgrInitCompleted: rejected non-DBMgr source",
    ),
    (
        "baseapp-spoofed-global-data",
        "baseapp",
        "internal",
        "BASEAPP_TYPE",
        "Baseapp::onBroadcastGlobalDataChanged",
        r"EntityApp::onBroadcastGlobalDataChanged: rejected non-DBMgr source",
    ),
    (
        "baseapp-spoofed-baseapp-data",
        "baseapp",
        "internal",
        "BASEAPP_TYPE",
        "Baseapp::onBroadcastBaseAppDataChanged",
        r"Baseapp::onBroadcastBaseAppDataChanged: rejected non-DBMgr source",
    ),
    (
        "baseapp-spoofed-pending-login",
        "baseapp",
        "internal",
        "BASEAPP_TYPE",
        "Baseapp::registerPendingLogin",
        r"Baseapp::registerPendingLogin: rejected non-BaseAppMgr source",
    ),
    (
        "baseapp-spoofed-entity-app-discovery",
        "baseapp",
        "internal",
        "BASEAPP_TYPE",
        "Baseapp::onGetEntityAppFromDbmgr",
        r"Baseapp::onGetEntityAppFromDbmgr: rejected non-DBMgr source",
    ),
    (
        "cellapp-spoofed-teleport",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::reqTeleportToCellAppCB",
        r"reqTeleportToCellAppCB: rejected unbound targetCellappID=8001",
    ),
    (
        "cellapp-spoofed-dbmgr-init",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onDbmgrInitCompleted",
        r"Cellapp::onDbmgrInitCompleted: rejected non-DBMgr source",
    ),
    (
        "cellapp-spoofed-global-data",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onBroadcastGlobalDataChanged",
        r"EntityApp::onBroadcastGlobalDataChanged: rejected non-DBMgr source",
    ),
    (
        "cellapp-spoofed-cellapp-data",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onBroadcastCellAppDataChanged",
        r"Cellapp::onBroadcastCellAppDataChanged: rejected non-DBMgr source",
    ),
    (
        "baseapp-unbound-account-request",
        "baseapp",
        "external",
        "BASEAPP_TYPE",
        "Baseapp::reqAccountBindEmail",
        r"Baseapp::reqAccountBindEmail: rejected unbound entity, requested=1, proxyID=0",
    ),
    (
        "cellapp-unregistered-cell-rpc",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onRemoteCallMethodFromClient",
        r"Cellapp::onRemoteCallMethodFromClient: rejected unregistered source Channel, "
        r"srcEntityID=1, targetID=2",
    ),
    (
        "cellapp-truncated-cell-rpc",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onRemoteCallMethodFromClient",
        r"Cellapp::onRemoteCallMethodFromClient: rejected incomplete fixed header",
    ),
    (
        "cellapp-truncated-entity-forward",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::forwardEntityMessageToCellappFromClient",
        r"Cellapp::forwardEntityMessageToCellappFromClient: rejected incomplete fixed header",
    ),
    (
        "cellapp-truncated-client-update",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onUpdateDataFromClient",
        r"Cellapp::onUpdateDataFromClient: rejected incomplete fixed header",
    ),
    (
        "cellapp-truncated-controlled-update",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onUpdateDataFromClientForControlledEntity",
        r"Cellapp::onUpdateDataFromClientForControlledEntity: rejected incomplete fixed header",
    ),
    (
        "cellapp-unregistered-entity-forward",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::forwardEntityMessageToCellappFromClient",
        r"Cellapp::forwardEntityMessageToCellappFromClient: rejected unregistered "
        r"source Channel, srcEntityID=1",
    ),
    (
        "cellapp-spoofed-entity-create",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onCreateCellEntityFromBaseapp",
        r"Cellapp::onCreateCellEntityFromBaseapp: rejected componentID=7001, "
        r"entityType=Account, hasClient=false",
    ),
    (
        "cellapp-spoofed-entity-app-discovery",
        "cellapp",
        "internal",
        "CELLAPP_TYPE",
        "Cellapp::onGetEntityAppFromDbmgr",
        r"Cellapp::onGetEntityAppFromDbmgr: rejected non-DBMgr source",
    ),
    (
        "loginapp-spoofed-dbmgr-init",
        "loginapp",
        "internal",
        "LOGINAPP_TYPE",
        "Loginapp::onDbmgrInitCompleted",
        r"Loginapp::onDbmgrInitCompleted: rejected non-DBMgr source",
    ),
    # BaseApp registration broadcasts discovery state, so keep these stateful probes last.
    # BaseApp 注册会广播发现状态，因此这两个有状态探针必须放在最后。
    (
        "dbmgr-registered-invalid-entity-type",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::entityAutoLoad",
        r"Dbmgr::entityAutoLoad: rejected entity script type ID=65535",
    ),
    (
        "dbmgr-registered-invalid-entity-range",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::entityAutoLoad",
        r"Dbmgr::entityAutoLoad: rejected entity auto-load range, start=-1, end=0",
    ),
    (
        "dbmgr-registered-invalid-db-interface",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::entityAutoLoad",
        r"Dbmgr::entityAutoLoad: rejected dbInterfaceIndex=65535",
    ),
    (
        "dbmgr-registered-invalid-autoload-flag",
        "dbmgr",
        "registered-internal",
        "DBMGR_TYPE",
        "Dbmgr::writeEntity",
        r"Dbmgr::writeEntity: rejected shouldAutoLoad=2",
    ),
)


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Start a real KBEngine cluster and verify fail-closed internal ingress paths."
    )
    parser.add_argument("--cluster-script", type=Path, required=True)
    parser.add_argument("--components", required=True)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--assets-root", type=Path, required=True)
    parser.add_argument("--binary-root", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--startup-timeout-seconds", type=int, default=90)
    args = parser.parse_args()
    if not 10 <= args.startup_timeout_seconds <= 150:
        parser.error("--startup-timeout-seconds must be between 10 and 150")
    return args


def normalized(path):
    return path.expanduser().resolve()


def read_text(path):
    if not path.is_file():
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        # 子进程可能正在以独占共享模式刷新日志；下一轮轮询会重试。
        # A child process may be flushing a log with exclusive sharing; retry on the next poll.
        return ""


def all_logs(run_root, controller_stdout, controller_stderr):
    paths = [controller_stdout, controller_stderr]
    if run_root.is_dir():
        paths.extend(sorted(run_root.rglob("*.log")))
    return "\n".join(read_text(path) for path in paths)


def wait_for_text(process, run_root, controller_stdout, controller_stderr, pattern, timeout, description):
    deadline = time.monotonic() + timeout
    expression = re.compile(pattern, re.IGNORECASE)
    while time.monotonic() < deadline:
        output = all_logs(run_root, controller_stdout, controller_stderr)
        if expression.search(output):
            return output
        result = process.poll()
        if result is not None:
            raise RuntimeError(
                f"cluster controller exited with code {result} while waiting for {description}"
            )
        time.sleep(0.1)
    raise RuntimeError(f"timed out waiting for {description}")


def find_endpoint(log_text, component_name):
    component_id = COMPONENT_IDS[component_name]
    pattern = re.compile(
        rf"componentID:{component_id}\b[^\r\n]*?intaddr:([^,\s]+),\s*intport:(\d+)",
        re.IGNORECASE,
    )
    match = pattern.search(log_text)
    if not match:
        raise RuntimeError(f"internal endpoint for {component_name}({component_id}) was not published")
    host = match.group(1)
    port = int(match.group(2))
    if not 1 <= port <= 65535:
        raise RuntimeError(f"invalid internal port for {component_name}: {port}")
    return host, port


def find_external_endpoint(log_text, component_name):
    component_id = COMPONENT_IDS[component_name]
    pattern = re.compile(
        rf"componentID:{component_id}\b[^\r\n]*?extaddr:([^,\s]+),\s*extport:(\d+)",
        re.IGNORECASE,
    )
    match = pattern.search(log_text)
    if not match:
        raise RuntimeError(
            f"external endpoint for {component_name}({component_id}) was not published"
        )
    host = match.group(1)
    port = int(match.group(2))
    if host.lower() == "nonsupport" or not 1 <= port <= 65535:
        raise RuntimeError(f"invalid external endpoint for {component_name}: {host}:{port}")
    return host, port


def find_component_uid(log_text, component_name):
    component_id = COMPONENT_IDS[component_name]
    pattern = re.compile(
        rf"ServerApp::onRegisterNewApp:\s*uid:(-?\d+).*?"
        rf"componentType:{component_name}.*?componentID:{component_id}\b",
        re.IGNORECASE,
    )
    match = pattern.search(log_text)
    if not match:
        raise RuntimeError(f"uid for {component_name}({component_id}) was not published")
    return int(match.group(1))


def flatten_values(values):
    result = {}
    for key, value in values.items():
        if isinstance(value, dict):
            result.update(flatten_values(value))
        else:
            result[str(key)] = value
    return result


def query_watcher_snapshot(watcher, path, timeout=5):
    watcher.clearWatchData()
    watcher.requireQueryWatcher(path)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and len(watcher.watchData) < 2:
        watcher.processOne(min(0.25, max(deadline - time.monotonic(), 0.01)))
    if len(watcher.watchData) < 2:
        raise TimeoutError(f"Watcher query timed out: {path}")
    return watcher.watchData


def query_message_contract(repository_root, component_type_name, endpoint, message_name):
    # Query the live target's registry instead of duplicating auto-assigned IDs.
    # 从目标进程的实时注册表读取协议，不在测试中复制自动分配的消息 ID。
    tools_root = repository_root / "kbe" / "tools" / "server"
    sys.path.insert(0, str(tools_root))
    watcher = None
    try:
        from pycommon import Define, Watcher

        component_type = getattr(Define, component_type_name)
        watcher = Watcher.Watcher(component_type)
        watcher.connect(*endpoint)
        # Watcher readWatchers resolves directory paths below its ``root`` node;
        # querying a leaf such as ``.../id`` returns an empty snapshot. Query the message directory once,
        # then read both protocol leaves from that snapshot.
        # Watcher 的 readWatchers 从 ``root`` 节点解析目录路径；直接查询 ``.../id`` 会得到空快照。
        # 一次查询消息目录，再从同一快照读取两个协议叶子。
        snapshot = query_watcher_snapshot(
            watcher, f"root/network/messages/{message_name}"
        )
        values = next((item["values"] for item in snapshot if item["type"] == 0), {})
        contract = flatten_values(values)
        if "id" not in contract or "len" not in contract:
            raise RuntimeError(
                f"Watcher did not return id/len for {message_name}: {contract}"
            )
        contract = {field: int(contract[field]) for field in ("id", "len")}
        return contract["id"], contract["len"]
    finally:
        if watcher is not None:
            watcher.close()
        sys.path.pop(0)


def probe_body(probe_case, component_uid):
    if probe_case == "dbmgr-invalid-global-data-type":
        return struct.pack("=BBIi", 255, 1, 0, 6)
    if probe_case == "dbmgr-zero-entity-dbid":
        return struct.pack("=HQiQ", 0, 7001, 1, 0)
    if probe_case in {
        "dbmgr-unregistered-create-account",
        "dbmgr-unregistered-interfaces-callback",
        "dbmgr-unregistered-charge",
    }:
        return b""
    if probe_case == "dbmgr-spoofed-id-allocation":
        return struct.pack("=iQ", 6, 7001)
    if probe_case == "dbmgr-spoofed-account-query":
        return b"a\0b\0" + struct.pack("=BQiQIH", 0, 7001, 1, 0, 0, 0)
    if probe_case == "dbmgr-spoofed-raw-database-command":
        return struct.pack("=iHQiII", -1, 0, 7001, 6, 0, 0)
    if probe_case == "dbmgr-truncated-raw-database-command":
        return struct.pack("=iH", -1, 0)
    if probe_case in {
        "dbmgr-truncated-write-entity",
        "dbmgr-truncated-remove-entity",
        "dbmgr-truncated-delete-entity",
        "dbmgr-truncated-lookup-entity",
    }:
        return b"\0\0"
    if probe_case == "dbmgr-unregistered-stream-template":
        return b""
    if probe_case == "dbmgr-registration-zero-id":
        return component_registration_body(component_uid, 13, 0)
    if probe_case == "dbmgr-registration-live-binding-conflict":
        return component_registration_body(component_uid, 13, 3000)
    if probe_case == "dbmgr-registered-wrong-component-type":
        return struct.pack("=iQ", 13, 9001)
    if probe_case == "dbmgr-registered-sender-channel-mismatch":
        return b"a\0b\0" + struct.pack("=BQiQIH", 0, 7001, 4242, 0, 0, 0)
    if probe_case == "dbmgr-registered-invalid-entity-type":
        return struct.pack("=HQHii", 0, 9003, 65535, 0, 0)
    if probe_case == "dbmgr-registered-invalid-entity-range":
        return struct.pack("=HQHii", 0, 9004, 1, -1, 0)
    if probe_case == "dbmgr-registered-invalid-db-interface":
        return struct.pack("=HQHii", 65535, 9005, 1, 0, 32)
    if probe_case == "dbmgr-registered-invalid-autoload-flag":
        return struct.pack("=QiQHHIb", 9006, 1, 0, 0, 1, 0, 2)
    if probe_case == "dbmgr-registered-truncated-create-account":
        return b"account\0password\0"
    if probe_case == "dbmgr-registered-invalid-account-type":
        return b"account\0password\0" + struct.pack("=BI", 255, 0)
    if probe_case == "dbmgr-registered-oversized-create-account":
        return b"a" * 129 + b"\0password\0" + struct.pack("=BI", 1, 0)
    if probe_case == "dbmgr-registered-truncated-account-login":
        return b"account\0"
    if probe_case == "dbmgr-registered-oversized-account-login":
        return b"a" * 129 + b"\0password\0" + struct.pack("=I", 0)
    if probe_case == "dbmgr-registered-truncated-interfaces-callback":
        return b"\0"
    if probe_case == "dbmgr-registered-invalid-callback-error":
        return (
            struct.pack("=Q", 9000)
            + b"account\0account\0password\0"
            + struct.pack("=HII", 65535, 0, 0)
        )
    if probe_case == "dbmgr-registered-invalid-bind-entity":
        return struct.pack("=i", 0) + b"account\0password\0mail@example.invalid\0"
    if probe_case == "dbmgr-registered-truncated-charge":
        return b"order\0"
    if probe_case == "dbmgr-registered-zero-charge-dbid":
        return b"order\0" + struct.pack("=QII", 0, 0, 1)
    if probe_case == "dbmgr-registered-empty-charge-id":
        return b"\0" + struct.pack("=QII", 1, 0, 1)
    if probe_case == "dbmgr-registered-oversized-charge-id":
        return b"a" * 129 + b"\0" + struct.pack("=QII", 1, 0, 1)
    if probe_case == "dbmgr-registered-truncated-charge-callback":
        return b"\0"
    if probe_case == "dbmgr-registered-invalid-charge-error":
        return struct.pack("=Q", 7001) + b"order\0" + struct.pack("=QIIH", 1, 0, 1, 65535)
    if probe_case == "dbmgr-registered-empty-erase-key":
        return b"\0"
    if probe_case == "dbmgr-registered-oversized-erase-key":
        return b"a" * 129 + b"\0"
    if probe_case == "dbmgr-spoofed-active-tick":
        return struct.pack("=iQ", 6, 7001)
    if probe_case == "dbmgr-spoofed-kill-request":
        return struct.pack("=Qi", 7001, 6) + b"security-probe\0" + struct.pack("=i", 0) + b"probe\0"
    if probe_case == "baseappmgr-zero-forward":
        return struct.pack("=QQ", 0, 7001)
    if probe_case == "baseappmgr-spoofed-update":
        return struct.pack("=QiifI", 7001, 0, 0, float("nan"), 0)
    if probe_case == "cellappmgr-zero-forward":
        return struct.pack("=QQ", 0, 8001)
    if probe_case == "cellappmgr-spoofed-update":
        return struct.pack("=QifI", 8001, 0, float("nan"), 0)
    if probe_case == "baseapp-spoofed-callback":
        return struct.pack("=I", 1) + b"Account\0" + struct.pack("=iQ", 1, 7001)
    if probe_case in {
        "baseapp-spoofed-dbmgr-init",
        "cellapp-spoofed-dbmgr-init",
    }:
        return struct.pack("=Iiiii", 0, 1, 2, 1, 1) + b"invalid\0"
    if probe_case in {
        "baseapp-spoofed-global-data",
        "baseapp-spoofed-baseapp-data",
        "cellapp-spoofed-global-data",
        "cellapp-spoofed-cellapp-data",
    }:
        return struct.pack("=BI", 1, 0)
    if probe_case == "baseapp-spoofed-pending-login":
        return b""
    if probe_case in {
        "baseapp-spoofed-entity-app-discovery",
        "cellapp-spoofed-entity-app-discovery",
    }:
        return (
            struct.pack("=i", 0)
            + b"security-probe\0"
            + struct.pack("=iQiiIHIH", 6, 9003, 0, 0, 0, 0, 0, 0)
            + b"\0"
        )
    if probe_case == "cellapp-spoofed-teleport":
        return struct.pack("=QQQib", 8001, 8001, 7001, 1, 0)
    if probe_case == "baseapp-unbound-account-request":
        return struct.pack("=i", 1) + b"invalid\0security@example.invalid\0"
    if probe_case == "cellapp-unregistered-cell-rpc":
        return struct.pack("=ii", 1, 2)
    if probe_case in {
        "cellapp-truncated-cell-rpc",
        "cellapp-truncated-entity-forward",
        "cellapp-truncated-client-update",
        "cellapp-truncated-controlled-update",
    }:
        return b""
    if probe_case == "cellapp-unregistered-entity-forward":
        return struct.pack("=i", 1)
    if probe_case == "cellapp-spoofed-entity-create":
        return struct.pack("=i", 1) + b"Account\0" + struct.pack("=iQBB", 2, 7001, 0, 0)
    if probe_case == "loginapp-spoofed-dbmgr-init":
        return struct.pack("=ii", 1, 1) + b"invalid\0"
    raise RuntimeError(f"unsupported probe case: {probe_case}")


def message_frame(message_id, message_length, body, message_name):
    frame = struct.pack("=H", message_id)
    if message_length == -1:
        if len(body) > 65534:
            raise RuntimeError(f"probe body is too large: {message_name}")
        frame += struct.pack("=H", len(body))
    elif message_length != len(body):
        raise RuntimeError(
            f"probe contract mismatch for {message_name}: "
            f"target={message_length} body={len(body)}"
        )
    return frame + body


def component_registration_body(component_uid, component_type, component_id, endpoint=None):
    intaddr = 0
    intport = 0
    if endpoint is not None:
        intaddr = struct.unpack("=I", socket.inet_aton(endpoint[0]))[0]
        intport = socket.htons(endpoint[1])
    return (
        struct.pack("=i", component_uid)
        + b"security-probe\0"
        + struct.pack(
            "=iQiiIHIH", component_type, component_id, 0, 0, intaddr, intport, 0, 0
        )
        + b"\0"
    )


def registered_probe_component_type(probe_case):
    if probe_case in {
        "dbmgr-registered-invalid-entity-type",
        "dbmgr-registered-invalid-entity-range",
        "dbmgr-registered-invalid-db-interface",
        "dbmgr-registered-invalid-autoload-flag",
        "dbmgr-registered-invalid-bind-entity",
        "dbmgr-registered-truncated-charge",
        "dbmgr-registered-zero-charge-dbid",
        "dbmgr-registered-empty-charge-id",
        "dbmgr-registered-oversized-charge-id",
    }:
        return 6
    if probe_case in {
        "dbmgr-registered-truncated-create-account",
        "dbmgr-registered-invalid-account-type",
        "dbmgr-registered-oversized-create-account",
        "dbmgr-registered-truncated-account-login",
        "dbmgr-registered-oversized-account-login",
        "dbmgr-registered-empty-erase-key",
        "dbmgr-registered-oversized-erase-key",
    }:
        return 2
    return 13


def component_type_label(component_type):
    return {2: "loginapp", 6: "baseapp", 13: "interfaces"}[component_type]


def run_probe(
    args,
    probe_case,
    component_type_name,
    message_name,
    contract_endpoint,
    ingress_endpoint,
    component_uid,
):
    message_id, message_length = query_message_contract(
        args.repository_root, component_type_name, contract_endpoint, message_name
    )
    frame = message_frame(
        message_id, message_length, probe_body(probe_case, component_uid), message_name
    )

    with socket.create_connection(ingress_endpoint, timeout=5) as connection:
        connection.sendall(frame)
        connection.shutdown(socket.SHUT_WR)
        time.sleep(0.05)


def run_registered_probe(
    args,
    process,
    run_root,
    controller_stdout,
    controller_stderr,
    probe_case,
    component_type_name,
    message_name,
    contract_endpoint,
    ingress_endpoint,
    registration_endpoint,
    rejection_pattern,
    component_uid,
):
    registration_id, registration_length = query_message_contract(
        args.repository_root, component_type_name, contract_endpoint, "Dbmgr::onRegisterNewApp"
    )
    message_id, message_length = query_message_contract(
        args.repository_root, component_type_name, contract_endpoint, message_name
    )
    component_id = REGISTERED_PROBE_COMPONENT_IDS[probe_case]
    component_type = registered_probe_component_type(probe_case)
    registration_frame = message_frame(
        registration_id,
        registration_length,
        component_registration_body(
            component_uid, component_type, component_id, registration_endpoint
        ),
        "Dbmgr::onRegisterNewApp",
    )
    probe_frame = message_frame(
        message_id, message_length, probe_body(probe_case, component_uid), message_name
    )

    with socket.create_connection(ingress_endpoint, timeout=5) as connection:
        connection.sendall(registration_frame)
        wait_for_text(
            process,
            run_root,
            controller_stdout,
            controller_stderr,
            rf"ServerApp::onRegisterNewApp:.*componentType:{component_type_label(component_type)}.*componentID:{component_id}",
            10,
            f"registration marker for {probe_case}",
        )
        connection.sendall(probe_frame)
        wait_for_text(
            process,
            run_root,
            controller_stdout,
            controller_stderr,
            rejection_pattern,
            10,
            f"rejection marker for {probe_case}",
        )


def run_test(args):
    args.cluster_script = normalized(args.cluster_script)
    args.repository_root = normalized(args.repository_root)
    args.assets_root = normalized(args.assets_root)
    args.binary_root = normalized(args.binary_root)
    args.run_root = normalized(args.run_root)
    if not args.cluster_script.is_file():
        raise RuntimeError(f"required cluster script is missing: {args.cluster_script}")

    stop_file = args.run_root / "security-probes-complete.stop"
    controller_stdout = args.run_root.parent / f"{args.run_root.name}.controller.stdout.log"
    controller_stderr = args.run_root.parent / f"{args.run_root.name}.controller.stderr.log"
    for path in (controller_stdout, controller_stderr):
        if path.exists():
            path.unlink()

    command = (
        sys.executable,
        "-B",
        str(args.cluster_script),
        "--components",
        args.components,
        "--repository-root",
        str(args.repository_root),
        "--assets-root",
        str(args.assets_root),
        "--binary-root",
        str(args.binary_root),
        "--run-root",
        str(args.run_root),
        "--startup-timeout-seconds",
        str(args.startup_timeout_seconds),
        "--hold-until-file",
        str(stop_file),
    )

    process = None
    stdout_handle = controller_stdout.open("wb")
    stderr_handle = controller_stderr.open("wb")
    try:
        process = subprocess.Popen(
            command,
            cwd=args.repository_root,
            stdin=subprocess.DEVNULL,
            stdout=stdout_handle,
            stderr=stderr_handle,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        ready_logs = wait_for_text(
            process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            r"SERVER_CLUSTER_READY",
            args.startup_timeout_seconds + 30,
            "cluster readiness",
        )

        internal_endpoints = {
            component_name: find_endpoint(ready_logs, component_name)
            for component_name in COMPONENT_IDS
        }
        external_endpoints = {
            component_name: find_external_endpoint(ready_logs, component_name)
            for component_name in {probe[1] for probe in PROBES if probe[2] == "external"}
        }
        component_uid = find_component_uid(ready_logs, "baseapp")
        # Every discovered fake component needs a distinct live endpoint. Real components
        # connect to advertised addresses, and reusing one endpoint creates Channel conflicts.
        # 每个被发现的伪组件都需要独立且存活的端点；真实组件会主动回连，复用地址会造成 Channel 冲突。
        with ExitStack() as registration_listeners:
            for (
                probe_case,
                component_name,
                endpoint_kind,
                component_type,
                message_name,
                rejection_pattern,
            ) in PROBES:
                if endpoint_kind == "registered-internal":
                    registration_listener = registration_listeners.enter_context(
                        socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    )
                    registration_listener.bind((internal_endpoints["dbmgr"][0], 0))
                    registration_listener.listen(64)
                    run_registered_probe(
                        args,
                        process,
                        args.run_root,
                        controller_stdout,
                        controller_stderr,
                        probe_case,
                        component_type,
                        message_name,
                        internal_endpoints[component_name],
                        internal_endpoints[component_name],
                        registration_listener.getsockname(),
                        rejection_pattern,
                        component_uid,
                    )
                    continue

                run_probe(
                    args,
                    probe_case,
                    component_type,
                    message_name,
                    internal_endpoints[component_name],
                    (
                        external_endpoints[component_name]
                        if endpoint_kind == "external"
                        else internal_endpoints[component_name]
                    ),
                    component_uid,
                )
                wait_for_text(
                    process,
                    args.run_root,
                    controller_stdout,
                    controller_stderr,
                    rejection_pattern,
                    10,
                    f"rejection marker for {probe_case}",
                )

            # The cluster controller checks every owned component while held. Keeping
            # it alive across a scheduling interval proves the malformed frames did
            # not terminate a target process. 集群控制器在 hold 状态持续检查全部子进程；
            # 跨过一个调度周期仍存活，证明畸形帧没有终止目标组件。
            time.sleep(0.5)
            if process.poll() is not None:
                raise RuntimeError(f"cluster exited after probes with code {process.returncode}")

            stop_file.touch()
            result = process.wait(timeout=30)
            if result != 0:
                raise RuntimeError(f"cluster cleanup failed with code {result}")
            print(f"SECURITY_MULTIPROCESS_INGRESS_PASS probes={len(PROBES)}", flush=True)
    finally:
        if process is not None and process.poll() is None:
            try:
                stop_file.parent.mkdir(parents=True, exist_ok=True)
                stop_file.touch()
                process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
        stdout_handle.close()
        stderr_handle.close()


def main():
    args = parse_arguments()
    try:
        run_test(args)
        return 0
    except Exception as exception:
        print(
            f"SECURITY_MULTIPROCESS_INGRESS_FAIL error={exception} run_root={args.run_root}",
            file=sys.stderr,
            flush=True,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
