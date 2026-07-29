import asyncio
import time
from urllib.parse import parse_qs

from asgiref.sync import async_to_sync
from channels.generic.websocket import AsyncWebsocketConsumer

from pycommon import Watcher
from webconsole.telnet_console import ProfileConsole


class ProfileProcessCmdConsumer(AsyncWebsocketConsumer):
    def __init__(self, *args, **kwargs):
        super().__init__(args, kwargs)
        self.console = None
        self.console_task = None

    async def connect(self):
        await self.accept()
        # self.console_task =  asyncio.create_task(self.run_ws_safe())

        qs = parse_qs(self.scope["query_string"].decode())
        host = qs.get("host", [None])[0]
        port = int(qs.get("port", [0])[0])
        cmd = qs.get("cmd", [None])[0]
        sec = qs.get("sec", [None])[0]
        password = qs.get("password", [None])[0]

        self.console = ProfileConsole(self, host, port, cmd, sec, password)
        self.console_task = asyncio.create_task(self.console.run())



    async def run_ws_safe(self):
        # 所有耗时操作放进线程池
        await asyncio.to_thread(self.run_ws_sync)

    def run_ws_sync(self):
        # return self.console.run()
        pass


    async def disconnect(self, close_code):

        if hasattr(self, "console"):
            if self.console:
                await self.console.close()

        # 取消后台任务
        if hasattr(self, "console_task"):
            self.console_task.cancel()
            try:
                await self.console_task
            except asyncio.CancelledError:
                pass
