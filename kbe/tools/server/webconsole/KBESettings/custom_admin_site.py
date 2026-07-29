from django.contrib.admin import AdminSite

class CustomAdminSite(AdminSite):
    site_header = "KBEngine Nex 管理后台"
    site_title = "KBEngine Nex Web Console"
    index_title = "控制面板"


    def each_context(self, request):
        context = super().each_context(request)
        # 移除 “查看站点” 按钮
        context["site_url"] = None
        return context

    def get_app_list(self, request, app_label = None):
        """
        返回整个站点的 app 列表（用于 admin index 页面）。
        我们在这里修正每个 app 的显示名并按自定义顺序排序。
        """
        # 使用 AdminSite 内部方法构建 app dict（keyed by app_label）
        app_dict = self._build_app_dict(request,app_label)
        app_list = list(app_dict.values())

        # 按自定义顺序排序 app
        desired_order = ['auth', 'cluster', 'webconsole', 'component']
        app_list.sort(key=lambda x: desired_order.index(x['app_label']) if x['app_label'] in desired_order else 100)
        return app_list


# 实例化供 urls.py 使用
custom_admin_site = CustomAdminSite(name="custom_admin")