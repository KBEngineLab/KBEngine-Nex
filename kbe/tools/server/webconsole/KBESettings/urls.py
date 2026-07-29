"""
URL configuration for KBESettings project.

The `urlpatterns` list routes URLs to views. For more information please see:
    https://docs.djangoproject.com/en/5.2/topics/http/urls/
Examples:
Function views
    1. Add an import:  from my_app import views
    2. Add a URL to urlpatterns:  path('', views.home, name='home')
Class-based views
    1. Add an import:  from other_app.views import Home
    2. Add a URL to urlpatterns:  path('', Home.as_view(), name='home')
Including another URLconf
    1. Import the include() function: from django.urls import include, path
    2. Add a URL to urlpatterns:  path('blog/', include('blog.urls'))
"""
from django.conf.urls.static import static
from django.urls import path, include, re_path

from KBESettings import settings
from KBESettings.custom_admin_site import  custom_admin_site

urlpatterns = [
    # path('admin/', admin.site.urls),
    path('admin/', custom_admin_site.urls),
    path('cluster/', include('cluster.urls' )),
    path('component/', include('component.urls' )),
] + static(settings.STATIC_URL, document_root=settings.STATIC_ROOT)
