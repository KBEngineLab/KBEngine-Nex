import KBEngine

isPrintPath = True

if isPrintPath:
    import sys

def _getCaller():
    frame = sys._getframe(3)
    filename = frame.f_code.co_filename
    lineno = frame.f_lineno
    func = frame.f_code.co_name
    return filename, lineno, func

def printMsg(args):
    msg = " ".join(str(m) for m in args)

    if isPrintPath:
        filename, lineno, func = _getCaller()
        print(f'{msg} - File "{filename}", line {lineno}, in {func}')
    else:
        print(msg)

def TRACE_MSG(*args):
    KBEngine.scriptLogType(KBEngine.LOG_TYPE_NORMAL)
    printMsg(args)

def DEBUG_MSG(*args):
    if KBEngine.publish() == 0:
        KBEngine.scriptLogType(KBEngine.LOG_TYPE_DBG)
        printMsg(args)

def INFO_MSG(*args):
    if KBEngine.publish() <= 1:
        KBEngine.scriptLogType(KBEngine.LOG_TYPE_INFO)
        printMsg(args)

def WARNING_MSG(*args):
    KBEngine.scriptLogType(KBEngine.LOG_TYPE_WAR)
    printMsg(args)

def ERROR_MSG(*args):
    KBEngine.scriptLogType(KBEngine.LOG_TYPE_ERR)
    printMsg(args)