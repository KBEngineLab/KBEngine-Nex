# -*- coding: utf-8 -*-
import sys

import KBEngine


def _getCaller():
    # 固定回溯三层可跳过日志包装函数，让 IDE 直接定位业务脚本调用点。
    # Walking back three frames skips the logging wrappers and points the IDE at the caller.
    frame = sys._getframe(3)
    return frame.f_code.co_filename, frame.f_lineno, frame.f_code.co_name


def printMsg(args, isPrintPath):
    message = " ".join(str(item) for item in args)
    if not isPrintPath:
        print(message)
        return

    filename, lineNumber, functionName = _getCaller()
    print(f'{message} - File "{filename}", line {lineNumber}, in {functionName}')


def TRACE_MSG(*args):
    KBEngine.scriptLogType(KBEngine.LOG_TYPE_NORMAL)
    printMsg(args, False)


def DEBUG_MSG(*args):
    if KBEngine.publish() == 0:
        KBEngine.scriptLogType(KBEngine.LOG_TYPE_DBG)
        printMsg(args, True)


def INFO_MSG(*args):
    if KBEngine.publish() <= 1:
        KBEngine.scriptLogType(KBEngine.LOG_TYPE_INFO)
        printMsg(args, False)


def WARNING_MSG(*args):
    KBEngine.scriptLogType(KBEngine.LOG_TYPE_WAR)
    printMsg(args, True)


def ERROR_MSG(*args):
    KBEngine.scriptLogType(KBEngine.LOG_TYPE_ERR)
    printMsg(args, True)
