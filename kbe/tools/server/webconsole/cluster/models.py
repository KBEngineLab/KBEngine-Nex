from django.db import models

class ServerConfig(models.Model):
    """
    服务器运行配置表
    """
    id = models.AutoField(primary_key=True)
    name = models.CharField(max_length=128, default="", help_text="名称", db_index=True, unique=True,verbose_name="配置名")
    sys_user = models.CharField(max_length=128, default="", help_text="系统账号",verbose_name="系统账号")
    config = models.TextField(max_length=32768, default="", help_text="配置(JSON)",verbose_name="配置")


    class Meta:
        app_label = 'cluster'          # ✅ 指定所属应用标签
        verbose_name = '服务器运行配置'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "服务器运行配置"

class ServerManage(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'cluster'  # ✅ 指定所属应用标签
        verbose_name = '服务器管理'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "服务器管理"


class ServerLog(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'cluster'  # ✅ 指定所属应用标签
        verbose_name = '实时日志'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "实时日志"

class ServerMachines(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'cluster'  # ✅ 指定所属应用标签
        verbose_name = '进程状态'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "进程状态"