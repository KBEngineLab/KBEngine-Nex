"""Select the configured default database without asset-specific routing.
不执行业务分库，始终选择已配置的默认数据库。
"""


def onSelectAccountDBInterface(accountName):
    return "default"
