from django.db import models

# Create your models here.
from django.db import models
from django.contrib.auth.models import User

class KBEUserExtension(models.Model):
    # 一对一绑定系统User
    user = models.OneToOneField(User, on_delete=models.CASCADE)

    # 添加你自己的字段
    system_username = models.CharField(max_length=30, blank=True, null=True,verbose_name="操作系统用户")
    system_user_uid = models.CharField(max_length=30, blank=True, null=True,verbose_name="操作系统用户uid")
    kbe_root = models.CharField(max_length=256, blank=True, null=True,verbose_name="KBE_ROOT")
    kbe_res_path = models.CharField(max_length=500, blank=True, null=True,verbose_name="KBE_RES_PATH")
    kbe_bin_path = models.CharField(max_length=500, blank=True, null=True,verbose_name="KBE_BIN_PATH")


    def __str__(self):
        return self.user.username
