import asyncio
from urllib.parse import parse_qs

from channels.generic.websocket import AsyncWebsocketConsumer

from webconsole.telnet_console import  TelnetConsole


class PythonConsoleProcessCmdConsumer(AsyncWebsocketConsumer):
    def __init__(self, *args, **kwargs):
        super().__init__(args, kwargs)
        self.console = None
        self.console_task = None

    async def connect(self):
        await self.accept()
        qs = parse_qs(self.scope["query_string"].decode())
        cp_host = qs.get("host", [None])[0]
        cp_port = int(qs.get("port", [0])[0])

        self.console = TelnetConsole(self, cp_host, cp_port)
        # await self.console.run()
        self.console_task = asyncio.create_task(self.console.run())

        # self.console_task =  asyncio.create_task(self.run_ws_safe())

    async def receive(self, text_data=None, bytes_data=None):
        # 被TelnetConsole._read_ws_loop调用替代，不用实现此方法
        if self.console:
            await self.console.onReceivedClientData(text_data)

    async def run_ws_safe(self):
        # 所有耗时操作放进线程池

        await asyncio.to_thread(self.run_ws_sync)

    def run_ws_sync(self):
        pass

    async def disconnect(self, close_code):
        if hasattr(self, "console"):
            await self.console.close()

        # # 取消后台任务
        # if hasattr(self, "console_task"):
        #     self.console_task.cancel()
        #     try:
        #         await self.console_task
        #     except asyncio.CancelledError:
        #         pass
