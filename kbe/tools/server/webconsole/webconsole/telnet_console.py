import  sys

import asyncio

def _pre_process_cmd(cmd: bytes) -> bytes:
    if cmd.endswith(b"\r\n"):
        return cmd
    elif cmd.endswith(b"\r"):
        cmd += b"\n"
    elif cmd.endswith(b"\n"):
        cmd = cmd[:-1] + b"\r\n"
    else:
        cmd += b"\r\n"
    return cmd


class TelnetConsole:
    def __init__(self, wsInst, host: str, port: int):
        """
        wsInst: Django Channels WebSocketConsumer 实例
        host, port: Telnet 服务器地址
        """
        self.wsInst = wsInst
        self.host = host
        self.port = port
        self.reader = None
        self.writer = None
        self.is_closed = False

    async def close(self):
        """
        安全关闭
        """
        if self.is_closed:
            return

        self.is_closed = True
        try:
            if self.writer:
                self.writer.close()
                await self.writer.wait_closed()
        except Exception:
            pass

        try:
            if self.wsInst:
                await self.wsInst.close()
        except Exception:
            pass

        self.reader = None
        self.writer = None
        self.wsInst = None

    async def run(self):
        """
        启动Telnet转发协程
        """
        try:
            self.reader, self.writer = await asyncio.open_connection(self.host, self.port)
        except Exception as e:
            await self.wsInst.send("服务器连接失败！\n")
            await self.close()
            return

        await self.onConnectedToConsole()

        # 并发执行Telnet和WebSocket监听
        try:
            await asyncio.gather(
                self._read_telnet_loop(),
            )
        except asyncio.CancelledError:
            pass
        except Exception:
            sys.excepthook(*sys.exc_info())

        await self.close()

    async def _read_telnet_loop(self):
        """
        监听Telnet服务器输出
        """
        while not self.is_closed:
            try:
                data = await asyncio.wait_for(self.reader.read(1024), timeout=0.1)
                if not data:
                    break
                ok = await self.onReceivedConsoleData(data)
                if not ok:
                    break
            except asyncio.TimeoutError:
                continue
            except Exception:
                break

    # async def _read_ws_loop(self):
    #     """
    #     监听前端WebSocket消息
    #     """
    #     while not self.is_closed:
    #         try:
    #             data = await self.wsInst.receive_text()  # Channels的receive_text()
    #             print(data)
    #             if not data:
    #                 continue
    #             ok = await self.onReceivedClientData(data)
    #             if not ok:
    #                 break
    #         except Exception:
    #             break

    async def onConnectedToConsole(self):
        """
        当成功连接上Telnet控制台时回调
        """
        await self.wsInst.send("已连接到服务器。\n")

    async def onReceivedConsoleData(self, data: bytes):
        """
        Telnet返回 -> WebSocket
        """
        try:
            await self.wsInst.send(data.decode(errors="ignore"))
        except Exception:
            return False
        return True

    async def onReceivedClientData(self, data: str):
        """
        WebSocket -> Telnet
        """
        if data.strip() == ":quit":
            await self.wsInst.close()
            return False

        if self.writer:
            try:
                self.writer.write(_pre_process_cmd(data.encode()))
                await self.writer.drain()
            except Exception:
                return False
        return True


class ProfileConsole(TelnetConsole):
    """
    用于性能分析的控制台类
    """
    def __init__(self, wsInst, host, port, command, sec, password):
        """
        """
        super().__init__(wsInst, host, port)
        self.wsInst = wsInst
        self.host = host
        self.port = port
        self.writer = None
        self.cmd = command.encode('utf-8')
        self.sec = sec.encode('utf-8')
        self.password = password.encode('utf-8')

    async def onConnectedToConsole( self ):
        """
        template method.
        当成功连接上telnet控制台时回调pytickprofile
        """
        self.writer.write( b"" + self.password + b"\r\n")
        await self.writer.drain()
        self.writer.write( b":"+self.cmd+b" "+self.sec+b"\r\n")
        await self.writer.drain()

    async def onReceivedConsoleData( self, data ):
        """
        template method.
        当从telenet控制台收到了新数据以后回调
        """
        # self.wsInst.send( data )
        # return True

        try:
            await self.wsInst.send(data.decode(errors="ignore"))
        except Exception:
            return False
        return True

    async def onReceivedClientData( self, data ):
        """
        template method.
        当从客户端收到了新数据以后回调
        """
        if data == ":":
            self.wsInst.close()
            return False
        self.writer.write( _pre_process_cmd( data.encode() ) )
        await self.writer.drain()
        return True
