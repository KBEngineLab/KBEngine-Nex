class_name NetworkInterfaceTCP extends NetworkInterfaceBase


func createSocket()-> NetSocket:
	## Security.PrefetchSocketPolicy(ip, 843)
	#var pSocket:NetSocket = NetSocket.new(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)
	#pSocket.SetSocketOption(System.Net.Sockets.SocketOptionLevel.Socket, SocketOptionName.ReceiveBuffer, KBEngineApp.app.getInitArgs().getTCPRecvBufferSize() * 2)
	#pSocket.SetSocketOption(System.Net.Sockets.SocketOptionLevel.Socket, SocketOptionName.SendBuffer, KBEngineApp.app.getInitArgs().getTCPSendBufferSize() * 2)
	#pSocket.NoDelay = true
	## pSocket.Blocking = false
	#return pSocket
	return NetSocketTCP.new()

	
