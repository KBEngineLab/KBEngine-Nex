#if UNITY_5_3_OR_NEWER


namespace KBEngine
{
    
    using KBEngine;
    public class UnityLogProvider : ILogProvider
    {
        public void Info(string message)
        {
            UnityEngine.Debug.Log(message);
        }

        public void Warn(string message)
        {
            UnityEngine.Debug.LogWarning(message);
        }

        public void Error(string message)
        {
            UnityEngine.Debug.LogError(message);
        }
        public void Debug(string message)
        {
            UnityEngine.Debug.Log(message);
        }
    }
}

#endif