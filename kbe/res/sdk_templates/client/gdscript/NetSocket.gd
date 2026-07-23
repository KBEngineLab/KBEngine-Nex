class_name NetSocket
## 用来封装实现C#中socket对象有关功能
## 注意：导出到安卓时，在导出项目或使用一键部署之前，请务必在安卓导出预设中，开启 INTERNET 权限。否则，任何类型的网络通信都将被 Android 阻止。

var error:Error
var state: int
var isClose: bool
var networkInterface:NetworkInterfaceBase

var onopen: Callable
var onerror: Callable

func connectHost(_addr:String, _port:int)-> void:
	pass

func close(_code:int=1000, _reason:String="")-> void:
	pass

func send(_stream:MemoryStream)-> bool:
	return false

func isConnected()-> bool:
	return false

func process()-> void:
	pass
