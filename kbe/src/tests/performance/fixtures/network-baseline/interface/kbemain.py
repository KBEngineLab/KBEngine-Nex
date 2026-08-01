"""Minimal Interfaces callbacks with no sockets, timers, or external IO.
不创建套接字、定时器或外部 IO 的最小 Interfaces 回调。
"""

import KBEngine


def onInterfaceAppReady():
    """Keep Interfaces available without opening a fixture socket.
    保持 Interfaces 可用，但基线不创建额外监听套接字。
    """
    pass


def onInterfaceAppShutDown():
    pass


def onRequestCreateAccount(registerName, password, datas):
    KBEngine.createAccountResponse(registerName, registerName, datas, KBEngine.SERVER_SUCCESS)


def onRequestAccountLogin(loginName, password, datas):
    # DBMgr 仍负责密码及账号状态语义；fixture 只移除外部平台依赖。
    # DBMgr retains password and account-state semantics; the fixture only removes external platform IO.
    KBEngine.accountLoginResponse(
        loginName,
        loginName,
        datas,
        KBEngine.SERVER_ERR_LOCAL_PROCESSING,
    )


def onRequestCharge(ordersID, entityDBID, datas):
    KBEngine.chargeResponse(ordersID, datas, KBEngine.SERVER_SUCCESS)
