from django.contrib import admin
from django.template.response import TemplateResponse

from KBESettings.custom_admin_site import custom_admin_site

from pycommon import Define

from component.models import Status
from webconsole.machines_mgr import machinesmgr
from webconsole.models import KBEUserExtension


@admin.register(Status, site=custom_admin_site)
class StatusAdmin(admin.ModelAdmin):
    change_list_template = "component/status/components.html"
    list_display = []

    def has_add_permission(self, request): return False
    def has_delete_permission(self, request, obj=None): return False

    def changelist_view(self, request, extra_context=None):
        """
        控制台可连接的组件显示页面
        """
        VALID_CT = {Define.BASEAPPMGR_TYPE, Define.CELLAPPMGR_TYPE, Define.DBMGR_TYPE, Define.LOGINAPP_TYPE,
                    Define.CELLAPP_TYPE, Define.BASEAPP_TYPE, Define.INTERFACES_TYPE, Define.LOGGER_TYPE}

        ext = KBEUserExtension.objects.get(user=request.user)
        system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
        system_username = "" if ext.system_username is None else ext.system_username
        interfaces_groups = machinesmgr.queryAllInterfaces(system_user_uid, system_username)

        # [(machine, [components, ...]), ...]
        kbeComps = []
        for mID, comps in interfaces_groups.items():
            for comp in comps:
                if comp.componentType in VALID_CT:
                    kbeComps.append( comp)

        context = {
            **self.admin_site.each_context(request),
            "title": self.model._meta.verbose_name_plural,
            "cl": {
                "model":self.model,
                "model_admin": self,
                "opts": self.model._meta,
            },
            "http_host":request.META["HTTP_HOST"],
            "KBEComps" : kbeComps,
            **(extra_context or {}),
        }
        return TemplateResponse(request, self.change_list_template, context)
