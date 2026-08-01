"""Local account validation for an isolated performance run.
隔离性能运行使用的本地账号校验。
"""

import KBEngine


def onLoginAppReady():
    """Keep local login validation side-effect free.
    本地登录校验保持无副作用，不混入外部平台逻辑。
    """
    pass


def onLoginCallbackFromDB(loginName, accountName, errorno, datas):
    pass


def onCreateAccountCallbackFromDB(accountName, errorno, datas):
    pass


def onRequestLogin(loginName, password, clientType, datas):
    if len(loginName) > 64:
        return (KBEngine.SERVER_ERR_NAME, loginName, password, clientType, datas)
    if len(password) > 64:
        return (KBEngine.SERVER_ERR_PASSWORD, loginName, password, clientType, datas)
    return (KBEngine.SERVER_SUCCESS, loginName, password, clientType, datas)


def onRequestCreateAccount(accountName, password, datas):
    if len(accountName) > 64:
        return (KBEngine.SERVER_ERR_NAME, accountName, password, datas)
    if len(password) > 64:
        return (KBEngine.SERVER_ERR_PASSWORD, accountName, password, datas)
    return (KBEngine.SERVER_SUCCESS, accountName, password, datas)
