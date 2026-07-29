import inspect
import json
import os

import KBEngine


# 调试器需要延迟展开这些容器，避免一次性序列化完整对象图。
# The debugger expands these containers lazily to avoid serializing the complete object graph at once.
BASIC_CONTAINER_TYPES = (list, tuple, dict, set)

# 这些引擎入口返回对象集合，应作为子节点而不是普通属性显示。
# These engine entry points return object collections and should be displayed as child nodes rather than plain attributes.
CHILDREN_FUNC = ("entities", "globalData", "baseAppData", "cellAppData")


def _member_info(member):
    """生成调试协议需要的成员元数据。
    Build member metadata required by the debugger protocol.
    """
    info = {
        "member": member,
        "callable": callable(member),
        "type": f"{type(member).__module__}.{type(member).__name__}",
    }
    if inspect.isfunction(member) or inspect.ismethod(member) or callable(member):
        try:
            info["sig"] = inspect.signature(member)
        except (TypeError, ValueError):
            info["sig"] = "(?)"
    return info


def get_obj_info(obj, children_id="", get_value=False, show_method_wrapper=False):
    """输出 KBEX 调试器可识别的对象描述。
    Emit an object description recognized by the KBEX debugger.

    保留未使用参数以兼容现有调试器调用协议。
    Keep the unused parameters for compatibility with the existing debugger call protocol.
    """
    del get_value, show_method_wrapper
    try:
        attrs = {}
        children = {}
        for name, member in inspect.getmembers(obj):
            target = children if name in CHILDREN_FUNC or type(member) in BASIC_CONTAINER_TYPES else attrs
            target[name] = _member_info(member)
            if name in CHILDREN_FUNC and callable(member):
                target[name]["sig"] = "()"

        if isinstance(obj, (list, tuple, set)):
            for index, item in enumerate(obj):
                children[index] = {
                    "__key_type__": type(index).__name__,
                    **_member_info(item),
                }
        elif isinstance(obj, dict) or obj.__class__.__name__ in ("GlobalDataClient", "Entities"):
            for key, value in obj.items():
                children[key] = {
                    "__key_type__": type(key).__name__,
                    **_member_info(value),
                }

        result = {
            "id": children_id,
            "attrs": attrs,
            "childrens": children,
            "type": f"{type(obj).__module__}.{type(obj).__name__}",
            "callable": callable(obj),
            "path": parse_kbe_entity_path(obj),
        }
        KBEngine.scriptLogType(-1)
        print("--KBEX--" + json.dumps(result, ensure_ascii=False, default=str) + "--KBEX--")
    except Exception:
        # 调试查询不能中断服务进程，空对象是调试协议约定的失败响应。
        # A debug query must not interrupt the server process; an empty object is the protocol-defined failure response.
        print("--KBEX--" + json.dumps({}, ensure_ascii=False, default=str) + "--KBEX--")


def parse_kbe_entity_path(obj):
    """根据定义文件路径判断实体运行域。
    Determine the entity execution domain from its definition file path.
    """
    try:
        file_path = inspect.getfile(obj.__class__)
        if not file_path or not isinstance(file_path, str):
            return ""
        if file_path.startswith("<") and file_path.endswith(">"):
            return ""

        path = os.path.normpath(file_path).replace("\\", "/").lower()
        if "/base/components/" in path:
            return "base_entity_component"
        if "/base/" in path:
            return "base_entity"
        if "/cell/components/" in path:
            return "cell_entity_component"
        if "/cell/" in path:
            return "cell_entity"
    except (OSError, TypeError):
        pass
    return ""
