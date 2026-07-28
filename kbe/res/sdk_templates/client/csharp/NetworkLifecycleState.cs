namespace KBEngine
{
	using System.Threading;

	// 网络生命周期只保存断线通知资格；transport 资源仍由 NetworkInterfaceBase 在自己的锁内释放。
	// Network lifecycle stores only disconnect-notification eligibility; NetworkInterfaceBase still releases transport resources under its own lock.
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
