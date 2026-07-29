class_name RecyclableObject extends RefCounted

# 帧解析只使用 MemoryStream 的数据生命周期，不应把对象池实现耦合进这个单元测试。
# Frame parsing only uses MemoryStream's data lifetime and must not couple the object-pool implementation into this unit test.
func clear()-> void:
	pass

func reclaimObject()-> void:
	pass
