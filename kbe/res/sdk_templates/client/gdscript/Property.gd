class_name Property
## 抽象出一个entitydef中定义的属性
## 该模块描述了属性的id以及数据类型等信息

enum EntityDataFlags
{
	ED_FLAG_UNKOWN						= 0x00000000, # 未定义
	ED_FLAG_CELL_PUBLIC					= 0x00000001, # 相关所有cell广播
	ED_FLAG_CELL_PRIVATE				= 0x00000002, # 当前cell
	ED_FLAG_ALL_CLIENTS					= 0x00000004, # cell广播与所有客户端
	ED_FLAG_CELL_PUBLIC_AND_OWN			= 0x00000008, # cell广播与自己的客户端
	ED_FLAG_OWN_CLIENT					= 0x00000010, # 当前cell和客户端
	ED_FLAG_BASE_AND_CLIENT				= 0x00000020, # base和客户端
	ED_FLAG_BASE						= 0x00000040, # 当前base
	ED_FLAG_OTHER_CLIENTS				= 0x00000080, # cell广播和其他客户端
}
				
var name:String = ""
var properUtype:int = 0
var properFlags:int = 0
var aliasID:int = -1
var defaultVal

func _init():
	pass

func isBase()-> bool:
	return self.properFlags == EntityDataFlags.ED_FLAG_BASE_AND_CLIENT \
			or self.properFlags == EntityDataFlags.ED_FLAG_BASE

func isOwnerOnly()-> bool:
	return self.properFlags == EntityDataFlags.ED_FLAG_CELL_PUBLIC_AND_OWN \
		or self.properFlags == EntityDataFlags.ED_FLAG_OWN_CLIENT

func isOtherOnly()-> bool:
	return self.properFlags == EntityDataFlags.ED_FLAG_OTHER_CLIENTS
