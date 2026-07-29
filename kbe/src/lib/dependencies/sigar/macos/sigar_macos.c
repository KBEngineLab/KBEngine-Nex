#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libproc.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/resource.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include "sigar.h"
#include "sigar_format.h"

struct sigar_t
{
    char errbuf[256];
};

static int sigar_macos_fill_proc_time(sigar_pid_t pid, sigar_proc_time_t* proctime)
{
    struct rusage_info_v4 rinfo;
    int rc = proc_pid_rusage((int)pid, RUSAGE_INFO_V4, (rusage_info_t*)&rinfo);
    if (rc != 0)
    {
        return errno ? errno : SIGAR_ENOENT;
    }

    proctime->start_time = (sigar_uint64_t)(rinfo.ri_proc_start_abstime / 1000000ULL);
    proctime->user = (sigar_uint64_t)(rinfo.ri_user_time / 1000000ULL);
    proctime->sys = (sigar_uint64_t)(rinfo.ri_system_time / 1000000ULL);
    proctime->total = proctime->user + proctime->sys;
    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_open(sigar_t **sigar)
{
    if (!sigar)
    {
        return EINVAL;
    }

    *sigar = (sigar_t*)calloc(1, sizeof(sigar_t));
    if (!*sigar)
    {
        return ENOMEM;
    }

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_close(sigar_t *sigar)
{
    if (sigar)
    {
        free(sigar);
    }

    return SIGAR_OK;
}

SIGAR_DECLARE(sigar_pid_t) sigar_pid_get(sigar_t *sigar)
{
    (void)sigar;
    return (sigar_pid_t)getpid();
}

SIGAR_DECLARE(int) sigar_proc_kill(sigar_pid_t pid, int signum)
{
    return kill((pid_t)pid, signum);
}

SIGAR_DECLARE(int) sigar_signum_get(char *name)
{
    if (!name)
    {
        return -1;
    }

    if (strcasecmp(name, "TERM") == 0 || strcasecmp(name, "SIGTERM") == 0)
    {
        return SIGTERM;
    }

    if (strcasecmp(name, "KILL") == 0 || strcasecmp(name, "SIGKILL") == 0)
    {
        return SIGKILL;
    }

    if (strcasecmp(name, "INT") == 0 || strcasecmp(name, "SIGINT") == 0)
    {
        return SIGINT;
    }

    return -1;
}

SIGAR_DECLARE(char *) sigar_strerror(sigar_t *sigar, int err)
{
    if (!sigar)
    {
        return (char*)"sigar handle is null";
    }

    if (err == SIGAR_ENOTIMPL)
    {
        strncpy(sigar->errbuf, "This function has not been implemented on this platform", sizeof(sigar->errbuf));
        sigar->errbuf[sizeof(sigar->errbuf) - 1] = '\0';
        return sigar->errbuf;
    }

    const char* msg = strerror(err);
    if (!msg)
    {
        msg = "unknown error";
    }

    strncpy(sigar->errbuf, msg, sizeof(sigar->errbuf));
    sigar->errbuf[sizeof(sigar->errbuf) - 1] = '\0';
    return sigar->errbuf;
}

SIGAR_DECLARE(int) sigar_mem_get(sigar_t *sigar, sigar_mem_t *mem)
{
    (void)sigar;

    if (!mem)
    {
        return EINVAL;
    }

    memset(mem, 0, sizeof(*mem));

    uint64_t physical_mem = 0;
    size_t physical_mem_len = sizeof(physical_mem);
    if (sysctlbyname("hw.memsize", &physical_mem, &physical_mem_len, NULL, 0) != 0)
    {
        return errno;
    }

    mach_msg_type_number_t host_size = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vm_stat;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm_stat, &host_size);
    if (kr != KERN_SUCCESS)
    {
        return SIGAR_ENOTIMPL;
    }

    vm_size_t page_size = 0;
    host_page_size(mach_host_self(), &page_size);

    sigar_uint64_t free_bytes = (sigar_uint64_t)(vm_stat.free_count + vm_stat.inactive_count) * (sigar_uint64_t)page_size;
    sigar_uint64_t used_bytes = (sigar_uint64_t)physical_mem > free_bytes ? (sigar_uint64_t)physical_mem - free_bytes : 0;

    mem->ram = (sigar_uint64_t)(physical_mem / (1024ULL * 1024ULL));
    mem->total = (sigar_uint64_t)physical_mem;
    mem->free = free_bytes;
    mem->used = used_bytes;
    mem->actual_free = free_bytes;
    mem->actual_used = used_bytes;
    mem->used_percent = mem->total ? ((double)mem->actual_used / (double)mem->total) * 100.0 : 0.0;
    mem->free_percent = 100.0 - mem->used_percent;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_cpu_get(sigar_t *sigar, sigar_cpu_t *cpu)
{
    (void)sigar;

    if (!cpu)
    {
        return EINVAL;
    }

    memset(cpu, 0, sizeof(*cpu));

    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    host_cpu_load_info_data_t load = {0};
    kern_return_t kr = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&load, &count);
    if (kr != KERN_SUCCESS)
    {
        return SIGAR_ENOTIMPL;
    }

    cpu->user = (sigar_uint64_t)load.cpu_ticks[CPU_STATE_USER];
    cpu->nice = (sigar_uint64_t)load.cpu_ticks[CPU_STATE_NICE];
    cpu->sys = (sigar_uint64_t)load.cpu_ticks[CPU_STATE_SYSTEM];
    cpu->idle = (sigar_uint64_t)load.cpu_ticks[CPU_STATE_IDLE];
    cpu->wait = 0;
    cpu->irq = 0;
    cpu->soft_irq = 0;
    cpu->stolen = 0;
    cpu->total = cpu->user + cpu->nice + cpu->sys + cpu->idle;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_cpu_list_get(sigar_t *sigar, sigar_cpu_list_t *cpulist)
{
    if (!cpulist)
    {
        return EINVAL;
    }

    memset(cpulist, 0, sizeof(*cpulist));

    int logical_cpus = 1;
    size_t len = sizeof(logical_cpus);
    if (sysctlbyname("hw.logicalcpu", &logical_cpus, &len, NULL, 0) != 0 || logical_cpus <= 0)
    {
        logical_cpus = 1;
    }

    cpulist->number = (unsigned long)logical_cpus;
    cpulist->size = (unsigned long)logical_cpus;
    cpulist->data = (sigar_cpu_t*)calloc((size_t)logical_cpus, sizeof(sigar_cpu_t));
    if (!cpulist->data)
    {
        return ENOMEM;
    }

    sigar_cpu_t aggregate;
    int status = sigar_cpu_get(sigar, &aggregate);
    if (status != SIGAR_OK)
    {
        free(cpulist->data);
        cpulist->data = NULL;
        cpulist->number = 0;
        cpulist->size = 0;
        return status;
    }

    for (int i = 0; i < logical_cpus; ++i)
    {
        cpulist->data[i] = aggregate;
    }

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_cpu_list_destroy(sigar_t *sigar, sigar_cpu_list_t *cpulist)
{
    (void)sigar;

    if (!cpulist)
    {
        return EINVAL;
    }

    free(cpulist->data);
    cpulist->data = NULL;
    cpulist->number = 0;
    cpulist->size = 0;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_list_get(sigar_t *sigar, sigar_proc_list_t *proclist)
{
    (void)sigar;

    if (!proclist)
    {
        return EINVAL;
    }

    memset(proclist, 0, sizeof(*proclist));

    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0)
    {
        return errno ? errno : SIGAR_ENOTIMPL;
    }

    int count = bytes / (int)sizeof(pid_t);
    pid_t* pids = (pid_t*)calloc((size_t)count, sizeof(pid_t));
    if (!pids)
    {
        return ENOMEM;
    }

    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes);
    if (bytes < 0)
    {
        free(pids);
        return errno;
    }

    int filled = bytes / (int)sizeof(pid_t);

    proclist->data = (sigar_pid_t*)calloc((size_t)filled, sizeof(sigar_pid_t));
    if (!proclist->data)
    {
        free(pids);
        return ENOMEM;
    }

    unsigned long out = 0;
    for (int i = 0; i < filled; ++i)
    {
        if (pids[i] > 0)
        {
            proclist->data[out++] = (sigar_pid_t)pids[i];
        }
    }

    free(pids);

    proclist->number = out;
    proclist->size = out;
    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_list_destroy(sigar_t *sigar, sigar_proc_list_t *proclist)
{
    (void)sigar;

    if (!proclist)
    {
        return EINVAL;
    }

    free(proclist->data);
    proclist->data = NULL;
    proclist->number = 0;
    proclist->size = 0;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_state_get(sigar_t *sigar, sigar_pid_t pid,
                                        sigar_proc_state_t *procstate)
{
    (void)sigar;

    if (!procstate)
    {
        return EINVAL;
    }

    struct proc_bsdinfo bsdinfo;
    int rc = proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &bsdinfo, sizeof(bsdinfo));
    if (rc <= 0)
    {
        return errno ? errno : SIGAR_ENOENT;
    }

    memset(procstate, 0, sizeof(*procstate));
    strncpy(procstate->name, bsdinfo.pbi_name, sizeof(procstate->name) - 1);

    switch (bsdinfo.pbi_status)
    {
        case SIDL:
            procstate->state = SIGAR_PROC_STATE_IDLE;
            break;
        case SRUN:
            procstate->state = SIGAR_PROC_STATE_RUN;
            break;
        case SSLEEP:
            procstate->state = SIGAR_PROC_STATE_SLEEP;
            break;
        case SSTOP:
            procstate->state = SIGAR_PROC_STATE_STOP;
            break;
        case SZOMB:
            procstate->state = SIGAR_PROC_STATE_ZOMBIE;
            break;
        default:
            procstate->state = SIGAR_PROC_STATE_RUN;
            break;
    }

    procstate->ppid = (sigar_pid_t)bsdinfo.pbi_ppid;
    procstate->threads = (sigar_uint64_t)bsdinfo.pbi_nfiles;
    procstate->processor = 0;
    procstate->priority = 0;
    procstate->nice = bsdinfo.pbi_nice;
    procstate->tty = bsdinfo.e_tdev;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_time_get(sigar_t *sigar, sigar_pid_t pid,
                                       sigar_proc_time_t *proctime)
{
    (void)sigar;

    if (!proctime)
    {
        return EINVAL;
    }

    memset(proctime, 0, sizeof(*proctime));
    return sigar_macos_fill_proc_time(pid, proctime);
}

SIGAR_DECLARE(int) sigar_proc_cpu_get(sigar_t *sigar, sigar_pid_t pid,
                                      sigar_proc_cpu_t *proccpu)
{
    (void)sigar;

    if (!proccpu)
    {
        return EINVAL;
    }

    memset(proccpu, 0, sizeof(*proccpu));

    sigar_proc_time_t pt;
    int status = sigar_macos_fill_proc_time(pid, &pt);
    if (status != SIGAR_OK)
    {
        return status;
    }

    proccpu->start_time = pt.start_time;
    proccpu->user = pt.user;
    proccpu->sys = pt.sys;
    proccpu->total = pt.total;
    proccpu->last_time = pt.total;
    proccpu->percent = 0.0;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_mem_get(sigar_t *sigar, sigar_pid_t pid,
                                      sigar_proc_mem_t *procmem)
{
    (void)sigar;

    if (!procmem)
    {
        return EINVAL;
    }

    memset(procmem, 0, sizeof(*procmem));

    struct proc_taskinfo taskinfo;
    int rc = proc_pidinfo((int)pid, PROC_PIDTASKINFO, 0, &taskinfo, sizeof(taskinfo));
    if (rc <= 0)
    {
        return errno ? errno : SIGAR_ENOENT;
    }

    procmem->resident = (sigar_uint64_t)taskinfo.pti_resident_size;
    procmem->size = (sigar_uint64_t)taskinfo.pti_virtual_size;
    procmem->share = 0;
    procmem->minor_faults = (sigar_uint64_t)taskinfo.pti_faults;
    procmem->major_faults = 0;
    procmem->page_faults = procmem->minor_faults;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_cpu_perc_calculate(sigar_cpu_t *prev,
                                            sigar_cpu_t *curr,
                                            sigar_cpu_perc_t *perc)
{
    if (!prev || !curr || !perc)
    {
        return EINVAL;
    }

    memset(perc, 0, sizeof(*perc));

    sigar_uint64_t user = curr->user - prev->user;
    sigar_uint64_t sys = curr->sys - prev->sys;
    sigar_uint64_t nice = curr->nice - prev->nice;
    sigar_uint64_t idle = curr->idle - prev->idle;
    sigar_uint64_t wait = curr->wait - prev->wait;
    sigar_uint64_t irq = curr->irq - prev->irq;
    sigar_uint64_t soft_irq = curr->soft_irq - prev->soft_irq;
    sigar_uint64_t stolen = curr->stolen - prev->stolen;

    sigar_uint64_t total = user + sys + nice + idle + wait + irq + soft_irq + stolen;
    if (total == 0)
    {
        return SIGAR_OK;
    }

    perc->user = (double)user / (double)total;
    perc->sys = (double)sys / (double)total;
    perc->nice = (double)nice / (double)total;
    perc->idle = (double)idle / (double)total;
    perc->wait = (double)wait / (double)total;
    perc->irq = (double)irq / (double)total;
    perc->soft_irq = (double)soft_irq / (double)total;
    perc->stolen = (double)stolen / (double)total;
    perc->combined = 1.0 - perc->idle;

    return SIGAR_OK;
}
