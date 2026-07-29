class_name KBEngine

# MemoryStream 的默认容量是本测试所需的唯一 KBEngine 全局契约，隔离它可避免帧解析测试加载完整客户端运行时。
# MemoryStream's default capacity is the only KBEngine global contract needed here; isolating it avoids loading the full client runtime for a frame parser test.
const PACKET_MAX_SIZE_TCP:int = 1460
