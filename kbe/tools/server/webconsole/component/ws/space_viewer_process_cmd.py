import asyncio
import json
import sys
import traceback
from urllib.parse import parse_qs

from asgiref.sync import async_to_sync
from channels.generic.websocket import AsyncWebsocketConsumer
from django.shortcuts import render

from pycommon import SpaceViews, Define


class spaceviewerDate(object):
    def __init__(self, wsInst, cp, host, port):
        self.wsInst = wsInst
        self.cp = cp
        self.port = port
        self.host = host


    def do(self):
        self.SpaceViews = SpaceViews.SpaceViewer(self.cp)
        self.SpaceViews.connect(self.host,self.port)
        self.test = ""
        while True:
            self.SpaceViews.requireQuerySpaceViewer()
            self.SpaceViews.processOne(0.1)
            if self.SpaceViews.SpaceViewerData != self.test:
                data = json.dumps(self.SpaceViews.SpaceViewerData)
                # self.wsInst.send(data)
                async_to_sync(self.wsInst.send)(text_data=data)
                self.test = self.SpaceViews.SpaceViewerData
            self.SpaceViews.clearSpaceViewerData()

    def close(self):
        if self.wsInst:
            self.wsInst.close()
        self.wsInst = None

class CellSpace(object):
    def __init__(self, wsInst, cp, host, port, spaceID):
        self.wsInst = wsInst
        self.cp = cp
        self.port = port
        self.host = host
        self.spaceID = spaceID
        self.test = ""


    def do(self):
        self.CellSpaceViewer = SpaceViews.CellViewer(self.cp, self.spaceID)
        self.CellSpaceViewer.connect(self.host,self.port)
        while True:
            self.CellSpaceViewer.requireQueryCellViewer()
            self.CellSpaceViewer.processOne(0.1)
            if self.CellSpaceViewer.CellViewerData != self.test:
                data = json.dumps(self.CellSpaceViewer.CellViewerData)
                # self.wsInst.send(data.encode())
                async_to_sync(self.wsInst.send)(text_data=data)
                self.test = self.CellSpaceViewer.CellViewerData
            self.CellSpaceViewer.clearCellViewerData()

    def close(self):
        if self.wsInst:
            self.wsInst.close()
        self.wsInst = None



class SpaceViewerProcessCmdConsumer(AsyncWebsocketConsumer):
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


        self.space_viewers = spaceviewerDate(self, cp_type, cp_host, cp_port)
        self.space_viewers.do()

    async def disconnect(self, close_code):
        # 取消后台任务
        if hasattr(self, "console_task"):
            self.console_task.cancel()
            try:
                await self.console_task
            except asyncio.CancelledError:
                pass



class SpaceViewerCellProcessCmdConsumer(AsyncWebsocketConsumer):
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
        space_id = int(qs.get("spaceID", [0])[0])

        self.CellSpaceViewer = CellSpace(self, cp_type, cp_host, cp_port, space_id)
        self.CellSpaceViewer.do()

    async def disconnect(self, close_code):
        # 取消后台任务
        if hasattr(self, "console_task"):
            self.console_task.cancel()
            try:
                await self.console_task
            except asyncio.CancelledError:
                pass
