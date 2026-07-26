# -*- coding: utf-8 -*-
"""
基于 KBEngine 完成式文件描述符 API 的非阻塞 TCP 示例。
Non-blocking TCP example built on KBEngine completion file-descriptor APIs.
"""

import socket

import KBEngine
from KBEDebug import DEBUG_MSG, ERROR_MSG, INFO_MSG


class Poller:
	"""
	监听连接、接收 HTTP 请求并异步返回固定响应，不阻塞 Interfaces 主线程。
	Accept connections, receive HTTP requests, and send a fixed response without blocking the Interfaces main thread.
	"""

	_MAX_REQUEST_SIZE = 64 * 1024

	def __init__(self):
		self._listener = None
		self._clients = {}

	def start(self, addr, port):
		"""
		启动监听并把 listener 注册到专用 accept 完成队列。
		Start listening and register the listener with the dedicated accept completion queue.
		"""
		if self._listener is not None:
			return

		listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
		listener.bind((addr, port))
		listener.listen(128)

		try:
			KBEngine.registerAcceptFileDescriptor(listener.fileno(), self.onAccept)
		except Exception:
			listener.close()
			raise

		self._listener = listener
		INFO_MSG("Poller::start: listen %s:%s" % (addr, port))

	def stop(self):
		"""
		先注销引擎回调再关闭 socket，防止迟到完成事件访问已释放对象。
		Deregister engine callbacks before closing sockets so late completions cannot access released objects.
		"""
		if self._listener is not None:
			KBEngine.deregisterAcceptFileDescriptor(self._listener.fileno())
			self._listener.close()
			self._listener = None

		for clientFD in list(self._clients):
			self.closeClient(clientFD)

	def onAccept(self, listenerFD, clientFD, errorCode):
		"""
		接收引擎已经完成 accept 的客户端句柄并注册数据读取。
		Consume a client handle already accepted by the engine and register data reads.
		"""
		if errorCode != 0:
			ERROR_MSG("Poller::onAccept: listenerFD=%i error=%i" % (listenerFD, errorCode))
			return

		if self._listener is None or listenerFD != self._listener.fileno():
			self._closeAcceptedHandle(clientFD)
			return

		sock = None
		try:
			sock = socket.socket(fileno=clientFD)
			sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
			address = sock.getpeername()
			self._clients[clientFD] = {
				"socket": sock,
				"address": address,
				"buffer": bytearray(),
				"responded": False,
			}
			KBEngine.registerReadDataFileDescriptor(clientFD, self.onRead)
		except Exception:
			self._clients.pop(clientFD, None)
			if sock is not None:
				sock.close()
			raise

		DEBUG_MSG("Poller::onAccept: new channel[%s/%i]" % (address, clientFD))

	def onRead(self, clientFD, data, errorCode):
		"""
		消费引擎交付的数据，不再对同一 socket 调用 recv。
		Consume data delivered by the engine without calling recv on the same socket again.
		"""
		client = self._clients.get(clientFD)
		if client is None:
			return

		if errorCode != 0:
			ERROR_MSG("Poller::onRead: fd=%i error=%i" % (clientFD, errorCode))
			self.closeClient(clientFD)
			return

		if not data:
			DEBUG_MSG("Poller::onRead: %s/%i disconnect" % (client["address"], clientFD))
			self.closeClient(clientFD)
			return

		if client["responded"]:
			return

		client["buffer"].extend(data)
		if len(client["buffer"]) > self._MAX_REQUEST_SIZE:
			ERROR_MSG("Poller::onRead: fd=%i request is too large" % clientFD)
			self.closeClient(clientFD)
			return

		DEBUG_MSG("Poller::onRead: fd=%i dataSize=%i totalSize=%i" % (
			clientFD, len(data), len(client["buffer"])))

		if b"\r\n\r\n" not in client["buffer"]:
			return

		client["responded"] = True
		self.processData(client["socket"], bytes(client["buffer"]))

	def processData(self, sock, datas):
		"""
		保留原模板的扩展点，并用异步写 API 返回最小 HTTP 响应。
		Preserve the original template extension point and return a minimal HTTP response through the async write API.
		"""
		body = b"Hello KBEngine completion API\n"
		response = (
			b"HTTP/1.1 200 OK\r\n"
			b"Content-Type: text/plain; charset=utf-8\r\n"
			b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
			b"Connection: close\r\n\r\n" + body
		)
		KBEngine.writeFileDescriptor(sock.fileno(), response, self.onWriteComplete)

	def onWriteComplete(self, clientFD, bytesWritten, errorCode):
		"""
		完成一次响应后关闭连接；错误同样必须释放读注册和 socket。
		Close the connection after one response; failures must release the read registration and socket as well.
		"""
		if errorCode != 0:
			ERROR_MSG("Poller::onWriteComplete: fd=%i error=%i" % (clientFD, errorCode))
		else:
			DEBUG_MSG("Poller::onWriteComplete: fd=%i bytesWritten=%i" % (clientFD, bytesWritten))

		self.closeClient(clientFD)

	def closeClient(self, clientFD):
		"""
		幂等移除客户端，确保异常、EOF 和正常响应共用同一清理路径。
		Idempotently remove a client so errors, EOF, and successful responses share one cleanup path.
		"""
		client = self._clients.pop(clientFD, None)
		if client is None:
			return

		KBEngine.deregisterReadDataFileDescriptor(clientFD)
		client["socket"].close()
		DEBUG_MSG("Poller::closeClient: fd=%i" % clientFD)

	@staticmethod
	def _closeAcceptedHandle(clientFD):
		"""
		关闭无法归属当前 listener 的已接收句柄，避免句柄泄漏。
		Close an accepted handle that cannot belong to the active listener to prevent handle leaks.
		"""
		try:
			socket.socket(fileno=clientFD).close()
		except Exception:
			pass
