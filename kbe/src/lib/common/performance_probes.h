#ifndef KBE_PERFORMANCE_PROBES_H
#define KBE_PERFORMANCE_PROBES_H

namespace KBEngine
{

/**
 * 进程启动时由 ServerConfig 设置，运行期间只读，避免热路径引入配置模块依赖。
 * Set by ServerConfig during process startup and read-only at runtime to keep configuration dependencies out of hot paths.
 */
extern bool g_performanceProbesEnabled;

}

#endif // KBE_PERFORMANCE_PROBES_H
