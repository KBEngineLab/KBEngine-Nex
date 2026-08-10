from dataclasses import dataclass

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
    )


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
