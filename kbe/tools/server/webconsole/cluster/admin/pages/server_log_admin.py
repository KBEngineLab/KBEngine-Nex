from django.contrib import admin
from django.http import HttpResponse
from django.template.response import TemplateResponse

from KBESettings.custom_admin_site import custom_admin_site
from cluster.models import ServerManage, ServerLog
from pycommon import Define
from pycommon.LoggerWatcher import logName2type
from webconsole.machines_mgr import machinesmgr
from webconsole.models import KBEUserExtension


@admin.register(ServerLog, site=custom_admin_site)
class ServerManageAdmin(admin.ModelAdmin):
    change_list_template = "cluster/server_log.html"
    list_display = []

    def has_add_permission(self, request): return False
    def has_delete_permission(self, request, obj=None): return False

    def changelist_view(self, request, extra_context=None):
        ext = KBEUserExtension.objects.get(user=request.user)
        system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
        system_username = "" if ext.system_username is None else ext.system_username

        VALID_CT = {Define.LOGGER_TYPE}

        interfaces_groups = machinesmgr.queryAllInterfaces(system_user_uid, system_username)

        # [(machine, [components, ...]), ...]
        kbeComps = []
        for mID, comps in interfaces_groups.items():
            for comp in comps:
                if comp.componentType in VALID_CT:
                    kbeComps.append(comp)
        POST = request.POST
        try:
            intaddr = kbeComps[0].intaddr
            intport = kbeComps[0].intport
            extaddr = kbeComps[0].extaddr
            extport = kbeComps[0].extport
            # host = kbeComps[0].extaddr
            # port = kbeComps[0].consolePort
            uid = system_user_uid
        except:
            context = {
                **self.admin_site.each_context(request),
                "title": self.model._meta.verbose_name_plural,
                "cl": {
                    "model":self.model,
                    "model_admin": self,
                    "opts": self.model._meta
                },
                "unlogger": "logger进程未运行"
            }
            return TemplateResponse(request, self.change_list_template, context)

        # 获取进程选中状态
        components_checks = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
        components_checks2 = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
        baseapp_check = POST.get("baseapp_check")
        baseappmgr_check = POST.get("baseappmgr_check")
        cellapp_check = POST.get("cellapp_check")
        dbmgr_check = POST.get("dbmgr_check")
        loginapp_check = POST.get("loginapp_check")
        pull_state = POST.get("pull_state")

        if pull_state == 0:
            pull_state = 1

        if baseapp_check: components_checks[6] = Define.BASEAPP_TYPE
        if baseappmgr_check: components_checks[3] = Define.BASEAPPMGR_TYPE
        if cellapp_check: components_checks[5] = Define.CELLAPP_TYPE
        if dbmgr_check: components_checks[1] = Define.DBMGR_TYPE
        if loginapp_check: components_checks[2] = Define.LOGINAPP_TYPE
        if components_checks[6] == 0 \
                and components_checks[3] == 0 \
                and components_checks[5] == 0 \
                and components_checks[1] == 0 \
                and components_checks[2] == 0:
            components_checks[6] = Define.BASEAPP_TYPE
            components_checks[3] = Define.BASEAPPMGR_TYPE
            components_checks[5] = Define.CELLAPP_TYPE
            components_checks[1] = Define.DBMGR_TYPE
            components_checks[2] = Define.LOGINAPP_TYPE
            baseapp_check = 1
            baseappmgr_check = 1
            cellapp_check = 1
            dbmgr_check = 1
            loginapp_check = 1

        # if len(components_checks)<=1:components_checks[].append(Define.COMPONENT_END_TYPE)

        # 获取log类型
        CRITICAL_check = 0
        DEBUG_check = 0
        ERROR_check = 0
        INFO_check = 0
        PRINT_check = 0
        S_DBG_check = 0
        S_ERR_check = 0
        S_INFO_check = 0
        S_NORM_check = 0
        S_WARN_check = 0
        WARNING_check = 0
        logtype = 0x00000000
        CRITICAL = POST.get("CRITICAL")
        DEBUG = POST.get("DEBUG")
        ERROR = POST.get("ERROR")
        INFO = POST.get("INFO")
        PRINT = POST.get("PRINT")
        S_DBG = POST.get("S_DBG")
        S_ERR = POST.get("S_ERR")
        S_INFO = POST.get("S_INFO")
        S_NORM = POST.get("S_NORM")
        S_WARN = POST.get("S_WARN")
        WARNING = POST.get("WARNING")

        if CRITICAL:
            logtype |= logName2type["CRITICAL"]
            CRITICAL_check = 1
        if DEBUG:
            logtype |= logName2type["DEBUG"]
            DEBUG_check = 1
        if ERROR:
            logtype |= logName2type["ERROR"]
            ERROR_check = 1
        if INFO:
            logtype |= logName2type["INFO"]
            INFO_check = 1
        if PRINT:
            logtype |= logName2type["PRINT"]
            PRINT_check = 1
        if S_DBG:
            logtype |= logName2type["S_DBG"]
            S_DBG_check = 1
        if S_ERR:
            logtype |= logName2type["S_ERR"]
            S_ERR_check = 1
        if S_INFO:
            logtype |= logName2type["S_INFO"]
            S_INFO_check = 1
        if S_NORM:
            logtype |= logName2type["S_NORM"]
            S_NORM_check = 1
        if S_WARN:
            logtype |= logName2type["S_WARN"]
            S_WARN_check = 1
        if WARNING:
            logtype |= logName2type["S_WARN"]
            WARNING_check = 1
        if logtype == 0x00000000:
            logtype = 0xffffffff
            CRITICAL_check = 1
            DEBUG_check = 1
            ERROR_check = 1
            INFO_check = 1
            PRINT_check = 1
            S_DBG_check = 1
            S_ERR_check = 1
            S_INFO_check = 1
            S_NORM_check = 1
            S_WARN_check = 1
            WARNING_check = 1

        # 自定义搜索
        globalOrder = POST.get("globalOrder")
        groupOrder = POST.get("groupOrder")
        searchDate = POST.get("searchDate")
        keystr = POST.get("keystr")
        if globalOrder is None: globalOrder = ""
        if groupOrder is None: groupOrder = ""
        if searchDate is None: searchDate = ""
        if keystr is None: keystr = ""

        ws_url = "ws://%s/ws/server_manage/?&logtype=%s&extaddr=%s&extport=%s&uid=%s&baseapp_check=%s&baseappmgr_check=%s&cellapp_check=%s&dbmgr_check=%s&loginapp_check=%s&globalOrder=%s&groupOrder=%s&searchDate=%s&keystr=%s" % (
            request.META["HTTP_HOST"], logtype, extaddr, extport, uid, baseapp_check, baseappmgr_check, cellapp_check,
            dbmgr_check, loginapp_check, globalOrder, groupOrder, searchDate, keystr)



        context = {
            **self.admin_site.each_context(request),
            "title": self.model._meta.verbose_name_plural,
            "cl": {
                "model":self.model,
                "model_admin": self,
                "opts": self.model._meta
            },
            "ws_url": ws_url,
            "baseapp_check": components_checks[6],
            "baseappmgr_check": components_checks[3],
            "cellapp_check": components_checks[5],
            "dbmgr_check": components_checks[1],
            "loginapp_check": components_checks[2],

            "CRITICAL_check": CRITICAL_check,
            "DEBUG_check": DEBUG_check,
            "ERROR_check": ERROR_check,
            "INFO_check": INFO_check,
            "PRINT_check": PRINT_check,
            "S_DBG_check": S_DBG_check,
            "S_ERR_check": S_ERR_check,
            "S_INFO_check": S_INFO_check,
            "S_NORM_check": S_NORM_check,
            "S_WARN_check": S_WARN_check,
            "WARNING_check": WARNING_check,
            "globalOrder": globalOrder,
            "groupOrder": groupOrder,
            "searchDate": searchDate,
            "keystr" :keystr,

            "components_checks ": components_checks,

            "pull_stat": pull_state,
            **(extra_context or {}),
        }
        return TemplateResponse(request, self.change_list_template, context)
