# -*- coding: utf-8 -*-

import argparse
import json
import math
import socket
import struct
import sys

from pycommon import Define, Machines, MessageStream
from pycommon.ManagementConfig import resolve_admin_token


MANAGE_SPACE_LAYOUT = 55400
SPACE_LAYOUT_REPLY = 65503


def request_layout(endpoint, token, space, *, split=None, merge=None):
    if not token or len(token) > 512 or "\0" in token or not token.isascii():
        raise ValueError("请在运行配置中设置有效的 management/adminToken")
    message = MessageStream.MessageStreamWriter(MANAGE_SPACE_LAYOUT)
    message.writeString(token)
    action = 1 if split is not None else 2 if merge is not None else 3
    message.writeUint8(action)
    message.writeUint32(space)
    if split is not None:
        version, revision, cell, axis, position = split
        message.writeUint64(version)
        message.writeUint64(revision)
        message.writeUint32(cell)
        message.writeUint8(axis)
        message.writeFloat(position)
    elif merge is not None:
        version, revision, retiring, keeping = merge
        message.writeUint64(version)
        message.writeUint64(revision)
        message.writeUint32(retiring)
        message.writeUint32(keeping)

    with socket.create_connection(endpoint, timeout=5) as sock, sock.makefile("rb") as reply:
        sock.sendall(message.build())
        header = reply.read(4)
        if len(header) != 4:
            raise ValueError("未收到 CellAppMgr 回执，请检查版本、管理地址和鉴权配置")
        msgid, length = struct.unpack("=HH", header)
        if msgid != SPACE_LAYOUT_REPLY or not 2 <= length <= 8192:
            raise ValueError("收到无效的 Space 管理回执")
        body = reply.read(length)
        if len(body) != length:
            raise ValueError("Space 管理回执不完整")
    stream = MessageStream.MessageStreamReader(body)
    accepted = stream.readUint8()
    error = stream.readString()
    if accepted != 1:
        raise ValueError(error or "CellAppMgr 拒绝了请求")
    result = {
        "space": space,
        "version": stream.readUint64(),
        "boundaryRevision": stream.readUint64(),
        "state": stream.readString(),
        "splitAttempt": stream.readUint64(),
        "splitState": stream.readString(),
        "splitError": stream.readString(),
        "migrationState": stream.readString(),
        "cells": [],
    }
    count = stream.readUint16()
    if not 1 <= count <= 127:
        raise ValueError("Space Layout 回执中的节点数无效")

    def read_node(bounds):
        axis = stream.readUint8()
        position = stream.readFloat()
        cell = stream.readUint32()
        cellapp = stream.readUint64()
        if axis == 0:
            result["cells"].append({"cell": cell, "cellapp": cellapp, "bounds": bounds})
        elif axis in (1, 2) and math.isfinite(position):
            low, high = bounds.copy(), bounds.copy()
            low[axis + 1] = high[axis - 1] = position
            read_node(low)
            read_node(high)
        else:
            raise ValueError("Space Layout 回执中的节点无效")

    node_size = struct.calcsize("=BfIQ")
    if stream.length() < count * node_size:
        raise ValueError("Space Layout 回执长度与节点数不一致")
    nodes_end = stream.length() - count * node_size
    read_node([None] * 4)
    if stream.length() != nodes_end:
        raise ValueError("Space Layout 回执中的节点数不匹配")
    if action != 1:
        result["mergeAttempt"] = stream.readUint64()
        result["mergeState"] = stream.readString()
        result["mergeError"] = stream.readString()
    if not stream.EOF():
        raise ValueError("Space Layout 回执包含多余的数据")
    return result


def manager_address(value):
    host, separator, port = value.rpartition(":")
    if not separator or not host or not port.isdigit() or not 1 <= int(port) <= 65535:
        raise argparse.ArgumentTypeError("管理地址格式为 HOST:PORT")
    return host, int(port)


def discover_manager(machine, uid, token):
    machines = Machines.Machines(uid=uid, adminToken=token)
    try:
        machines.queryAllInterfaces(machine, timeout=1)
        addresses = {(info.intaddr, info.intport) for info in machines.getComponentInfos(Define.CELLAPPMGR_TYPE)
                     if info.uid == machines.uid}
    finally:
        machines.stopListen()
    if len(addresses) != 1:
        raise ValueError("未发现唯一的 CellAppMgr，请用 --manager 指定管理地址")
    return addresses.pop()


def uint32(value):
    number = int(value)
    if not 1 <= number <= 0xffffffff:
        raise argparse.ArgumentTypeError("编号必须是 1 到 4294967295 之间的整数")
    return number


def main(argv, default_machine):
    parser = argparse.ArgumentParser(prog="cluster_controller.py space")
    commands = parser.add_subparsers(dest="action", required=True)
    for action in ("show", "split", "merge"):
        command = commands.add_parser(action)
        command.add_argument("--space", type=uint32, required=True)
        command.add_argument("--manager", type=manager_address)
        command.add_argument("--machine", default=default_machine)
        command.add_argument("--uid", type=int)
        if action == "split":
            command.add_argument("--cell", type=uint32, required=True)
            command.add_argument("--axis", type=str.lower, choices=("x", "z"), required=True)
            command.add_argument("--at", type=float, required=True)
        elif action == "merge":
            command.add_argument("--retire", type=uint32, required=True)
            command.add_argument("--keep", type=uint32, required=True)
    args = parser.parse_args(argv)
    try:
        token = resolve_admin_token()
        endpoint = args.manager or discover_manager(args.machine, args.uid, token)
        state = request_layout(endpoint, token, args.space)
        if args.action == "split":
            if not math.isfinite(args.at):
                raise ValueError("切分坐标必须是有限数值")
            state = request_layout(endpoint, token, args.space, split=(
                state["version"], state["boundaryRevision"], args.cell, 1 if args.axis == "x" else 2, args.at,
            ))
        elif args.action == "merge":
            state = request_layout(endpoint, token, args.space, merge=(
                state["version"], state["boundaryRevision"], args.retire, args.keep,
            ))
        print(json.dumps(state, indent=2))
    except (OSError, ValueError, OverflowError, struct.error) as exc:
        print(f"Space 管理操作失败：{exc}", file=sys.stderr)
        return 1
    return 0
