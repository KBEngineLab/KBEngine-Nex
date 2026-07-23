class_name MessageReaderTCP extends MessageReaderBase

enum READ_STATE
{
	# 消息ID
	READ_STATE_MSGID = 0,
	# 消息的长度65535以内
	READ_STATE_MSGLEN = 1,
	# 当上面的消息长度都无法到达要求时使用扩展长度
	# uint32
	READ_STATE_MSGLEN_EX = 2,
	# 消息的内容
	READ_STATE_BODY = 3
}

var msgid:int = 0
var msglen:int = 0
var expectSize:int = 2
var state:READ_STATE = READ_STATE.READ_STATE_MSGID
var stream:MemoryStream = ObjectPool.createObject(MemoryStream)


func process(_datas:PackedByteArray, _offset:int, _length:int)-> void:
	## print_debug("offest[" + str(_offset) + "]length[" + str(_length) +"]", _datas)
	var totallen:int = _offset
	while _length > 0 and self.expectSize > 0:
		if self.state == READ_STATE.READ_STATE_MSGID:
			if _length >= self.expectSize:
				self.stream.overWriteBuffer(_datas, totallen, self.stream.wpos, self.expectSize)
				## print_debug("1111 [[", self.stream.buffer)
				totallen += self.expectSize
				self.stream.wpos += self.expectSize
				_length -= self.expectSize
				self.msgid = self.stream.readUint16()
				self.stream.clear()
				var msg:Messages.Message = Messages.clientMessages.get(self.msgid, null)
				if msg == null:
					Dbg.ERROR_MSG("MessageReaderTCP::process: unknown message id " + str(self.msgid))
					self.stream.clear()
					self.state = READ_STATE.READ_STATE_MSGID
					self.expectSize = 2
				elif msg.msglen == -1:
					self.state = READ_STATE.READ_STATE_MSGLEN
					self.expectSize = 2
				elif msg.msglen == 0:
					# 如果是0个参数的消息，那么没有后续内容可读了，处理本条消息并且直接跳到下一条消息
					## print_debug("1666 msgid[%d]" % self.msgid)
					msg.handleMessage(self.stream)
					self.state = READ_STATE.READ_STATE_MSGID
					self.expectSize = 2
				else:
					self.expectSize = msg.msglen
					self.state = READ_STATE.READ_STATE_BODY
			else:
				self.stream.overWriteBuffer(_datas, totallen, self.stream.wpos, _length)
				## print_debug("2222 [[", self.stream.buffer)
				self.stream.wpos += _length
				self.expectSize -= _length
				break
		elif self.state == READ_STATE.READ_STATE_MSGLEN:
			if _length >= self.expectSize:
				self.stream.overWriteBuffer(_datas, totallen, self.stream.wpos, self.expectSize)
				## print_debug("2244 [[", self.stream.buffer)
				totallen += self.expectSize
				self.stream.wpos += self.expectSize
				_length -= self.expectSize
				self.msglen = self.stream.readUint16()
				self.stream.clear()
				# 长度扩展
				if self.msglen >= 65535:
					self.state = READ_STATE.READ_STATE_MSGLEN_EX
					self.expectSize = 4
				else:
					self.state = READ_STATE.READ_STATE_BODY
					self.expectSize = self.msglen
			else:
				self.stream.overWriteBuffer(_datas, totallen, self.stream.wpos, _length)
				## print_debug("3333 [[", self.stream.buffer)
				self.stream.wpos += _length
				self.expectSize -= _length
				break
		elif self.state == READ_STATE.READ_STATE_MSGLEN_EX:
			if _length >= self.expectSize:
				self.stream.overWriteBuffer(_datas, totallen, self.stream.wpos, self.expectSize)
				## print_debug("4444 [[", self.stream.buffer)
				totallen += self.expectSize
				self.stream.wpos += self.expectSize
				_length -= self.expectSize
				self.expectSize = self.stream.readUint32()
				self.stream.clear()
				self.state = READ_STATE.READ_STATE_BODY
			else:
				self.stream.overWriteBuffer(_datas, totallen, self.stream.wpos, _length)
				## print_debug("5555 [[", self.stream.buffer)
				self.stream.wpos += _length
				self.expectSize -= _length
				break
		elif self.state == READ_STATE.READ_STATE_BODY:
			if _length >= self.expectSize:
				self.stream.append(_datas, totallen, self.expectSize)
				totallen += self.expectSize
				_length -= self.expectSize
				var msg:Messages.Message = Messages.clientMessages.get(self.msgid, null)
				if msg == null:
					Dbg.ERROR_MSG("MessageReaderTCP::process(BODY): unknown message id " + str(self.msgid))
					self.stream.clear()
					self.state = READ_STATE.READ_STATE_MSGID
					self.expectSize = 2
				else:
					msg.handleMessage(self.stream)
				self.stream.clear()
				self.state = READ_STATE.READ_STATE_MSGID
				self.expectSize = 2
			else:
				self.stream.append(_datas, totallen, _length)
				self.expectSize -= _length
				break
