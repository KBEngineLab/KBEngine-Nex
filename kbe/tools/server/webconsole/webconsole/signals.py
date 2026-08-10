from django.contrib.auth.models import User
from django.db.models.signals import post_save
from django.dispatch import receiver

from .models import KBEUserExtension

@receiver(post_save, sender=User)
def ensure_kbe_user_extension(sender, instance, **kwargs):
    KBEUserExtension.objects.get_or_create(user=instance)
