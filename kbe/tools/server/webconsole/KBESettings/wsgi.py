"""
WSGI config for KBESettings project.

It exposes the WSGI callable as a module-level variable named ``application``.

For more information on this file, see
https://docs.djangoproject.com/en/5.2/howto/deployment/wsgi/
"""

import os

from django.core.wsgi import get_wsgi_application


os.environ.setdefault('DJANGO_SETTINGS_MODULE', 'KBESettings.settings')

application = get_wsgi_application()


# application = ProtocolTypeRouter({
#     "http": get_wsgi_application(),  # 继续支持 HTTP
#     # "websocket": AuthMiddlewareStack(
#     #     URLRouter(
#     #         websocket_urlpatterns # 👈 引入 websocket 路由
#     #     )
#     # ),
# })