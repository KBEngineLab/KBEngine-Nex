from django.urls import re_path

from cluster import consumers
from component.ws.profile_process_cmd import ProfileProcessCmdConsumer
from component.ws.python_console_process_cmd import PythonConsoleProcessCmdConsumer
from component.ws.space_viewer_process_cmd import SpaceViewerProcessCmdConsumer, SpaceViewerCellProcessCmdConsumer
from component.ws.status_process_cmd import StatusProcessCmdConsumer
from component.ws.watcher_process_cmd import WatcherProcessCmdConsumer

websocket_urlpatterns = [
    # re_path(r"ws/server_manage/$", consumers.LogConsumer.as_asgi()),
    re_path(r"^ws/server_manage/$", consumers.LogConsumer.as_asgi()),
    re_path(r"ws/space_viewer/process_cmd/$", SpaceViewerProcessCmdConsumer.as_asgi()),
    re_path(r"ws/space_viewer/cell_process_cmd/$", SpaceViewerCellProcessCmdConsumer.as_asgi()),


    re_path(r"^ws/watcher/process_cmd/$", WatcherProcessCmdConsumer.as_asgi()),
    re_path(r"^ws/python_console/process_cmd/$", PythonConsoleProcessCmdConsumer.as_asgi()),
    re_path(r"^ws/profile/process_cmd/$", ProfileProcessCmdConsumer.as_asgi()),
    re_path(r"^ws/status/process_cmd/$", StatusProcessCmdConsumer.as_asgi()),
]