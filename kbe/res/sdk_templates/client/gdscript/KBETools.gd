class_name KBETools

static var reg:RegEx = RegEx.new()

static func is_subclass_of(_cls: Script, _base_script: Script) -> bool:
	var b : Script = _cls
	while b:
		if b == _base_script:
			return true
		b = b.get_base_script()
	return false
	
#region 数据格式转换
static func utf8ArrayToString(_bytes:PackedByteArray) -> String:
	if _bytes is PackedByteArray:
		return _bytes.get_string_from_utf8()
	else:
		Dbg.ERROR_MSG("utf8ArrayToString报错：%s不是utf8字节流" % _bytes)
		return ""
		
static func stringToUTF8Bytes(_str:String) -> PackedByteArray:
	return _str.to_utf8_buffer()

#endregion

static func IsIpAddress(_ip: String) -> bool:
	reg.compile(r"^((?:(?:25[0-5]|2[0-4]\d|((1\d{2})|([1-9]?\d)))\.){3}(?:25[0-5]|2[0-4]\d|((1\d{2})|([1-9]?\d))))$")
	return reg.search(_ip) != null

static func validEmail(_strEmail:String) -> bool:
	reg.compile(r"^([\w-\.]+)@((\[[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.)|(([\w-]+\.)+))([a-zA-Z]{2,4}|[0-9]{1,3})(\]?)$")
	return reg.search(_strEmail) != null

static func int82angle(_angle:float, _half:bool)-> float:
	var halfv:float = 128.0
	if _half:
		halfv = 254.0
	halfv = _angle * PI / halfv
	return halfv

static func almostEqual(_f1:float, _f2:float, _epsilon:float) -> bool:
	return abs(_f1 - _f2) < _epsilon

static func isNumeric(_v) -> bool:
	return _v is int or _v is float or _v is bool
