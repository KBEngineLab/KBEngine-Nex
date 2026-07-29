from django.contrib import admin
from django.template.response import TemplateResponse

from KBESettings.custom_admin_site import custom_admin_site
from cluster.models import ServerMachines
from webconsole.machines_mgr import machinesmgr
from webconsole.models import KBEUserExtension


@admin.register(ServerMachines, site=custom_admin_site)
class ServerMachinesAdmin(admin.ModelAdmin):
    change_list_template = "cluster/server_machines.html"
    list_display = []

    def has_add_permission(self, request): return False
    def has_delete_permission(self, request, obj=None): return False

    def changelist_view(self, request, extra_context=None):
        """
            忽略用户，显示所有的machine
            """
        interfaces_groups = machinesmgr.queryAllInterfaces(0, "WebConsole")

        targetIP = request.GET.get("target", None)

        kbeComps = []
        for mID, comps in interfaces_groups.items():
            if len(comps) > 1 and comps[0].intaddr == targetIP:
                kbeComps = comps[1:]
                break

        kbeMachines = machinesmgr.queryMachines()
        kbeMachines.sort(key=lambda info: info.intaddr)

        context = {
            **self.admin_site.each_context(request),
            "title": self.model._meta.verbose_name_plural,
            "cl": {
                "model":self.model,
                "model_admin": self,
                "opts": self.model._meta
            },
            "KBEMachines": kbeMachines,
            "KBEComps": kbeComps,
            **(extra_context or {}),
        }
        return TemplateResponse(request, self.change_list_template, context)
