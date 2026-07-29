from django.contrib import admin
from django.http import HttpResponse
from django.template.response import TemplateResponse

from KBESettings.custom_admin_site import custom_admin_site
from cluster.models import ServerManage
from webconsole.machines_mgr import machinesmgr
from webconsole.models import KBEUserExtension


@admin.register(ServerManage, site=custom_admin_site)
class ServerManageAdmin(admin.ModelAdmin):
    change_list_template = "cluster/server_manage.html"
    list_display = []

    def has_add_permission(self, request): return False
    def has_delete_permission(self, request, obj=None): return False

    def changelist_view(self, request, extra_context=None):
        ext = KBEUserExtension.objects.get(user=request.user)
        system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
        system_username = "" if ext.system_username is None else ext.system_username

        interfaces_groups = machinesmgr.queryAllInterfaces(system_user_uid, system_username)

        # [(machine, [components, ...]), ...]
        kbeComps = []
        for mID, comps in interfaces_groups.items():
            if len(comps) > 1:
                kbeComps.extend(comps[1:])

        context = {
            **self.admin_site.each_context(request),
            "title": self.model._meta.verbose_name_plural,
            "cl": {
                "model":self.model,
                "model_admin": self,
                "opts": self.model._meta
            },
            "KBEComps": kbeComps,
            "hasComponents": len(kbeComps) > 0,
            "hasMachines": len(interfaces_groups) > 0,
            **(extra_context or {}),
        }
        return TemplateResponse(request, self.change_list_template, context)
