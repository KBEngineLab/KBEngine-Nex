import asyncio
import time
from urllib.parse import parse_qs

from asgiref.sync import async_to_sync
from channels.generic.websocket import AsyncWebsocketConsumer

from pycommon import Component_Status

class CSData(object):
    def __init__(self, wsInst, cp, host, port):
        self.wsInst = wsInst
        self.cp = cp
        self.port = port
        self.host = host
        self.Component_Status = Component_Status.ComponentStatus(cp)

    def do(self):
        self.Component_Status.connect(self.host,self.port)
        self.Component_Status.requireQueryCS()
        while True:
            # if self.Component_Status.CSData == []:
            self.Component_Status.processOne()
            time.sleep(0.5)

            async_to_sync(self.wsInst.send)(text_data=str(self.Component_Status.CSData))
            # self.wsInst.send(str.encode(str(self.Component_Status.CSData)))
            # time.sleep(1)
            self.Component_Status.clearCSData()
            self.Component_Status.requireQueryCS()

    def close(self):
        if self.wsInst:
            self.wsInst.close()
        self.wsInst = None


class StatusProcessCmdConsumer(AsyncWebsocketConsumer):
    def __init__(self, *args, **kwargs):
        super().__init__(args, kwargs)
        self.console = None
        self.console_task = None

    async def connect(self):
        await self.accept()
        # self.console_task =  asyncio.create_task(self.run_ws_safe())


        self.console_task =  asyncio.create_task(self.run_ws_safe())



    async def run_ws_safe(self):
        # 所有耗时操作放进线程池
        await asyncio.to_thread(self.run_ws_sync)

    def run_ws_sync(self):

        qs = parse_qs(self.scope["query_string"].decode())
        host = qs.get("host", [None])[0]
        port = int(qs.get("port", [0])[0])
        cp = int(qs.get("cp", [0])[0])

        csData = CSData(self, cp, host, port)
        csData.do()
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
