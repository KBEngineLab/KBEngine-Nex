class_name Dbg

enum DEBUGLEVEL
{
	DEBUG = 0,
	INFO,
	WARNING,
	ERROR,

	NOLOG,  # 放在最后面，使用这个时表示不输出任何日志（!!!慎用!!!）
}

static var debugLevel:DEBUGLEVEL = DEBUGLEVEL.DEBUG

static func _format(_level:String, _msg:String)-> String:
	return "KBEngine %s:: %s" % [_level, _msg]

static func INFO_MSG(_msg:String)-> void:
	if DEBUGLEVEL.INFO >= debugLevel:
		print(_format("INFO", _msg))

static func DEBUG_MSG(_msg:String)-> void:
	if DEBUGLEVEL.DEBUG >= debugLevel:
		print(_format("DEBUG", _msg))

static func WARNING_MSG(_msg:String)-> void:
	if DEBUGLEVEL.WARNING >= debugLevel:
		var msg:String = _format("WARNING", _msg)
		print(msg)
		push_warning(msg)

static func ERROR_MSG(_msg:String)-> void:
	if DEBUGLEVEL.ERROR >= debugLevel:
		var msg:String = _format("ERROR", _msg)
		printerr(msg)
		push_error(msg)

#region 性能分析（参考 C# Profile.cs + Dbg.cs）
static var m_profiles:Dictionary = {}
static var m_enableProfile:bool = false

class Profile:
	var name:String
	var startTime:int
	var totalTime:int = 0
	var maxTime:int = 0
	var count:int = 0
	
	func start()-> void:
		startTime = Time.get_ticks_msec()
	
	func end()-> void:
		var elapsed:int = Time.get_ticks_msec() - startTime
		totalTime += elapsed
		count += 1
		if elapsed > maxTime:
			maxTime = elapsed
		if elapsed >= 100:
			Dbg.WARNING_MSG("Profile::" + name + ": took " + str(elapsed) + " ms")

static func profileStart(_name:String)-> void:
	if not m_enableProfile:
		return
	var p:Profile = m_profiles.get(_name)
	if p == null:
		p = Profile.new()
		p.name = _name
		m_profiles[_name] = p
	p.start()

static func profileEnd(_name:String)-> void:
	if not m_enableProfile:
		return
	var p:Profile = m_profiles.get(_name)
	if p != null:
		p.end()
#endregion
