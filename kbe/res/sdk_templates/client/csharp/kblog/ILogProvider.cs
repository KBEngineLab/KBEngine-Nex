namespace KBEngine
{
    public interface ILogProvider
    {
        void Info(string message);
        void Warn(string message);
        void Error(string message);
        void Debug(string message);
    }
}

