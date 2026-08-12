# -*- coding: utf-8 -*-
"""
Poller 模块 — 基于 KBEngine 文件描述符回调的 TCP 轮询器。

本模块实现了一个简易的、单次响应式的 TCP 服务器。它利用 KBEngine 引擎提供的
文件描述符注册机制（registerAcceptFileDescriptor / registerReadDataFileDescriptor /
writeFileDescriptor）来实现非阻塞的异步 I/O，完全不依赖 threading 或 asyncio，
因此能够与 KBEngine 的主事件循环无缝集成。

工作流程：
    1. 调用 start() 创建监听 socket，注册到 KBEngine 的 accept 回调。
    2. 有新连接时，KBEngine 回调 onAccept()，将客户端 socket 包装并注册读回调。
    3. 客户端发送数据时，KBEngine 回调 onRead()，累积数据直到收到 HTTP 请求结束标志。
    4. 收到完整请求后，调用 processData() 构造 HTTP 响应并通过 writeFileDescriptor 异步写出。
    5. 写出完成后回调 onWriteComplete()，关闭客户端连接。
"""

import socket
import KBEngine
from KBEDebug import *


class Poller:
    """
    TCP 轮询器（HTTP 协议版）。

    负责监听指定地址和端口，接受客户端 TCP 连接，读取客户端发来的 HTTP 请求，
    并返回一个固定的文本响应。所有 I/O 操作均通过 KBEngine 引擎的文件描述符
    回调机制驱动，不阻塞引擎主循环。

    用法:
        from Poller import Poller

        poller = Poller()

        # 启动监听（onInterfaceAppReady）
        poller.start("localhost", 12345)

        # 当有 HTTP 客户端连接并发送请求时，自动回调处理并返回响应：
        #   HTTP/1.1 200 OK
        #   Content-Type: text/plain; charset=utf-8
        #   ...
        #   Hello KBEngine completion API

        # 停止监听
        poller.stop()

    测试方式:
        # 启动服务后，可以用 curl 测试：
        curl http://localhost:12345/

        # 或用浏览器访问：
        http://localhost:12345/

    属性（私有）:
        _listener (socket.socket | None): 监听 socket 对象，未启动时为 None。
        _clients  (dict[int, dict]):      当前活跃的客户端字典。
            key   = 客户端文件描述符（int）
            value = {
                "socket":    socket.socket,   # 客户端 socket 对象
                "buffer":    bytearray,       # 接收数据缓冲区
                "responded": bool,            # 是否已对该客户端做出过响应
            }
    """

    def __init__(self):
        """
        初始化 Poller 实例。

        此时尚未开始监听，_listener 为 None，_clients 为空字典。
        需要显式调用 start() 来启动服务。
        """
        self._listener = None
        self._clients = {}

    # ------------------------------------------------------------------
    # 公开方法
    # ------------------------------------------------------------------

    def start(self, addr, port):
        """
        启动 TCP 监听服务。

        创建一个 IPv4 TCP 监听 socket，设置 SO_REUSEADDR 选项（允许快速重启时
        复用端口），绑定到指定的地址和端口，开始监听（backlog=128）。
        然后将监听 socket 的文件描述符注册到 KBEngine 引擎，引擎会在有新连接
        时回调 self.onAccept()。

        参数:
            addr (str): 监听地址，如 "0.0.0.0" 或 "127.0.0.1"。
            port (int): 监听端口号。
        """
        # 创建 TCP 监听 socket
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # 设置 SO_REUSEADDR，允许重启后立即绑定同一端口（避免 TIME_WAIT 占用）
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # 绑定地址和端口
        self._listener.bind((addr, port))
        # 开始监听，backlog 为 128（操作系统允许的最大未 accept 连接队列长度）
        self._listener.listen(128)

        # 向 KBEngine 注册 accept 回调：当有新连接到达时，引擎会调用 self.onAccept
        KBEngine.registerAcceptFileDescriptor(self._listener.fileno(), self.onAccept)
        INFO_MSG("Poller::start: listen %s:%s" % (addr, port))

    def stop(self):
        """
        停止 TCP 监听服务。

        先取消监听 socket 在 KBEngine 中的 accept 注册，关闭监听 socket。
        然后遍历并关闭所有当前活跃的客户端连接。
        调用后 _listener 被置为 None，_clients 被清空。
        """
        if self._listener:
            # 取消 KBEngine 中该监听 fd 的 accept 回调注册
            KBEngine.deregisterAcceptFileDescriptor(self._listener.fileno())
            # 关闭监听 socket
            self._listener.close()
            self._listener = None

        # 关闭所有活跃的客户端连接
        # 使用 list() 复制键列表，避免在迭代过程中修改字典
        for fd in list(self._clients.keys()):
            self.closeClient(fd)

    # ------------------------------------------------------------------
    # KBEngine 回调方法（由引擎在 I/O 事件发生时调用）
    # ------------------------------------------------------------------

    def onAccept(self, listenerFD, clientFD, errorCode):
        """
        KBEngine accept 回调 — 处理新的客户端连接。

        当监听 socket 上有新连接到达时，KBEngine 引擎会调用此方法。

        参数:
            listenerFD (int): 监听 socket 的文件描述符（即 self._listener.fileno()）。
            clientFD   (int): 新客户端连接的文件描述符。
            errorCode  (int): 错误码。0 表示成功，非 0 表示 accept 过程中发生错误。

        处理逻辑：
            1. 如果 errorCode != 0，记录错误并直接返回（不创建客户端条目）。
            2. 用 clientFD 包装一个 Python socket 对象。
            3. 设置 TCP_NODELAY（禁用 Nagle 算法，减少小包延迟）。
            4. 创建客户端记录并注册读回调。
        """
        if errorCode != 0:
            # accept 出错，记录错误日志
            ERROR_MSG("Poller::onAccept: listenerFD=%i error=%i" % (listenerFD, errorCode))
            return

        try:
            # 通过文件描述符构造 Python socket 对象，以便后续操作
            sock = socket.socket(fileno=clientFD)
            # 禁用 Nagle 算法：数据立即发送，不等待缓冲区填满
            # 对于低延迟场景（如 API 响应）非常重要
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except Exception:
            # 包装 socket 失败（如 fd 无效），记录错误
            ERROR_MSG("Poller::onAccept: wrap client socket failed, clientFD=%i" % clientFD)
            return

        # 创建客户端记录，包含 socket、接收缓冲区和是否已响应的标记
        self._clients[clientFD] = {
            "socket": sock,
            "buffer": bytearray(),   # 可变字节缓冲区，用于累积接收的数据
            "responded": False,      # 标记是否已向该客户端发送过响应
        }

        # 向 KBEngine 注册该客户端 fd 的读回调：当有数据到达时引擎会调用 self.onRead
        KBEngine.registerReadDataFileDescriptor(clientFD, self.onRead)
        DEBUG_MSG("Poller::onAccept: new clientFD=%i" % clientFD)

    def onRead(self, fd, data, errorCode):
        """
        KBEngine 读回调 — 处理客户端发来的数据。

        当某个已注册的客户端 socket 上有数据可读时，KBEngine 引擎会调用此方法。

        参数:
            fd        (int):  客户端文件描述符。
            data      (bytes):本次读取到的数据（可能是一段 TCP 分片）。
            errorCode (int):  错误码。0 表示正常，非 0 表示读出错。

        处理逻辑：
            1. 校验客户端是否存在；不存在则忽略。
            2. 如果 errorCode != 0，记录错误并关闭连接。
            3. 如果 data 长度为 0，表示对端主动断开（EOF），关闭连接。
            4. 如果已经响应过该客户端，忽略后续数据（单次响应模式）。
            5. 将数据追加到缓冲区，检查是否收到完整的 HTTP 请求头（以 \r\n\r\n 结尾）。
            6. 收到完整请求头后，标记 responded=True 并调用 processData() 处理。
        """
        # 查找客户端记录
        client = self._clients.get(fd)
        if client is None:
            # 客户端已被关闭或不存在，忽略此次回调
            return

        if errorCode != 0:
            # 读取过程中发生错误，记录并关闭连接
            ERROR_MSG("Poller::onRead: fd=%i error=%i" % (fd, errorCode))
            self.closeClient(fd)
            return

        if len(data) == 0:
            # 对端正常关闭连接（TCP FIN），数据长度为 0
            DEBUG_MSG("Poller::onRead: fd=%i disconnect" % fd)
            self.closeClient(fd)
            return

        if client["responded"]:
            # 已经对该客户端做出过响应，忽略后续所有数据
            # 这是一个"一次性请求-响应"的设计：每个连接只处理首次完整请求
            return

        # 将本次收到的数据追加到缓冲区
        client["buffer"].extend(data)
        DEBUG_MSG("Poller::onRead: fd=%i dataSize=%i totalSize=%i" % (fd, len(data), len(client["buffer"])))

        # 检测 HTTP 请求头结束标志：\r\n\r\n（空行，表示 HTTP 头部结束）
        # 注意：这是一个简化实现，不解析 Content-Length 或 chunked 编码的 Body
        if b"\r\n\r\n" not in client["buffer"]:
            # 尚未收到完整的 HTTP 请求头，继续等待更多数据
            return

        # 收到完整请求头，标记已响应（阻止重复处理）
        client["responded"] = True
        # 将缓冲区转为不可变 bytes 并传递给数据处理方法
        self.processData(fd, bytes(client["buffer"]))

    def processData(self, fd, data):
        """
        处理收到的完整 HTTP 请求数据，构造并发送 HTTP 响应。

        当前实现忽略请求内容，返回固定的文本响应：
            "Hello KBEngine completion API\n"

        参数:
            fd   (int):  客户端文件描述符。
            data (bytes):完整的 HTTP 请求数据（用于将来扩展，当前未使用）。

        响应格式:
            HTTP/1.1 200 OK
            Content-Type: text/plain; charset=utf-8
            Content-Length: <body 长度>
            Connection: close

            <body>
        """
        # 响应体内容
        body = b"Hello KBEngine completion API\n"

        # 构造完整的 HTTP 响应
        # 使用 bytes 拼接（而非字符串），避免编码转换开销
        response = (
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/plain; charset=utf-8\r\n"
            b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
            b"Connection: close\r\n"    # 通知客户端响应完成后将关闭连接
            b"\r\n" +                   # HTTP 头部与 Body 之间的空行
            body
        )

        # 通过 KBEngine 异步写出响应数据，完成后回调 self.onWriteComplete
        KBEngine.writeFileDescriptor(fd, response, self.onWriteComplete)

    def onWriteComplete(self, fd, bytesWritten, errorCode):
        """
        KBEngine 写完成回调 — 响应数据写入完成后的处理。

        当通过 writeFileDescriptor 发起的异步写操作完成时，KBEngine 引擎调用此方法。

        参数:
            fd           (int): 客户端文件描述符。
            bytesWritten (int): 实际写入的字节数。
            errorCode    (int): 错误码。0 表示成功，非 0 表示写出错。

        处理逻辑：
            无论成功或失败，都关闭客户端连接（因为响应头中 Connection: close）。
        """
        if errorCode != 0:
            # 写出错，记录错误
            ERROR_MSG("Poller::onWriteComplete: fd=%i error=%i" % (fd, errorCode))
        else:
            # 写成功，记录写入字节数
            DEBUG_MSG("Poller::onWriteComplete: fd=%i bytesWritten=%i" % (fd, bytesWritten))

        # 响应已发送（或发送失败），关闭连接
        self.closeClient(fd)

    # ------------------------------------------------------------------
    # 内部辅助方法
    # ------------------------------------------------------------------

    def closeClient(self, fd):
        """
        关闭指定的客户端连接并清理相关资源。

        执行步骤：
            1. 从 _clients 字典中移除该客户端记录。
            2. 取消该 fd 在 KBEngine 中的读回调注册。
            3. 关闭底层 socket。
            4. 记录调试日志。

        参数:
            fd (int): 要关闭的客户端文件描述符。

        注意：
            此方法是幂等的 — 如果 fd 对应的客户端已不存在，则静默返回。
            socket.close() 的异常被捕获并忽略，因为连接可能已经处于异常状态。
        """
        # pop() 安全地移除并返回客户端记录；若不存在则返回 None
        client = self._clients.pop(fd, None)
        if client is None:
            # 客户端已被清理过，直接返回
            return

        # 取消 KBEngine 中该 fd 的读回调注册，避免引擎继续触发 onRead
        KBEngine.deregisterReadDataFileDescriptor(fd)

        try:
            # 关闭底层 socket，释放系统资源
            client["socket"].close()
        except Exception:
            # 忽略关闭时的异常（socket 可能已经处于异常/半关闭状态）
            pass

        DEBUG_MSG("Poller::closeClient: fd=%i" % fd)
