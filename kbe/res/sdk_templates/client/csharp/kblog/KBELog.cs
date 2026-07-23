

namespace KBEngine
{
    using System;
public enum DEBUGLEVEL : int
{
    DEBUG = 0,
    INFO,
    WARNING,
    ERROR,
            
    NOLOG,  // 放在最后面，使用这个时表示不输出任何日志（!!!慎用!!!）
}


public static class KBELog
{
    
    public static DEBUGLEVEL DebugLevel = DEBUGLEVEL.DEBUG;
        
    private static ILogProvider _provider;

    public static void Init(ILogProvider provider)
    {
        _provider = provider;
    }

    public static string GetHead()
    {
        return "[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss fff") + "] ";
    }

    public static void INFO_MSG(string message)
    {
        if (DEBUGLEVEL.INFO >= DebugLevel)
            _provider?.Info(GetHead()+message);
    }

    public static void WARNING_MSG(string message)
    {
        if (DEBUGLEVEL.WARNING >= DebugLevel)
            _provider?.Warn(GetHead()+message);
    }

    public static void ERROR_MSG(string message)
    {
        if (DEBUGLEVEL.ERROR >= DebugLevel)
            _provider?.Error(GetHead()+message);
    }

    public static void DEBUG_MSG(string message)
    {
        if (DEBUGLEVEL.DEBUG >= DebugLevel)
            _provider?.Debug(GetHead()+message);
    }
}
}
