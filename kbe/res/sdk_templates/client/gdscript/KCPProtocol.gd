class_name KCPProtocol
## KCP 可靠 UDP 协议实现 (GDScript)
## 移植自 C# deps/KCP.cs，适配 Godot 4.x

#region Constants
const IKCP_RTO_NDL:int = 30         # no delay min rto
const IKCP_RTO_MIN:int = 100        # normal min rto
const IKCP_RTO_DEF:int = 200
const IKCP_RTO_MAX:int = 60000
const IKCP_CMD_PUSH:int = 81        # cmd: push data
const IKCP_CMD_ACK:int = 82         # cmd: ack
const IKCP_CMD_WASK:int = 83        # cmd: window probe (ask)
const IKCP_CMD_WINS:int = 84        # cmd: window size (tell)
const IKCP_ASK_SEND:int = 1         # need to send IKCP_CMD_WASK
const IKCP_ASK_TELL:int = 2         # need to send IKCP_CMD_WINS
const IKCP_WND_SND:int = 32
const IKCP_WND_RCV:int = 32
const IKCP_MTU_DEF:int = 1400
const IKCP_ACK_FAST:int = 3
const IKCP_INTERVAL:int = 100
const IKCP_OVERHEAD:int = 24
const IKCP_DEADLINK:int = 20
const IKCP_THRESH_INIT:int = 2
const IKCP_THRESH_MIN:int = 2
const IKCP_PROBE_INIT:int = 7000    # 7 secs to probe window size
const IKCP_PROBE_LIMIT:int = 120000 # up to 120 secs to probe window
#endregion

#region TimeUtils
class TimeUtils:
	static func iclock()-> int:
		return Time.get_ticks_msec() & 0xFFFFFFFF
#endregion

#region Segment 内部类
class Segment:
	var conv:int = 0
	var cmd:int = 0
	var frg:int = 0
	var wnd:int = 0
	var ts:int = 0
	var sn:int = 0
	var una:int = 0
	var resendts:int = 0
	var rto:int = 0
	var faskack:int = 0
	var xmit:int = 0
	var data:PackedByteArray
	
	func _init(_size:int = 0)-> void:
		data = PackedByteArray()
		data.resize(_size)
	
	func encode(_ptr:PackedByteArray, _offset:int)-> int:
		var len:int = data.size()
		_encode32u(_ptr, _offset, conv)
		_encode8u(_ptr, _offset + 4, cmd)
		_encode8u(_ptr, _offset + 5, frg)
		_encode16u(_ptr, _offset + 6, wnd)
		_encode32u(_ptr, _offset + 8, ts)
		_encode32u(_ptr, _offset + 12, sn)
		_encode32u(_ptr, _offset + 16, una)
		_encode32u(_ptr, _offset + 20, len)
		return IKCP_OVERHEAD
	
	static func _encode8u(_p:PackedByteArray, _off:int, _c:int)-> void:
		_p[_off] = _c & 0xFF
	static func _encode16u(_p:PackedByteArray, _off:int, _v:int)-> void:
		_p[_off] = _v & 0xFF
		_p[_off + 1] = (_v >> 8) & 0xFF
	static func _encode32u(_p:PackedByteArray, _off:int, _v:int)-> void:
		_p[_off] = _v & 0xFF
		_p[_off + 1] = (_v >> 8) & 0xFF
		_p[_off + 2] = (_v >> 16) & 0xFF
		_p[_off + 3] = (_v >> 24) & 0xFF
#endregion

#region 静态编解码工具
static func _decode8u(_p:PackedByteArray, _off:int)-> Array:
	return [_p[_off] & 0xFF, _off + 1]
static func _decode16u(_p:PackedByteArray, _off:int)-> Array:
	var r:int = (_p[_off] & 0xFF) | ((_p[_off + 1] & 0xFF) << 8)
	return [r, _off + 2]
static func _decode32u(_p:PackedByteArray, _off:int)-> Array:
	var r:int = (_p[_off] & 0xFF) | ((_p[_off + 1] & 0xFF) << 8) | ((_p[_off + 2] & 0xFF) << 16) | ((_p[_off + 3] & 0xFF) << 24)
	# 确保无符号32位范围
	return [r & 0xFFFFFFFF, _off + 4]
static func _imin_(_a:int, _b:int)-> int:
	return _a if _a <= _b else _b
static func _imax_(_a:int, _b:int)-> int:
	return _a if _a >= _b else _b
static func _ibound_(_lower:int, _middle:int, _upper:int)-> int:
	return _imin_(_imax_(_lower, _middle), _upper)
static func _itimediff(_later:int, _earlier:int)-> int:
	# KCP 协议期望带符号的 int32 差值（用于判断时间先后）
	# C# 版本: return (Int32)(later - earlier)
	# Godot int 是 64 位，需要先做 uint32 回绕再恢复带符号 int32
	var result:int = (_later - _earlier) & 0xFFFFFFFF
	if result > 0x7FFFFFFF:
		result -= 0x100000000
	return result
#endregion

#region KCP 主体
var conv:int = 0
var mtu:int = 0
var mss:int = 0
var state:int = 0

var snd_una:int = 0
var snd_nxt:int = 0
var rcv_nxt:int = 0
var ssthresh:int = 0

var rx_rttval:int = 0
var rx_srtt:int = 0
var rx_rto:int = 0
var rx_minrto:int = 0

var snd_wnd:int = 0
var rcv_wnd:int = 0
var rmt_wnd:int = 0
var cwnd:int = 0
var probe:int = 0

var current:int = 0
var interval:int = 0
var ts_flush:int = 0
var xmit:int = 0

var nrcv_buf:int = 0
var nsnd_buf:int = 0
var nrcv_que:int = 0
var nsnd_que:int = 0

var nodelay:int = 0
var updated:int = 0
var ts_probe:int = 0
var probe_wait:int = 0
var dead_link:int = 0
var incr:int = 0

var snd_queue:Array[Segment] = []
var rcv_queue:Array[Segment] = []
var snd_buf:Array[Segment] = []
var rcv_buf:Array[Segment] = []

var acklist:Array[int] = []
var ackcount:int = 0
var ackblock:int = 0

var buffer:PackedByteArray
var user:Object

var fastresend:int = 0
var nocwnd:int = 0

# 输出回调: func(data:PackedByteArray, size:int, user:Object)
var output:Callable

func _init(_conv:int, _user:Object)-> void:
	user = _user
	conv = _conv
	snd_wnd = IKCP_WND_SND
	rcv_wnd = IKCP_WND_RCV
	rmt_wnd = IKCP_WND_RCV
	mtu = IKCP_MTU_DEF
	mss = mtu - IKCP_OVERHEAD
	rx_rto = IKCP_RTO_DEF
	rx_minrto = IKCP_RTO_MIN
	interval = IKCP_INTERVAL
	ts_flush = IKCP_INTERVAL
	ssthresh = IKCP_THRESH_INIT
	dead_link = IKCP_DEADLINK
	buffer = PackedByteArray()
	buffer.resize((mtu + IKCP_OVERHEAD) * 3)

func release()-> void:
	snd_buf.clear()
	rcv_buf.clear()
	snd_queue.clear()
	rcv_queue.clear()
	nrcv_buf = 0
	nsnd_buf = 0
	nrcv_que = 0
	nsnd_que = 0
	ackblock = 0
	ackcount = 0
	buffer = PackedByteArray()
	acklist.clear()

func set_output(_output:Callable)-> void:
	output = _output

func set_mtu(_mtu:int)-> int:
	if _mtu < 50 or _mtu < IKCP_OVERHEAD:
		return -1
	var _buf:PackedByteArray = PackedByteArray()
	_buf.resize((_mtu + IKCP_OVERHEAD) * 3)
	mtu = _mtu
	mss = mtu - IKCP_OVERHEAD
	buffer = _buf
	return 0

func set_interval(_iv:int)-> int:
	if _iv > 5000: _iv = 5000
	elif _iv < 10: _iv = 10
	interval = _iv
	return 0

func no_delay(_nodelay:int, _interval:int, _resend:int, _nc:int)-> int:
	if _nodelay >= 0:
		nodelay = _nodelay
		if _nodelay > 0:
			rx_minrto = IKCP_RTO_NDL
		else:
			rx_minrto = IKCP_RTO_MIN
	if _interval >= 0:
		if _interval > 5000: _interval = 5000
		elif _interval < 10: _interval = 10
		interval = _interval
	if _resend >= 0:
		fastresend = _resend
	if _nc >= 0:
		nocwnd = _nc
	return 0

func set_minrto(_minrto:int)-> int:
	rx_minrto = _minrto
	return 0

func wnd_size(_sndwnd:int, _rcvwnd:int)-> int:
	if _sndwnd > 0: snd_wnd = _sndwnd
	if _rcvwnd > 0: rcv_wnd = _rcvwnd
	return 0

func peek_size()-> int:
	if rcv_queue.is_empty():
		return -1
	var seg:Segment = rcv_queue[0]
	if seg.frg == 0:
		return seg.data.size()
	if nrcv_que < seg.frg + 1:
		return -1
	var length:int = 0
	for _seg:Segment in rcv_queue:
		length += _seg.data.size()
		if _seg.frg == 0:
			break
	return length

func recv(_buffer:PackedByteArray, _offset:int, _len:int)-> int:
	var ispeek:int = 1 if _len < 0 else 0
	var recover:int = 0
	
	if rcv_queue.is_empty():
		return -1
	
	if _len < 0:
		_len = -_len
	
	var peeksize:int = peek_size()
	if peeksize < 0:
		return -2
	if peeksize > _len:
		return -3
	
	if nrcv_que >= rcv_wnd:
		recover = 1
	
	_len = 0
	var to_remove:Array[int] = []
	for i:int in range(rcv_queue.size()):
		var seg:Segment = rcv_queue[i]
		if _buffer != null:
			var _b:PackedByteArray = seg.data
			for j:int in range(_b.size()):
				if _offset + j < _buffer.size():
					_buffer[_offset + j] = _b[j]
			_offset += _b.size()
		_len += seg.data.size()
		to_remove.append(i)
		if seg.frg == 0:
			break
	
	# 移除已消费的段
	for _idx:int in range(to_remove.size() - 1, -1, -1):
		if ispeek == 0:
			rcv_queue.remove_at(to_remove[_idx])
			nrcv_que -= 1
	
	# 将 rcv_buf 中可用数据移动到 rcv_queue
	while not rcv_buf.is_empty():
		var seg:Segment = rcv_buf[0]
		if seg.sn == rcv_nxt and nrcv_que < rcv_wnd:
			rcv_buf.remove_at(0)
			nrcv_buf -= 1
			rcv_queue.append(seg)
			nrcv_que += 1
			rcv_nxt += 1
		else:
			break
	
	# fast recover
	if nrcv_que < rcv_wnd and recover != 0:
		probe |= IKCP_ASK_TELL
	
	return _len

func send(_buffer:PackedByteArray, _offset:int, _len:int)-> int:
	if _len < 0:
		return -1
	
	if mss <= 0:
		return -1
	
	var count:int = 1
	if _len <= mss:
		count = 1
	else:
		count = (_len + mss - 1) / mss
	
	if count > 255:
		return -2
	if count == 0:
		count = 1
	
	for i:int in range(count):
		var size:int = mss if _len > mss else _len
		var seg:Segment = Segment.new(size)
		if _buffer != null and _len > 0:
			for j:int in range(size):
				if _offset + j < _buffer.size():
					seg.data[j] = _buffer[_offset + j]
			_offset += size
		seg.frg = count - i - 1
		snd_queue.append(seg)
		nsnd_que += 1
		_len -= size
	return 0

func input(_data:PackedByteArray, _offset:int, _size:int)-> int:
	var maxack:int = 0
	var flag:int = 0
	
	if _data == null or _size < IKCP_OVERHEAD:
		return -1
	
	var off:int = _offset
	while true:
		if _size < IKCP_OVERHEAD:
			break
		
		var _r:Array
		_r = _decode32u(_data, off); var _conv:int = _r[0]; off = _r[1]
		if conv != _conv:
			return -1
		_r = _decode8u(_data, off); var cmd:int = _r[0]; off = _r[1]
		_r = _decode8u(_data, off); var frg:int = _r[0]; off = _r[1]
		_r = _decode16u(_data, off); var wnd:int = _r[0]; off = _r[1]
		_r = _decode32u(_data, off); var ts:int = _r[0]; off = _r[1]
		_r = _decode32u(_data, off); var sn:int = _r[0]; off = _r[1]
		_r = _decode32u(_data, off); var una:int = _r[0]; off = _r[1]
		_r = _decode32u(_data, off); var length:int = _r[0]; off = _r[1]
		
		_size -= IKCP_OVERHEAD
		if _size < length:
			return -2
		
		if cmd != IKCP_CMD_PUSH and cmd != IKCP_CMD_ACK and cmd != IKCP_CMD_WASK and cmd != IKCP_CMD_WINS:
			return -3
		
		rmt_wnd = wnd
		_parse_una(una)
		_shrink_buf()
		
		if cmd == IKCP_CMD_ACK:
			if _itimediff(current, ts) >= 0:
				_update_ack(_itimediff(current, ts))
			_parse_ack(sn)
			_shrink_buf()
			if flag == 0:
				flag = 1
				maxack = sn
			elif _itimediff(sn, maxack) > 0:
				maxack = sn
		elif cmd == IKCP_CMD_PUSH:
			if _itimediff(sn, rcv_nxt + rcv_wnd) < 0:
				_ack_push(sn, ts)
				if _itimediff(sn, rcv_nxt) >= 0:
					var seg:Segment = Segment.new(length)
					seg.conv = _conv
					seg.cmd = cmd
					seg.frg = frg
					seg.wnd = wnd
					seg.ts = ts
					seg.sn = sn
					seg.una = una
					if length > 0:
						for j:int in range(length):
							seg.data[j] = _data[off + j]
					_parse_data(seg)
		elif cmd == IKCP_CMD_WASK:
			probe |= IKCP_ASK_TELL
		elif cmd == IKCP_CMD_WINS:
			pass  # do nothing
		else:
			return -3
		
		off += length
		_size -= length
	
	if flag != 0:
		_parse_fastack(maxack)
	
	# 拥塞控制
	if _itimediff(snd_una, 0) > 0:  # snd_una increased
		if cwnd < rmt_wnd:
			if cwnd < ssthresh:
				cwnd += 1
				incr += mss
			else:
				if incr < mss: incr = mss
				incr += (mss * mss) / incr + (mss / 16)
				if (cwnd + 1) * mss <= incr:
					cwnd += 1
			if cwnd > rmt_wnd:
				cwnd = rmt_wnd
				incr = rmt_wnd * mss
	
	return 0

func update(_current:int)-> void:
	current = _current
	if updated == 0:
		updated = 1
		ts_flush = current
	
	var slap:int = _itimediff(current, ts_flush)
	if slap >= 10000 or slap < -10000:
		ts_flush = current
		slap = 0
	
	if slap >= 0:
		ts_flush += interval
		if _itimediff(current, ts_flush) >= 0:
			ts_flush = current + interval
		_flush()

func check(_current:int)-> int:
	var ts_flush_local:int = ts_flush
	var tm_flush:int = 0x7fffffff
	var tm_packet:int = 0x7fffffff
	
	if updated == 0:
		return _current
	
	if _itimediff(_current, ts_flush_local) >= 10000 or _itimediff(_current, ts_flush_local) < -10000:
		ts_flush_local = _current
	
	if _itimediff(_current, ts_flush_local) >= 0:
		return _current
	
	tm_flush = _itimediff(ts_flush_local, _current)
	
	for seg:Segment in snd_buf:
		var diff:int = _itimediff(seg.resendts, _current)
		if diff <= 0:
			return _current
		if diff < tm_packet:
			tm_packet = diff
	
	var minimal:int = tm_packet if tm_packet < tm_flush else tm_flush
	if minimal >= interval:
		minimal = interval
	
	return _current + minimal

#region 内部方法
func _update_ack(_rtt:int)-> void:
	if rx_srtt == 0:
		rx_srtt = _rtt
		rx_rttval = _rtt / 2
	else:
		var delta:int = _rtt - rx_srtt
		if delta < 0: delta = -delta
		rx_rttval = (3 * rx_rttval + delta) / 4
		rx_srtt = (7 * rx_srtt + _rtt) / 8
		if rx_srtt < 1: rx_srtt = 1
	var rto:int = rx_srtt + _imax_(interval, 4 * rx_rttval)
	rx_rto = _ibound_(rx_minrto, rto, IKCP_RTO_MAX)

func _shrink_buf()-> void:
	if not snd_buf.is_empty():
		snd_una = snd_buf[0].sn
	else:
		snd_una = snd_nxt

func _parse_ack(_sn:int)-> void:
	if _itimediff(_sn, snd_una) < 0 or _itimediff(_sn, snd_nxt) >= 0:
		return
	
	var to_remove:int = -1
	for i:int in range(snd_buf.size()):
		var seg:Segment = snd_buf[i]
		if _sn == seg.sn:
			to_remove = i
			break
		if _itimediff(_sn, seg.sn) < 0:
			break
	
	if to_remove >= 0:
		snd_buf.remove_at(to_remove)
		nsnd_buf -= 1

func _parse_una(_una:int)-> void:
	var to_remove:Array[int] = []
	for i:int in range(snd_buf.size()):
		var seg:Segment = snd_buf[i]
		if _itimediff(_una, seg.sn) > 0:
			to_remove.append(i)
		else:
			break
	for _idx:int in range(to_remove.size() - 1, -1, -1):
		snd_buf.remove_at(to_remove[_idx])
		nsnd_buf -= 1

func _parse_fastack(_sn:int)-> void:
	if _itimediff(_sn, snd_una) < 0 or _itimediff(_sn, snd_nxt) >= 0:
		return
	
	for seg:Segment in snd_buf:
		if _itimediff(_sn, seg.sn) < 0:
			break
		elif _sn != seg.sn:
			seg.faskack += 1

func _ack_push(_sn:int, _ts:int)-> void:
	var newsize:int = ackcount + 1
	if newsize > ackblock:
		var newblock:int = 8
		while newblock < newsize:
			newblock <<= 1
		var newacklist:Array[int] = []
		newacklist.resize(newblock * 2)
		for i:int in range(ackcount):
			newacklist[i * 2] = acklist[i * 2]
			newacklist[i * 2 + 1] = acklist[i * 2 + 1]
		acklist = newacklist
		ackblock = newblock
	acklist[ackcount * 2] = _sn
	acklist[ackcount * 2 + 1] = _ts
	ackcount += 1

func _ack_get(_pos:int)-> Array:
	return [acklist[_pos * 2], acklist[_pos * 2 + 1]]

func _parse_data(_newseg:Segment)-> void:
	var sn:int = _newseg.sn
	var repeat:int = 0
	
	if _itimediff(sn, rcv_nxt + rcv_wnd) >= 0 or _itimediff(sn, rcv_nxt) < 0:
		return
	
	# 在 rcv_buf 中查找插入位置（保持 sn 升序）
	var insert_idx:int = 0
	for i:int in range(rcv_buf.size() - 1, -1, -1):
		var seg:Segment = rcv_buf[i]
		if seg.sn == sn:
			repeat = 1
			break
		if _itimediff(sn, seg.sn) > 0:
			insert_idx = i + 1
			break
	
	if repeat == 0:
		rcv_buf.insert(insert_idx, _newseg)
		nrcv_buf += 1
	
	# 将 rcv_buf 中可用数据移动到 rcv_queue
	while not rcv_buf.is_empty():
		var seg:Segment = rcv_buf[0]
		if seg.sn == rcv_nxt and nrcv_que < rcv_wnd:
			rcv_buf.remove_at(0)
			nrcv_buf -= 1
			rcv_queue.append(seg)
			nrcv_que += 1
			rcv_nxt += 1
		else:
			break

func _wnd_unused()-> int:
	if nrcv_que < rcv_wnd:
		return rcv_wnd - nrcv_que
	return 0

func _flush()-> void:
	var change:int = 0
	var lost:int = 0
	var offset:int = 0
	
	if updated == 0:
		return
	
	# 构建 ACK segment 模板
	var ack_seg:Segment = Segment.new()
	ack_seg.conv = conv
	ack_seg.cmd = IKCP_CMD_ACK
	ack_seg.wnd = _wnd_unused()
	ack_seg.una = rcv_nxt
	
	# flush acknowledges
	var count:int = ackcount
	for i:int in range(count):
		if (offset + IKCP_OVERHEAD) > mtu:
			if output.is_valid():
				output.call(buffer, offset, user)
			offset = 0
		var _ar:Array = _ack_get(i)
		ack_seg.sn = _ar[0]
		ack_seg.ts = _ar[1]
		ack_seg.encode(buffer, offset)
		offset += IKCP_OVERHEAD
	ackcount = 0
	
	# probe window size
	if rmt_wnd == 0:
		if probe_wait == 0:
			probe_wait = IKCP_PROBE_INIT
			ts_probe = current + probe_wait
		elif _itimediff(current, ts_probe) >= 0:
			if probe_wait < IKCP_PROBE_INIT: probe_wait = IKCP_PROBE_INIT
			probe_wait += probe_wait / 2
			if probe_wait > IKCP_PROBE_LIMIT: probe_wait = IKCP_PROBE_LIMIT
			ts_probe = current + probe_wait
			probe |= IKCP_ASK_SEND
	else:
		ts_probe = 0
		probe_wait = 0
	
	# flush window probing
	if (probe & IKCP_ASK_SEND) != 0:
		ack_seg.cmd = IKCP_CMD_WASK
		if (offset + IKCP_OVERHEAD) > mtu:
			if output.is_valid(): output.call(buffer, offset, user)
			offset = 0
		ack_seg.encode(buffer, offset)
		offset += IKCP_OVERHEAD
	
	if (probe & IKCP_ASK_TELL) != 0:
		ack_seg.cmd = IKCP_CMD_WINS
		if (offset + IKCP_OVERHEAD) > mtu:
			if output.is_valid(): output.call(buffer, offset, user)
			offset = 0
		ack_seg.encode(buffer, offset)
		offset += IKCP_OVERHEAD
	
	probe = 0
	
	# calculate window size
	var _cwnd:int = _imin_(snd_wnd, rmt_wnd)
	if nocwnd == 0:
		_cwnd = _imin_(_cwnd, cwnd)
	
	# move data from snd_queue to snd_buf
	while _itimediff(snd_nxt, snd_una + _cwnd) < 0:
		if snd_queue.is_empty():
			break
		var newseg:Segment = snd_queue.pop_front()
		snd_buf.append(newseg)
		nsnd_que -= 1
		nsnd_buf += 1
		
		newseg.conv = conv
		newseg.cmd = IKCP_CMD_PUSH
		newseg.wnd = ack_seg.wnd
		newseg.ts = current
		newseg.sn = snd_nxt
		snd_nxt += 1
		newseg.una = rcv_nxt
		newseg.resendts = current
		newseg.rto = rx_rto
		newseg.faskack = 0
		newseg.xmit = 0
	
	# calculate resent
	var resent:int = fastresend if fastresend > 0 else 0xffffffff
	var rtomin:int = 0 if nodelay == 0 else (rx_rto >> 3)
	
	# flush data segments
	for seg:Segment in snd_buf:
		var needsend:int = 0
		if seg.xmit == 0:
			needsend = 1
			seg.xmit += 1
			seg.rto = rx_rto
			seg.resendts = current + seg.rto + rtomin
		elif _itimediff(current, seg.resendts) >= 0:
			needsend = 1
			seg.xmit += 1
			xmit += 1
			if nodelay == 0:
				seg.rto += rx_rto
			else:
				seg.rto += rx_rto / 2
			seg.resendts = current + seg.rto
			lost = 1
		elif seg.faskack >= resent:
			needsend = 1
			seg.xmit += 1
			seg.faskack = 0
			seg.resendts = current + seg.rto
			change += 1
		
		if needsend > 0:
			seg.ts = current
			seg.wnd = ack_seg.wnd
			seg.una = rcv_nxt
			
			var need:int = IKCP_OVERHEAD + seg.data.size()
			if offset + need > mtu:
				if output.is_valid(): output.call(buffer, offset, user)
				offset = 0
			
			seg.encode(buffer, offset)
			offset += IKCP_OVERHEAD
			if seg.data.size() > 0:
				for j:int in range(seg.data.size()):
					buffer[offset + j] = seg.data[j]
				offset += seg.data.size()
			
			if seg.xmit >= dead_link:
				state = 0xFFFFFFFF
	
	# flush remain
	if offset > 0:
		if output.is_valid(): output.call(buffer, offset, user)
	
	# update ssthresh
	if change > 0:
		var inflight:int = snd_nxt - snd_una
		ssthresh = inflight / 2
		if ssthresh < IKCP_THRESH_MIN: ssthresh = IKCP_THRESH_MIN
		cwnd = ssthresh + resent
		incr = cwnd * mss
	
	if lost > 0:
		ssthresh = _cwnd / 2
		if ssthresh < IKCP_THRESH_MIN: ssthresh = IKCP_THRESH_MIN
		cwnd = 1
		incr = mss
	
	if cwnd < 1:
		cwnd = 1
		incr = mss
#endregion
#endregion
