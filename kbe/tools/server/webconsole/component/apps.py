from django.apps import AppConfig


class ComponentConfig(AppConfig):
    default_auto_field = 'django.db.models.BigAutoField'
    name = 'component'
    verbose_name = '组件'
