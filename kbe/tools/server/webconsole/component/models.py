from django.db import models

class SpaceViewer(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'component'  # ✅ 指定所属应用标签
        verbose_name = 'SpaceViewer'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "SpaceViewer"


class Watcher(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'component'  # ✅ 指定所属应用标签
        verbose_name = 'Watcher'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "Watcher"


class PythonConsole(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'component'  # ✅ 指定所属应用标签
        verbose_name = 'Python控制台'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "Python控制台"


class Profile(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'component'  # ✅ 指定所属应用标签
        verbose_name = '性能分析'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "性能分析"


class Status(models.Model):
    class Meta:
        managed = False  # 不需要创建数据库表
        app_label = 'component'  # ✅ 指定所属应用标签
        verbose_name = '状态'
        verbose_name_plural = verbose_name

    def __str__(self):
        return "状态"
