from django.db.models.signals import post_save
from django.dispatch import receiver
from django.contrib.auth.models import User
from .models import KBEUserExtension

# @receiver(post_save, sender=User)
# def create_kbe_user_extension(sender, instance, created, **kwargs):
#     if created:
#         KBEUserExtension.objects.get_or_create(user=instance)

# @receiver(post_save, sender=User)
# def save_kbe_user_extension(sender, instance, **kwargs):
#     instance.kbeuserextension.save()