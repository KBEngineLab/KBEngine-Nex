import json

from django.template.response import TemplateResponse
from django.utils.html import format_html
from django.utils.safestring import mark_safe

from KBESettings.custom_admin_site import custom_admin_site
from cluster.models import ServerConfig
from django.contrib import admin, messages

from pycommon import Define, Machines
from webconsole.machines_mgr import machinesmgr
from webconsole.models import KBEUserExtension


#
# @admin.register(ServerConfig)
# class ServerConfigAdmin(admin.ModelAdmin):
#     change_list_template = "cluster/server_config.html"
#     list_display = []
#
#     def has_add_permission(self, request): return False
#     def has_delete_permission(self, request, obj=None): return False
#
#     def changelist_view(self, request, extra_context=None):
#         context = {
#             **self.admin_site.each_context(request),
#             "title": self.model._meta.verbose_name_plural,
#             "cl": {
#                 "model":self.model,
#                 "model_admin": self,
#                 "opts": self.model._meta
#             },
#             **(extra_context or {}),
#         }
#         return TemplateResponse(request, self.change_list_template, context)


# admin.site.register(ServerConfig)

def load_servers(modeladmin, request, queryset):
    """
    加载某个保存的服务器运行配置，并启动服务器
    """
    if queryset.count() != 1:
        messages.error(request, "只能选择一条服务器配置执行此操作。")
        return

    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username

    server_config = queryset.first()

    server_app_run_order = [
        Define.LOGGER_TYPE,
        Define.INTERFACES_TYPE,
        Define.DBMGR_TYPE,
        Define.BASEAPPMGR_TYPE,
        Define.CELLAPPMGR_TYPE,
        Define.CELLAPP_TYPE,
        Define.BASEAPP_TYPE,
        Define.LOGINAPP_TYPE,
    ]



    kbe_root = "" if ext.kbe_root is None else ext.kbe_root
    kbe_res_path = "" if ext.kbe_res_path is None else ext.kbe_res_path
    kbe_bin_path = "" if ext.kbe_bin_path is None else ext.kbe_bin_path

    # if kbe_root == "":
    #     messages.error(request, "请配置KBE_ROOT。")
    #     return
    if kbe_res_path == "":
        messages.error(request, "请配置KBE_RES_PATH。")
        return
    if kbe_bin_path == "":
        messages.error(request, "请配置KBE_BIN_PATH。")
        return

    components = Machines.Machines(system_user_uid, system_username)
    interfaces_groups = machinesmgr.queryAllInterfaces(system_user_uid, system_username)

    for mID, comps in interfaces_groups.items():
        if len(comps) > 1:
            messages.error(request, "服务器正在运行，不允许加载。")
            return

    # 计数器
    t2c = [0, ] * len(Define.COMPONENT_NAME)
    components_ct = [0, ] * len(Define.COMPONENT_NAME)
    components_cid = [0, ] * len(Define.COMPONENT_NAME)
    components_gus = [0, ] * len(Define.COMPONENT_NAME)

    layout_data = json.loads(server_config.config)


    for server_app in server_app_run_order:
        component_name = Define.COMPONENT_NAME[server_app]
        components_ct[server_app] = server_app
        for comp in layout_data.get(component_name, []):
            print("components_load_layout(), component data: %s" % comp)
            cid = comp["cid"]
            if cid <= 0:
                cid = machinesmgr.makeCID(server_app)
            components_cid[server_app] = cid

            gus = comp["gus"]
            if gus <= 0:
                gus = machinesmgr.makeGUS(server_app)
            components_gus[server_app] = gus
            t2c[server_app] += 1
            components.startServer(server_app, cid, gus, comp["ip"], kbe_root, kbe_res_path, kbe_bin_path, 0)


    messages.success(request, mark_safe(f"已发送 [{server_config.name}] 配置请求，<a href='/admin/cluster/servermanage/'>点击前往</a>查看状态。"))

load_servers.short_description = "加载服务器配置"


@admin.register(ServerConfig, site=custom_admin_site)
class ServerConfigAdmin(admin.ModelAdmin):
    # 在后台列表页显示哪些字段（列）
    list_display = ("id", "name", "sys_user", "cellapp","baseapp","cellappmgr","baseappmgr","loginapp","dbmgr","interfaces","logger")

    # 允许搜索的字段
    search_fields = ("name", "sys_user")

    # 允许过滤的字段（右侧筛选栏）
    list_filter = ("sys_user",)

    # 每页显示的行数
    list_per_page = 20

    # 是否允许点击进入详情页
    list_display_links = ("id", "name")

    # 可选：控制哪些字段可编辑（直接在列表页修改）
    # list_editable = ("sys_user", "config")
    actions = [load_servers]

    def cellapp(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("cellapp", []))
        except Exception:
            return "(解析错误)"
    def baseapp(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("baseapp", []))
        except Exception:
            return "(解析错误)"
    def cellappmgr(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("cellappmgr", []))
        except Exception:
            return "(解析错误)"
    def baseappmgr(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("baseappmgr", []))
        except Exception:
            return "(解析错误)"
    def loginapp(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("loginapp", []))
        except Exception:
            return "(解析错误)"
    def dbmgr(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("dbmgr", []))
        except Exception:
            return "(解析错误)"
    def interfaces(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("interfaces", []))
        except Exception:
            return "(解析错误)"
    def logger(self,obj):
        try:
            data = json.loads(obj.config)
            return len(data.get("logger", []))
        except Exception:
            return "(解析错误)"
