import asyncio
import time
from urllib.parse import parse_qs

from asgiref.sync import async_to_sync
from channels.generic.websocket import AsyncWebsocketConsumer

from pycommon import Watcher


class WatcherData(object):
    def __init__(self, wsInst, cp, port, host, key):
        self.wsInst = wsInst
        self.cp = cp
        self.port = port
        self.host = host
        self.key = key
        self.watcher = Watcher.Watcher(cp)

    def do(self):
        self.watcher.connect(self.host,self.port)
        self.watcher.requireQueryWatcher(self.key)
        while True:
            if not self.watcher.watchData:
                self.watcher.processOne()
                if self.key == "root/network/messages":
                    time.sleep(1)
                else:
                    time.sleep(0.5)
            else:
                # self.wsInst.send()
                async_to_sync(self.wsInst.send)(text_data=str(self.watcher.watchData))
                self.watcher.clearWatchData()
                self.watcher.requireQueryWatcher(self.key)

    def close(self):
        if self.wsInst:
            self.wsInst.close()
        self.wsInst = None




class WatcherProcessCmdConsumer(AsyncWebsocketConsumer):
    def __init__(self, *args, **kwargs):
        super().__init__(args, kwargs)
        self.space_viewers = None
        self.console_task = None

    async def connect(self):
        await self.accept()
        self.console_task =  asyncio.create_task(self.run_ws_safe())



    async def run_ws_safe(self):
        # 所有耗时操作放进线程池

        await asyncio.to_thread(self.run_ws_sync)

    def run_ws_sync(self):
        qs = parse_qs(self.scope["query_string"].decode())
        cp_host = qs.get("host", [None])[0]
        cp_port = int(qs.get("port", [0])[0])
        cp_type = int(qs.get("cp", [0])[0])
        cp_key = qs.get("key", [None])[0]

        watcher = WatcherData(self, cp_type, cp_port, cp_host, cp_key)
        watcher.do()


    async def disconnect(self, close_code):
        # 取消后台任务
        if hasattr(self, "console_task"):
            self.console_task.cancel()
            try:
                await self.console_task
            except asyncio.CancelledError:
                pass
