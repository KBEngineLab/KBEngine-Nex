from django.contrib import admin

# Register your models here.
from django.contrib import admin
from django.contrib.auth.models import User, Group
from django.contrib.auth.admin import UserAdmin as BaseUserAdmin, GroupAdmin
from django.shortcuts import render
from django.template.response import TemplateResponse

from KBESettings.custom_admin_site import custom_admin_site
from .models import KBEUserExtension

class KBEUserExtensionInline(admin.StackedInline):
    model = KBEUserExtension
    can_delete = False
    verbose_name= '用户扩展数据'


class UserAdmin(BaseUserAdmin):
    inlines = (KBEUserExtensionInline,)
    # 创建用户时显示的字段
    add_fieldsets = (
        (None, {
            'classes': ('wide',),
            'fields': (
                'username',
                'password1',
                'password2',
                'is_active',  # ✅ 是否有效
                'is_staff',  # ✅ 是否后台可登录
                'is_superuser',  # ✅ 是否超级管理员
                'groups',  # ✅ 用户组
                'user_permissions'  # ✅ 单独权限
            ),
        }),
    )
admin.site = custom_admin_site

# admin.site.unregister(User)
admin.site.register(User, UserAdmin)
admin.site.register(Group, GroupAdmin)
