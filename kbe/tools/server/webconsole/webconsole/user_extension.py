from dataclasses import dataclass
from pathlib import Path
import os
import xml.etree.ElementTree as ET

from django.contrib.auth.models import AnonymousUser, User
from django.core.exceptions import PermissionDenied

from .models import KBEUserExtension


@dataclass(frozen=True)
class KBEUserExtensionContext:
    extension: KBEUserExtension
    system_user_uid: int
    system_username: str
    kbe_root: str
    kbe_res_path: str
    kbe_bin_path: str
    gui_console_admin_token: str


def get_kbe_user_extension(user: User) -> KBEUserExtension:
    """Return the per-user KBE settings row, creating it lazily for first use.
    返回用户的 KBE 扩展配置；首次进入后台时懒创建，避免未保存用户扩展数据导致页面 500。
    """
    if isinstance(user, AnonymousUser) or not user.is_authenticated:
        raise PermissionDenied("KBE user extension requires an authenticated Django user.")
    extension, _ = KBEUserExtension.objects.get_or_create(user=user)
    return extension


def get_kbe_user_context(user: User) -> KBEUserExtensionContext:
    """Normalize nullable form fields into values safe for Machine queries.
    将可空表单字段归一化为 Machine 查询可安全使用的值。

    The admin inline allows blank strings. Machine APIs expect uid as an int, so
    invalid or missing values are treated the same as the historical default 0.
    后台内联表单允许空字符串；Machine API 需要 int 类型 uid，因此缺失或非法值统一按历史默认 0 处理。
    """
    extension = get_kbe_user_extension(user)
    return KBEUserExtensionContext(
        extension=extension,
        system_user_uid=_safe_int(extension.system_user_uid, 0),
        system_username=_safe_str(extension.system_username),
        kbe_root=_safe_str(extension.kbe_root),
        kbe_res_path=_safe_str(extension.kbe_res_path),
        kbe_bin_path=_safe_str(extension.kbe_bin_path),
        gui_console_admin_token=resolve_gui_console_admin_token(extension),
    )


def resolve_gui_console_admin_token(extension: KBEUserExtension) -> str:
    """Resolve GUIConsole admin token using the same defaults-then-override rule as the server.
    按服务端一致的 defaults 后 overlay 规则解析 GUIConsole 管理 Token。
    """
    roots = _resource_roots(extension)
    token = _read_first_gui_console_admin_token(
        roots,
        ("server/kbengine_defaults.xml", "kbengine_defaults.xml"),
    )
    override = _read_first_gui_console_admin_token(
        roots,
        ("server/kbengine.xml", "kbengine.xml"),
    )
    return token if override is None else override


def _resource_roots(extension: KBEUserExtension) -> list[Path]:
    roots: list[Path] = []
    res_path = _safe_str(extension.kbe_res_path)
    if res_path:
        roots.extend(Path(item) for item in res_path.split(os.pathsep) if item)

    # WebConsole sits under kbe/tools/server/webconsole; this fallback lets log
    # watching work with the engine default token even before KBE_RES_PATH is filled.
    # WebConsole 位于 kbe/tools/server/webconsole；该回退允许用户未填写 KBE_RES_PATH 时
    # 仍能读取引擎默认 token 并订阅日志。
    engine_res = Path(__file__).resolve().parents[4] / "res"
    if engine_res not in roots:
        roots.append(engine_res)
    return roots


def _read_first_gui_console_admin_token(roots: list[Path], names: tuple[str, ...]) -> str | None:
    for root in roots:
        for name in names:
            candidate = root / name
            if not candidate.is_file():
                continue
            try:
                tree = ET.parse(candidate)
            except (OSError, ET.ParseError):
                continue
            token_node = tree.getroot().find("./guiConsole/adminToken")
            if token_node is not None:
                return (token_node.text or "").strip()
    return None


def _safe_str(value: object) -> str:
    return "" if value is None else str(value).strip()


def _safe_int(value: object, default: int) -> int:
    if value is None:
        return default
    try:
        text = str(value).strip()
        return int(text) if text else default
    except (TypeError, ValueError):
        return default
