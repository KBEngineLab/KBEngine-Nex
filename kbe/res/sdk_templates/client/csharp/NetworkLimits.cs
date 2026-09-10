namespace KBEngine
{
	public static class NetworkLimits
	{
		public const int TCP_PACKET_MAX = 1460;
		public const int UDP_PACKET_MAX = 1472;
		public const int DEFAULT_MESSAGE_MAX = 16 * 1024 * 1024;
	}
}
