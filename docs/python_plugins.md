# KBE Python 插件说明

## 目标

Python 插件是 assets 资源读取方式的扩展，而不是一套独立的运行时框架。

引擎只在 `resmgr/plugins` 中维护一个很薄的插件索引：读取 `plugins.xml`、按声明顺序解析对应插件的 `plugin.json`、缓存启用插件和实体声明。具体读取仍由原来的模块负责：

- `EntityDef` 负责读取插件 `entity_defs/types.xml` 和实体 `.def`。
- `ScriptDefModule` 负责判断插件实体是否有 base/cell/client 脚本。
- `PythonApp`、`EntityApp`、`Bots` 负责安装自己的插件 Python path、导入自己的插件 entry、派发自己的生命周期。

这样可以保持 KBE 原有风格：谁读取 assets，谁顺手读取 plugins。

## 目录结构

插件固定放在 assets 脚本根目录的 `plugins/` 下：

```text
assets/
  plugins.xml
  plugins/
    Bag/
      plugin.json
      entity_defs/
        BagEntity.def
        types.xml
      base/
        BagEntity.py
        plugin_entry.py
      bots/
        plugin_entry.py
      common/
        bag_model.py
```

Nex assets 中脚本根通常就是 assets 根目录；老式 assets 如果脚本根是 `scripts/`，则目录为 `scripts/plugins/`。

## plugins.xml

插件必须先在 assets 脚本根目录的 `plugins.xml` 中声明，才会被引擎启用：

```xml
<root>
    <plugin>Bag</plugin>
</root>
```

规则说明：

- `plugins.xml` 不存在时，引擎不启用任何插件。
- `plugins.xml` 存在时，只加载文件中声明的插件。
- 插件加载顺序严格等于 XML 中的声明顺序。
- XML 中声明的插件必须存在 `plugins/<Plugin>/plugin.json`。
- `plugin.json` 中的 `name` 必须和 `plugins.xml` 声明名一致。
- `plugin.json` 里的 `enabled=false` 仍可作为二次开关，声明了但会跳过加载。

推荐写法是 `<plugin>Bag</plugin>`。为了贴近 `entities.xml` 的手感，也支持直接写：

```xml
<root>
    <Bag/>
</root>
```

## plugin.json

```json
{
  "name": "Bag",
  "prefix": "Bag",
  "version": "0.1.0",
  "enabled": true,
  "entities": [
    {
      "name": "BagEntity",
      "def": "entity_defs/BagEntity.def",
      "hasBase": true,
      "hasCell": false,
      "hasClient": false
    }
  ],
  "components": {
    "base": {
      "scriptPaths": ["base", "common"],
      "entry": "plugin_entry"
    },
    "bots": {
      "scriptPaths": ["bots", "common"],
      "entry": "plugin_entry"
    }
  }
}
```

字段说明：

- `name`：插件名，只允许字母、数字和下划线。
- `prefix`：插件命名前缀，必填，全局唯一，只允许字母、数字和下划线。
- `enabled`：为 `false` 时跳过插件。
- `entities`：声明插件实体，不写入根 `entities.xml`。
- `def`：相对插件根目录的 `.def` 路径，不允许绝对路径或 `..`。
- `hasBase/hasCell/hasClient`：实体组件声明，会参与 EntityDef MD5。
- `components.<component>.scriptPaths`：当前组件额外加入 Python path 的目录。
- `components.<component>.entry`：当前组件的生命周期入口。无路径分隔符时默认查找 `<component>/<entry>.py`。

命名前缀规则：

- 插件实体名必须匹配 `prefix`。
- 插件 `entity_defs/types.xml` 中定义的 Type alias 也必须匹配 `prefix`。
- 合法边界为：`prefix + "_"` 或 `prefix + 大写字母`，并且后缀不能为空。
- 例如 `prefix="Bag"` 时，`BagItem`、`Bag_Item` 合法，`Bag`、`Baggage`、`Inventory` 非法。
- 违反前缀规则会输出 `ERROR` 并终止启动。

支持的 component 名称：

```text
base, cell, db, interface, login, logger, bots, client
```

当前运行时覆盖：

- `baseapp`
- `cellapp`
- `dbmgr`
- `interfaces`
- `loginapp`
- `logger`
- `bots`

`kbcmd` 生成 SDK 会通过 `EntityDef` 读取插件实体和类型。`client_lib/clientapp/machine` 当前不作为插件运行时目标。

## 生命周期

插件 entry 可以选择实现：

```python
def onInit(isReload):
    pass

def onComponentReady(isFirstGroup):
    pass

def onFini():
    pass
```

没有对应函数时会跳过。

## 加载顺序

插件 schema 前置加载。引擎先读取插件清单：

```text
plugins.xml
plugins/<Plugin>/plugin.json
```

然后按 `plugins.xml` 顺序加载插件类型和实体，再加载 assets 自身类型和实体：

```text
plugins/<Plugin>/entity_defs/types.xml
entity_defs/types.xml
plugins/<Plugin>/entity_defs/*.def
entities.xml
entity_defs/*.def
```

插件 manifest 不再按目录扫描自动启用，而是按 `plugins.xml` 中的声明顺序加载。
这个顺序会影响插件实体 utype、DataTypes 合并顺序、EntityDef MD5、Python path 顺序和 SDK 输出，所以需要由 assets 明确控制。
启用插件后，assets 可以引用插件提供的 Type；如果 assets 实体与插件实体重名，启动会失败。

## 限制

- 第一版只支持 Python 插件，不支持 C++ 动态库插件。
- 插件不会修改根 `entities.xml`。
- 插件目录中的文件不会同步复制到 `base/`、`cell/` 等根目录。
- 新增或删除插件后需要重启进程；当前 reloadScript 不重新扫描插件列表。
- 插件名和插件实体名必须全局唯一。
- 插件 `prefix` 必填且全局唯一。
