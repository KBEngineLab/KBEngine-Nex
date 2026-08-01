"""The baseline has no cell-side game bootstrap work.
基线不执行 Cell 侧游戏启动逻辑。
"""


def onInit(isReload):
    """Keep the cell fixture explicit without adding periodic work.
    显式实现 Cell 初始化入口，但不增加任何周期任务。
    """
    pass
