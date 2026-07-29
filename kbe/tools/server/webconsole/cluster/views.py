import json
import time

from django.contrib.auth.decorators import login_required
from django.core.exceptions import ObjectDoesNotExist
from django.http import HttpResponseRedirect, JsonResponse
from django.shortcuts import render
from django.views.decorators.clickjacking import xframe_options_exempt

from KBESettings import settings
from cluster.models import ServerConfig
from pycommon import Define, Machines

from utils import kbe_util
from webconsole.machines_mgr import machinesmgr
from webconsole.models import KBEUserExtension


# ===================view===============================
def server_shutdown(request):
    """
    停止服务器
    """
    COMPS_FOR_SHUTDOWN = [
        Define.BOTS_TYPE,
        Define.LOGINAPP_TYPE,
        Define.CELLAPP_TYPE,
        Define.BASEAPP_TYPE,
        Define.CELLAPPMGR_TYPE,
        Define.BASEAPPMGR_TYPE,
        Define.DBMGR_TYPE,
        Define.INTERFACES_TYPE,
        Define.LOGGER_TYPE,
    ]

    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username

    components = Machines.Machines(system_user_uid, system_username)

    for ctid in COMPS_FOR_SHUTDOWN:
        hosts = kbe_util.get_machines_address()
        print(hosts)
        components.stopServer(ctid, trycount=0, targetIP=hosts)
    context = {
        "shutType": "all_ct"
    }
    return render(request, "cluster/server_shutdown.html", context)


def server_kill(request, ct, cid):
    """
    杀死一个组件进程
    """
    ct = int(ct)
    cid = int(cid)

    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username

    components = Machines.Machines(system_user_uid, system_username)
    hosts = kbe_util.get_machines_address()
    components.killServer(ct, componentID=cid, trycount=0, targetIP=hosts)
    context = {
        "shutType": "kill_cid",
        "ct": ct,
        "cid": cid
    }
    return render(request, "cluster/server_kill.html", context)

def server_runapp():
    pass

def server_run(request):
    """
    运行组件
    """

    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username
    kbe_root = "" if ext.kbe_root is None else ext.kbe_root
    kbe_bin_path = "" if ext.kbe_bin_path is None else ext.kbe_bin_path
    kbe_res_path = "" if ext.kbe_res_path is None else ext.kbe_res_path

    components = Machines.Machines(system_user_uid, system_username)
    context = {
        "ext":ext
    }

    POST = request.POST
    if POST.get("run", ""):
        componentType = int(POST.get("componentType", "0"))
        targetMachine = POST.get("targetMachine", "").strip()
        runNumber = int(POST.get("runNumber", "0"))
        # kbe_root = request.session["kbe_root"]
        # kbe_res_path = request.session["kbe_res_path"]
        # kbe_bin_path = request.session["kbe_bin_path"]

        if componentType not in Define.VALID_COMPONENT_TYPE_FOR_RUN or \
                not machinesmgr.hasMachine(targetMachine) or \
                runNumber <= 0:
            context = {"error": "invalid data!"}
        else:
            for e in range(runNumber):
                cid = machinesmgr.makeCID(componentType)
                gus = machinesmgr.makeGUS(componentType)
                print("cid: %s, gus: %s" % (cid, gus))

                components.startServer(componentType, cid, gus, targetMachine, kbe_root, kbe_res_path, kbe_bin_path)

            time.sleep(2)
            return JsonResponse({"result": "ok"})

    context["machines"] = machinesmgr.machines

    return render(request, "cluster/server_run.html", context)


def server_stop(request, ct, cid):
    """
    停止一个组件
    """
    ct = int(ct)
    cid = int(cid)

    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username

    components = Machines.Machines(system_user_uid, system_username)

    hosts = kbe_util.get_machines_address()

    components.stopServer(ct, componentID=cid, trycount=0, targetIP=hosts)
    context = {
        "shutType": "stop_cid",
        "ct": ct,
        "cid": cid
    }
    return render(request, "cluster/server_shutdown.html", context)



# ========================api==============================
def server_query(request):
    """
    请求获取组件数据
    """
    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username

    interfaces_groups = machinesmgr.queryAllInterfaces(system_user_uid, system_username)

    # [ [machine, other-components, ...], ...]
    kbeComps = []
    for mID, comps in interfaces_groups.items():
        if len(comps) <= 1:
            continue

        dl = []
        kbeComps.append(dl)
        for comp in comps:
            d = {
                "ip": comp.intaddr,
                "componentType": comp.componentType,
                "componentName": comp.componentName,
                "fullname": comp.fullname,
                "uid": comp.uid,
                "pid": comp.pid,
                "componentID": comp.componentID,
                "globalOrderID": comp.globalOrderID,
                "cpu": comp.cpu,
                "mem": comp.mem,
                "usedmem": comp.usedmem,
                "entities": comp.entities,
                "proxies": comp.proxies,
                "clients": comp.clients,
                "consolePort": comp.consolePort,
            }
            dl.append(d)

    return JsonResponse(kbeComps, safe=False)


def server_one_query(request, ct, cid):
    """
    请求获取一个组件数据
    """

    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username

    ct = int(ct)
    cid = int(cid)
    interfaces_groups = machinesmgr.queryAllInterfaces(system_user_uid, system_username)
    # [ [machine, other-components, ...], ...]
    kbeComps = []
    for mID, comps in interfaces_groups.items():
        if len(comps) <= 1:
            continue

        dl = []
        kbeComps.append(dl)
        for comp in comps:
            if comp.componentType == 8:
                d = {
                    "ip": comp.intaddr,
                    "componentType": comp.componentType,
                    "componentName": comp.componentName,
                    "fullname": comp.fullname,
                    "uid": comp.uid,
                    "pid": comp.pid,
                    "componentID": comp.componentID,
                    "globalOrderID": comp.globalOrderID,
                    "cpu": comp.cpu,
                    "mem": comp.mem,
                    "usedmem": comp.usedmem,
                    "entities": comp.entities,
                    "proxies": comp.proxies,
                    "clients": comp.clients,
                    "consolePort": comp.consolePort,
                }
                dl.append(d)

            if comp.componentID == cid:
                d = {
                    "ip": comp.intaddr,
                    "componentType": comp.componentType,
                    "componentName": comp.componentName,
                    "fullname": comp.fullname,
                    "uid": comp.uid,
                    "pid": comp.pid,
                    "componentID": comp.componentID,
                    "globalOrderID": comp.globalOrderID,
                    "cpu": comp.cpu,
                    "mem": comp.mem,
                    "usedmem": comp.usedmem,
                    "entities": comp.entities,
                    "proxies": comp.proxies,
                    "clients": comp.clients,
                    "consolePort": comp.consolePort,
                }
                dl.append(d)

            if ct == 3:
                if comp.componentType == 6 or comp.componentType == 3:
                    d = {
                        "ip": comp.intaddr,
                        "componentType": comp.componentType,
                        "componentName": comp.componentName,
                        "fullname": comp.fullname,
                        "uid": comp.uid,
                        "pid": comp.pid,
                        "componentID": comp.componentID,
                        "globalOrderID": comp.globalOrderID,
                        "cpu": comp.cpu,
                        "mem": comp.mem,
                        "usedmem": comp.usedmem,
                        "entities": comp.entities,
                        "proxies": comp.proxies,
                        "clients": comp.clients,
                        "consolePort": comp.consolePort,
                    }
                    dl.append(d)

            if ct == 4:
                if comp.componentType == 5 or comp.componentType == 4:
                    d = {
                        "ip": comp.intaddr,
                        "componentType": comp.componentType,
                        "componentName": comp.componentName,
                        "fullname": comp.fullname,
                        "uid": comp.uid,
                        "pid": comp.pid,
                        "componentID": comp.componentID,
                        "globalOrderID": comp.globalOrderID,
                        "cpu": comp.cpu,
                        "mem": comp.mem,
                        "usedmem": comp.usedmem,
                        "entities": comp.entities,
                        "proxies": comp.proxies,
                        "clients": comp.clients,
                        "consolePort": comp.consolePort,
                    }
                    dl.append(d)

    return JsonResponse(kbeComps, safe=False)


def server_save_config(request):
    """
    保存当前服务器运行状态
    """
    layoutName = request.GET.get("name")
    if not layoutName:
        result = {"state": "fault", "message": "invalid layout name!!!"}
        return JsonResponse(result, safe=False)

    VALID_CT = {
        Define.DBMGR_TYPE,
        Define.LOGINAPP_TYPE,
        Define.BASEAPPMGR_TYPE,
        Define.CELLAPPMGR_TYPE,
        Define.CELLAPP_TYPE,
        Define.BASEAPP_TYPE,
        Define.INTERFACES_TYPE,
        Define.LOGGER_TYPE
    }
    ext = KBEUserExtension.objects.get(user=request.user)
    system_user_uid = 0 if ext.system_user_uid is None else int(ext.system_user_uid)
    system_username = "" if ext.system_username is None else ext.system_username
    interfaces_groups = machinesmgr.queryAllInterfaces(system_user_uid, system_username)

    conf = {}

    for machineID, infos in interfaces_groups.items():
        for info in infos:
            if info.componentType not in VALID_CT:
                continue

            compnentName = Define.COMPONENT_NAME[info.componentType]
            if compnentName not in conf:
                conf[compnentName] = []
            d = {"ip": info.intaddr, "cid": info.componentID, "gus": info.genuuid_sections}
            conf[compnentName].append(d)

    if len(conf) == 0:
        result = {"state": "fault", "message": "当前没有服务器在运行!!!"}
        return JsonResponse(result, safe=False)

    try:
        m = ServerConfig.objects.get(name=layoutName)
    except ObjectDoesNotExist:
        m = ServerConfig()

    m.name = layoutName
    m.sys_user = system_username
    m.config = json.dumps(conf)
    m.save()

    result = {"state": "success", "message": ""}
    return JsonResponse(result, safe=False)



def server_log(request):
    pass