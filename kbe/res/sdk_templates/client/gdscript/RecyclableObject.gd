class_name RecyclableObject
## 对象池可回收需继承此类

func clear()-> void:
	pass

func reclaimObject()-> void:
	ObjectPool.reclaimObject(self)
