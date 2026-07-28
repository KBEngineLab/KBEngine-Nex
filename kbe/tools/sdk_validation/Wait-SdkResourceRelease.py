#!/usr/bin/env python3

import argparse
import pathlib
import sys
import time


SERVER_TOOLS = pathlib.Path(__file__).resolve().parents[1] / "server"
# 复用现有 watcher 协议实现，避免资源验收维护第二套消息 ID 与二进制解码器。
# Reuse the existing watcher protocol so resource validation does not maintain a second set of message IDs and binary decoders.
sys.path.insert(0, str(SERVER_TOOLS))

from pycommon import Define, Watcher


WATCHER_PATH = "root/network/channels"
RESOURCE_KEYS = (
    "external",
    "externalTcp",
    "externalWebSocket",
    "externalKcp",
    "externalUdp",
    "externalKcpControlBlocks",
    "externalKcpUpdateTimers",
)
WATCHER_COMPONENTS = tuple(
    sorted(name for name, component_type in Define.COMPONENT_NAME2TYPE.items() if component_type in Watcher.CMD_ID_queryWatcher)
)


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Wait until a KBEngine component releases external channels and KCP resources."
    )
    parser.add_argument("--component", choices=WATCHER_COMPONENTS, required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--timeout", default=30.0, type=float)
    parser.add_argument("--poll-interval", default=0.25, type=float)
    parser.add_argument("--report-interval", default=5.0, type=float)
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.poll_interval <= 0:
        parser.error("--poll-interval must be greater than zero")
    if args.report_interval <= 0:
        parser.error("--report-interval must be greater than zero")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")

    component_type = Define.COMPONENT_NAME2TYPE[args.component]
    return args, component_type


def query_channel_resources(watcher, deadline):
    # 每次请求清除上轮响应；查询连接属于内部 Channel，不会污染被验证的 external 计数。
    # Clear the previous response for each request; the watcher connection is internal and does not affect the external count under test.
    watcher.clearWatchData()
    watcher.requireQueryWatcher(WATCHER_PATH)

    while time.monotonic() < deadline:
        watcher.processOne(min(0.2, max(0.0, deadline - time.monotonic())))
        for response in watcher.watchData:
            if response.get("type") != 0:
                continue

            values = response.get("values", {})
            missing = [key for key in RESOURCE_KEYS if key not in values]
            if missing:
                raise RuntimeError("watcher response is missing resources: {}".format(", ".join(missing)))

            return {key: int(values[key]) for key in RESOURCE_KEYS}

    raise TimeoutError("watcher query timed out")


def resources_released(values):
    # 传输分类总和必须与 external 真值一致，否则新增协议未纳入统计时不能静默报告通过。
    # Transport classes must sum to the external source of truth, preventing a newly added protocol from silently passing unaccounted.
    transport_total = (
        values["externalTcp"]
        + values["externalWebSocket"]
        + values["externalKcp"]
        + values["externalUdp"]
    )
    if transport_total != values["external"]:
        raise RuntimeError(
            "external channel classification mismatch: external={}, classified={}".format(
                values["external"], transport_total
            )
        )

    return (
        values["external"] == 0
        and values["externalKcpControlBlocks"] == 0
        and values["externalKcpUpdateTimers"] == 0
    )


def format_sample(component, values):
    return (
        "component={} client_connections={} tcp={} websocket={} kcp={} udp={} "
        "kcp_control_blocks={} kcp_timers={}".format(
            component,
            values["external"],
            values["externalTcp"],
            values["externalWebSocket"],
            values["externalKcp"],
            values["externalUdp"],
            values["externalKcpControlBlocks"],
            values["externalKcpUpdateTimers"],
        )
    )


def main():
    args, component_type = parse_arguments()
    deadline = time.monotonic() + args.timeout
    watcher = Watcher.Watcher(component_type)
    last_values = None
    last_reported_values = None
    next_report_time = 0.0

    try:
        watcher.connect(args.host, args.port)
        while time.monotonic() < deadline:
            last_values = query_channel_resources(watcher, deadline)
            now = time.monotonic()
            # 仅在状态变化或报告周期到达时输出，长超时不会用重复采样淹没 CI 日志。
            # Report only on state changes or at the reporting interval so long timeouts do not flood CI logs with identical samples.
            if last_values != last_reported_values or now >= next_report_time:
                print("RESOURCE_SAMPLE {}".format(format_sample(args.component, last_values)), flush=True)
                last_reported_values = last_values
                next_report_time = now + args.report_interval

            if resources_released(last_values):
                print(
                    "RESOURCE_PASS client_connections=0 server_channels=0 kcp_timers=0 component={}".format(
                        args.component
                    ),
                    flush=True,
                )
                return 0

            time.sleep(min(args.poll_interval, max(0.0, deadline - time.monotonic())))
    except Exception as exc:
        print("RESOURCE_FAIL component={} error={}".format(args.component, exc), file=sys.stderr, flush=True)
        return 1
    finally:
        watcher.close()

    details = "no watcher sample" if last_values is None else format_sample(args.component, last_values)
    print("RESOURCE_FAIL timeout={}s {}".format(args.timeout, details), file=sys.stderr, flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
