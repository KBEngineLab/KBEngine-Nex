class_name ObjectPool
static var m_objects: Dictionary[String, Array] = {}
static var mutex: Mutex = Mutex.new()



static func createObject(_cls:Script)-> RecyclableObject:
	if not KBETools.is_subclass_of(_cls, RecyclableObject):
		return
	mutex.lock() # 线程锁
	var _clsName:String = _cls.to_string()
	var rst:RecyclableObject
	if m_objects.has(_clsName) and m_objects[_clsName].size() > 0:
		## print_debug("pop" + str(_cls))
		rst = m_objects[_clsName].pop_back()
	mutex.unlock()
	if rst == null:
		## print_debug("创建" + str(_cls))
		rst = _cls.new()
	rst.clear()
	return rst

static func reclaimObject(_item:RecyclableObject)-> void:
	if _item is RecyclableObject:
		## print_debug("回收" + str(_item.get_script()) + "引用计数：" + str(_item.get_reference_count()))
		_item.clear()
		var _clsName:String = _item.get_script().to_string()
		mutex.lock() # 线程锁
		if m_objects.has(_clsName):
			m_objects[_clsName].append(_item)
		else:
			m_objects[_clsName] = [_item]
		mutex.unlock()
	elif _item:
		Dbg.WARNING_MSG("对象需要继承RecyclableObject才可回收")
	else:
		Dbg.WARNING_MSG("对象在回收前已被置空")
