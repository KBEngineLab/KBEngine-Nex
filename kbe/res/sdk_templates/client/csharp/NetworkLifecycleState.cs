namespace KBEngine
{
	using System.Threading;

	// 网络生命周期只保存断线通知资格；transport 资源由 NetworkSession 确定性释放。
	// Network lifecycle stores only disconnect-notification eligibility; NetworkSession releases transport resources deterministically.
	internal sealed class NetworkLifecycleState
	{
		private int _disconnectNotificationArmed;

		public void arm()
		{
			Interlocked.Exchange(ref _disconnectNotificationArmed, 1);
		}

		public bool consume(bool notifyDisconnected)
		{
			int wasArmed = Interlocked.Exchange(ref _disconnectNotificationArmed, 0);
			return notifyDisconnected && wasArmed != 0;
		}
	}
}
