import asyncio
import threading
from urllib.parse import parse_qs

from asgiref.sync import async_to_sync
from channels.generic.websocket import AsyncWebsocketConsumer
import json

from pycommon import Define
from pycommon.LoggerWatcher import LoggerWatcher


class LogWatch(object):
    """
    日志输出
    """

    def __init__(self, wsInst, extaddr, extport, uid, components_check, logtype, globalOrder, groupOrder, searchDate,
                 keystr):
        self.wsInst = wsInst
        self.extaddr = extaddr
        self.extport = extport
        self.uid = uid
        self.components_check = components_check
        self.logtype = logtype
        self.globalOrder = globalOrder
        self.groupOrder = groupOrder
        self.searchDate = searchDate
        self.keystr = keystr
        self.logger = LoggerWatcher()
        self.previous_log = []

        self._stop_event = threading.Event()  # 停止标记

    def do(self):
        """
        """
        self.logger.close()
        self.logger.connect(self.extaddr, self.extport)
        self.logger.registerToLoggerForWeb(self.uid, self.components_check, self.logtype, self.globalOrder,
                                           self.groupOrder, self.searchDate, self.keystr)

        def onReceivedLog(logs):
            if self._stop_event.is_set():  # 检查停止
                return

            new_logs = list(set(logs) ^ set(self.previous_log))
            for e in new_logs:
                e = e.decode("utf-8")
                async_to_sync(self.wsInst.send)(text_data=e)
            self.previous_log = logs

        self.logger.receiveLog(onReceivedLog, True)

    async def close(self):
        """
        """
        self._stop_event.set()  # 设置停止标记
        self.logger.deregisterFromLogger()
        self.logger.close()

        if self.wsInst:
            await self.wsInst.close()
        self.wsInst = None

        self.extaddr = ""
        self.extport = 0



class LogConsumer(AsyncWebsocketConsumer):
    def __init__(self, *args, **kwargs):
        super().__init__(args, kwargs)
        self.console_task = None
        self.console = None

    async def connect(self):


        await self.accept()
        await self.send(text_data="WebSocket connected!")
        self.console_task =  asyncio.create_task(self.run_ws_safe())
        # asyncio.create_task(self.run_ws())

    async def run_ws_safe(self):
        # 所有耗时操作放进线程池
        await asyncio.to_thread(self.run_ws_sync)

    def run_ws_sync(self):
        # 获取 query string
        query_string_bytes = self.scope["query_string"]  # bytes 类型
        query_string = query_string_bytes.decode()  # str 类型

        # 解析成字典
        query_params = parse_qs(query_string)

        extaddr = query_params.get("extaddr", [None])[0]
        extport = int(query_params.get("extport", [0])[0])
        uid = int(query_params.get("uid", [0])[0])

        # 获取进程选中状态
        components_check = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
        components_check2 = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
        baseapp_check = query_params.get("baseapp_check", [None])[0]
        baseappmgr_check = query_params.get("baseappmgr_check", [None])[0]
        cellapp_check = query_params.get("cellapp_check", [None])[0]
        dbmgr_check = query_params.get("dbmgr_check", [None])[0]
        loginapp_check = query_params.get("loginapp_check", [None])[0]

        if baseapp_check == '1': components_check[6] = Define.BASEAPP_TYPE
        if baseappmgr_check == '1': components_check[3] = Define.BASEAPPMGR_TYPE
        if cellapp_check == '1': components_check[5] = Define.CELLAPP_TYPE
        if dbmgr_check == '1': components_check[1] = Define.DBMGR_TYPE
        if loginapp_check == '1': components_check[2] = Define.LOGINAPP_TYPE
        if components_check == components_check2:
            i = -1
            for x in components_check:
                i = i + 1
                components_check[x] = i

        logtype = int(query_params.get("logtype", [None])[0])

        # 自定义搜索
        globalOrder = query_params.get("globalOrder", [None])[0]
        groupOrder = query_params.get("groupOrder", [None])[0]
        if globalOrder == "" or globalOrder == None:
            globalOrder = 0
        if groupOrder == "" or groupOrder == None:
            groupOrder = 0

        globalOrder = int(globalOrder)
        groupOrder = int(groupOrder)
        searchDate = query_params.get("searchDate", [''])[0]
        keystr = query_params.get("keystr", [''])[0]

        self.console = LogWatch(self, extaddr, extport, uid, components_check, logtype, globalOrder, groupOrder,
                           searchDate, keystr)
        self.console.do()



    async def disconnect(self, close_code):
        print("WebSocket closed:", close_code)

            # 安全关闭 LogWatch
        if hasattr(self, "console"):
            await self.console.close()

        # 取消后台任务
        if hasattr(self, "console_task"):
            self.console_task.cancel()
            try:
                await self.console_task
            except asyncio.CancelledError:
                pass
