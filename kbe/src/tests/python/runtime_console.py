"""连接本次测试进程的 Python 调试控制台，不提供通用远程命令入口。"""

from __future__ import annotations

import json
import re
import socket
import time
import uuid


class RuntimeConsole:
    def __init__(self, host: str, port: int, password: str, component_id: int, pid: int):
        self.socket = socket.create_connection((host, port), timeout=5)
        self.socket.settimeout(5)
        phase = "读取登录提示"
        try:
            welcome = self._receive_until(lambda data: b"password:" in data or b">>> " in data)
            if b"password:" in welcome:
                phase = "等待认证结果"
                self.socket.sendall(password.encode("utf-8") + b"\r\n")
                welcome = self._receive_until(lambda data: b">>> " in data)
            phase = "核对进程身份"
            validate_console_identity(welcome, component_id, pid)
        except (OSError, RuntimeError) as exc:
            self.close()
            raise RuntimeError(f"控制台 {component_id}/{pid} {phase}失败：{exc}") from exc
        except BaseException:
            self.close()
            raise

    def _receive_until(self, ready, timeout: float = 5.0) -> bytes:
        deadline = time.monotonic() + timeout
        data = bytearray()
        self.last_response = b""
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                chunk = self.socket.recv(8192)
            except socket.timeout as exc:
                self.last_response = bytes(data)
                raise TimeoutError(f"控制台响应超时，已收到 {len(data)} 字节") from exc
            if not chunk:
                raise ConnectionError("测试控制台在返回结果前断开")
            data.extend(chunk)
            self.last_response = bytes(data)
            if len(data) > 512 * 1024:
                raise RuntimeError("测试控制台响应超过允许长度")
            if ready(data):
                return bytes(data)
        raise TimeoutError("等待测试控制台响应超时")

    def evaluate(self, expression: str):
        # 每次结果都有独立标记；只接受单独一行的结果，不把回显的命令当作执行结果。
        marker = f"KBE_RUNTIME_{uuid.uuid4().hex}:"
        command = f"import json, KBEngine; print('\\n{marker}' + json.dumps({expression}))\r\n"
        self.socket.sendall(command.encode("utf-8"))
        try:
            data = self._receive_until(
                lambda current: result_line(current, marker) is not None
                or b"Traceback (most recent call last):" in current
            )
        except TimeoutError as exc:
            # 只有无凭据的检查表达式会走此处；认证失败不输出收到的内容。
            raise TimeoutError(f"{exc}，响应末尾：{self.last_response[-2048:]!r}") from exc
        result = result_line(data, marker)
        if result is None:
            raise RuntimeError("测试控制台执行失败，请检查对应组件日志")
        return json.loads(result)

    def close(self):
        self.socket.close()


def validate_console_identity(welcome: bytes, component_id: int, pid: int):
    identity = re.search(rb"AppID:\s*(\d+).*?PID:\s*(\d+)", welcome, re.DOTALL)
    if identity is None or tuple(map(int, identity.groups())) != (component_id, pid):
        raise RuntimeError("控制台所属进程与本次集群不一致，拒绝执行")


def result_line(data: bytes, marker: str) -> str | None:
    match = re.search(
        rb"(?:^|[\r\n])" + re.escape(marker.encode("ascii")) + rb"([^\r\n]*)[\r\n]",
        data,
    )
    return match.group(1).decode("utf-8") if match is not None else None
