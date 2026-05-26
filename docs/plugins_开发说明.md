# KBE 插件开发说明文档

> 版本: 2.8 | 最后更新: 2026-05-26

## 一、概述

KBE 插件系统允许你将游戏功能打包为独立模块，放置在 assets 的 `plugins/` 目录下，由引擎自动发现、加载和集成。插件可以包含实体定义、类型定义、Python 脚本和生命周期回调，不需要修改引擎核心代码或 assets 根目录的 `entities.xml`。

**设计原则：**

- 插件是 assets 资源读取方式的扩展，不是独立的运行时框架。
- 引擎只在 `resmgr/plugins` 中维护一个轻量插件索引：读取 `plugins.xml`、解析 `plugin.json`、缓存插件和实体的元数据。
- 具体读取仍由原来负责的模块完成：`EntityDef` 读 `.def` 和 `types.xml`，`ScriptDefModule` 判断实体脚本，各 App 自行安装 Python path 和派发生命周期。
- 这种设计保持了 KBE 原有的"谁读取 assets，谁顺手读取 plugins"的风格。

**适用场景：**

- 将通用功能（背包、任务、聊天等）封装为可复用插件
- 多人协作时各人负责独立插件模块，减少代码冲突
- 为不同项目提供可插拔的功能模块

---

后续章节接第二部分...

## 二、快速入门：创建一个背包插件

本节从零开始创建一个名为 `Bag` 的插件，包含一个只在 base 端运行的背包实体。

### 2.1 准备工作

确认你的 assets 目录。新版 Nex assets 的脚本根通常就是 assets 根目录；老式 assets 如果脚本根是 `scripts/`，则以下所有路径以 `scripts/` 为基准。本文以 Nex assets 为例，脚本根 = assets 根目录。

### 2.2 第一步：创建目录结构

在 assets 根目录下创建以下目录和文件：

```
assets/
  plugins/
    Bag/
      plugin.json              ← 插件清单
      entity_defs/
        BagEntity.def          ← 实体定义
        types.xml              ← 类型定义（可为空）
      base/
        BagEntity.py           ← 实体脚本
        plugin_entry.py        ← 生命周期入口
      common/
        bag_model.py           ← 公共模块
```

### 2.3 第二步：编写 plugin.json

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
    }
  }
}
```

### 2.4 第三步：编写实体定义文件

`plugins/Bag/entity_defs/BagEntity.def`：

```xml
<root>
    <Properties>
        <ownerDBID>
            <Type>          DBID                </Type>
            <Flags>         BASE                </Flags>
            <Persistent>    true                </Persistent>
        </ownerDBID>

        <items>
            <Type>          PYTHON              </Type>
            <Flags>         BASE                </Flags>
            <Persistent>    true                </Persistent>
        </items>
    </Properties>

    <BaseMethods>
        <addItem>
            <Arg>       UNICODE         </Arg>
            <Arg>       UINT32          </Arg>
        </addItem>

        <removeItem>
            <Arg>       UNICODE         </Arg>
            <Arg>       UINT32          </Arg>
        </removeItem>
    </BaseMethods>
</root>
```

### 2.5 第四步：编写实体脚本

`plugins/Bag/base/BagEntity.py`：

```python
# -*- coding: utf-8 -*-
import KBEngine
from KBEDebug import INFO_MSG


class BagEntity(KBEngine.Entity):
    def __init__(self):
        KBEngine.Entity.__init__(self)
        if self.items is None:
            self.items = {}
        INFO_MSG("BagEntity created: id=%s" % self.id)

    def addItem(self, itemKey, count):
        items = dict(self.items or {})
        items[itemKey] = items.get(itemKey, 0) + count
        self.items = items

    def removeItem(self, itemKey, count):
        items = dict(self.items or {})
        old = items.get(itemKey, 0)
        new = max(0, old - count)
        if new == 0:
            items.pop(itemKey, None)
        else:
            items[itemKey] = new
        self.items = items
```

### 2.6 第五步：编写生命周期入口

`plugins/Bag/base/plugin_entry.py`：

```python
# -*- coding: utf-8 -*-
from KBEDebug import INFO_MSG


def onInit(isReload):
    INFO_MSG("Bag plugin onInit: isReload=%s" % isReload)


def onComponentReady(isFirstGroup):
    INFO_MSG("Bag plugin onComponentReady: isFirstGroup=%s" % isFirstGroup)


def onFini():
    INFO_MSG("Bag plugin onFini")
```

### 2.7 第六步：注册插件

在 assets 根目录创建 `plugins.xml`：

```xml
<root>
    <plugin>Bag</plugin>
</root>
```

### 2.8 第七步：启动验证

启动服务器后，你会在日志中看到：

```
PluginManager::discover: plugins.xml declares 1 plugin(s)
PluginManager::discover: loaded plugin [Bag] prefix [Bag] version [0.1.0], entities=1, components=1
PluginManager::addPlugin: registered plugin [Bag] prefix [Bag], 1 entity(s), 1 component(s)
```

然后在 baseapp 日志中会看到生命周期回调：

```
Bag base plugin onInit: isReload=False
Bag base plugin onComponentReady: isFirstGroup=True
```

现在你可以通过 `KBEngine.createEntityLocally("BagEntity", {})` 在 base 端创建插件实体。

## 三、插件目录结构详解

一个完整的插件目录如下：

```
plugins/<PluginName>/
    plugin.json                          # 必填，插件清单
    entity_defs/
        <EntityName>.def                 # 实体定义文件
        types.xml                        # 类型别名定义（可为空）
    base/                                # base 端脚本（可选）
        <EntityName>.py                  # 实体脚本
        plugin_entry.py                  # 生命周期入口（可选）
    cell/                                # cell 端脚本（可选）
        <EntityName>.py
        plugin_entry.py
    client/                              # 客户端脚本（可选）
        <EntityName>.py
        plugin_entry.py
    db/                                  # dbmgr 端脚本（可选）
        plugin_entry.py
    login/                               # loginapp 端脚本（可选）
        plugin_entry.py
    logger/                              # logger 端脚本（可选）
        plugin_entry.py
    interface/                           # interfaces 端脚本（可选）
        plugin_entry.py
    bots/                                # bots 端脚本（可选）
        plugin_entry.py
    common/                              # 跨组件共享模块（可选）
        ...
```

规则：

- 除了 `plugin.json` 之外，所有目录和文件都是可选的。
- `common/` 目录对所有 component 可见。
- 如果某个 component 不需要，就不创建对应目录。

## 四、plugins.xml 配置

### 4.1 文件位置

`plugins.xml` 放在 assets 脚本根目录（与 `entities.xml` 同级）：

```
assets/
    plugins.xml          ← 这里
    entities.xml
```

### 4.2 基本格式

```xml
<root>
    <plugin>Bag</plugin>
    <plugin>Quest</plugin>
    <plugin>Chat</plugin>
</root>
```

### 4.3 规则

- `plugins.xml` 不存在时，引擎不启用任何插件，正常启动。
- `plugins.xml` 存在时，只加载文件中声明的插件。
- 加载顺序严格等于 XML 声明顺序，影响实体 utype、DataTypes 合并顺序、EntityDef MD5 和 Python path 顺序。
- 插件名只允许字母、数字和下划线。
- 重复声明会报错终止启动。
- `plugins.xml` 声明的插件名必须与 `plugins/<Name>/plugin.json` 中的 `name` 字段一致。

### 4.4 简写形式

也支持直接用节点名声明插件：

```xml
<root>
    <Bag/>
    <Quest/>
</root>
```

两种写法等价，推荐使用 `<plugin>Name</plugin>` 形式，语义更明确。

## 五、plugin.json 配置详解

### 5.1 完整示例

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

### 5.2 顶层字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|:---:|------|
| `name` | string | 是 | 插件名，必须与 `plugins.xml` 声明名一致。只允许字母、数字和下划线 |
| `prefix` | string | 是 | 插件命名前缀，全局唯一，只允许字母、数字和下划线。详见第六章 |
| `version` | string | 否 | 插件版本号，仅用于日志和标识 |
| `enabled` | bool | 否 | 二次开关。`false` 时即使 `plugins.xml` 声明了也跳过加载。默认 `true` |
| `entities` | array | 否 | 插件实体声明列表。不写入根 `entities.xml` |
| `components` | object | 否 | 各组件（base/cell/bots 等）的配置 |

### 5.3 entities 字段

每个实体是一个 JSON 对象：

| 字段 | 类型 | 必填 | 说明 |
|------|------|:---:|------|
| `name` | string | 是 | 实体名，必须满足 prefix 命名规则。全局唯一 |
| `def` | string | 是 | `.def` 文件相对插件根目录的路径。不允许绝对路径、`..` 或盘符 |
| `hasBase` | bool | 否 | 是否有 base 端脚本。参与 EntityDef MD5。默认 `false` |
| `hasCell` | bool | 否 | 是否有 cell 端脚本。参与 EntityDef MD5。默认 `false` |
| `hasClient` | bool | 否 | 是否有客户端脚本。参与 EntityDef MD5。默认 `false` |

### 5.4 components 字段

以 component 名称为 key，值为配置对象。支持的 component 名称：

```
base, cell, db, interface, login, logger, bots, client
```

每个 component 的配置：

| 字段 | 类型 | 必填 | 说明 |
|------|------|:---:|------|
| `scriptPaths` | array | 否 | 追加到该组件 Python `sys.path` 的目录。相对插件根目录 |
| `entry` | string | 否 | 生命周期入口名。无路径分隔符时默认查找 `<component>/<entry>.py` |

**scriptPaths 效果示例：** 配置 `"scriptPaths": ["base", "common"]` 后，baseapp 的 Python path 将包含 `assets/plugins/Bag/base` 和 `assets/plugins/Bag/common`，实体脚本可以直接 `from bag_model import normalize_item_key`。

**entry 效果示例：** 配置 `"entry": "plugin_entry"` 后，引擎在 baseapp 启动时从 `plugins/Bag/base/plugin_entry.py` 导入并调用生命周期函数。不需要 entry 时留空或不填。

## 六、命名前缀规则

### 6.1 为什么需要前缀

KBE 的插件实体名直接参与全局实体注册表。多个插件可能定义同名的实体（比如都叫 `Item`），没有前缀隔离会导致命名冲突。`prefix` 字段强制所有插件实体和类型遵循统一的前缀命名约定，从根本上避免冲突。

### 6.2 规则

- `prefix` 在 `plugin.json` 中**必填**。
- `prefix` 值**全局唯一**，两个插件不能使用相同的前缀。
- 插件的所有**实体名**必须以 `prefix` 开头。
- 插件的 `entity_defs/types.xml` 中定义的**类型别名**也必须以 `prefix` 开头。

### 6.3 合法边界

`prefix` 和后续名称之间的边界字符必须是下划线 `_` 或大写字母 A-Z：

| 实体名 | prefix="Bag" 时 | 原因 |
|--------|:---:|------|
| `BagEntity` | 合法 | 边界为大写 E |
| `Bag_Entity` | 合法 | 边界为下划线 |
| `BagItem` | 合法 | 边界为大写 I |
| `Bag` | **非法** | 后缀为空 |
| `bagEntity` | **非法** | 前缀匹配但边界为小写字母 |
| `Inventory` | **非法** | 不以 Bag 开头 |
| `Baggage` | **非法** | 边界为小写 g |

违反规则时引擎输出 `ERROR` 日志并拒绝启动：

```
PluginManager::validatePrefixedName: plugin entity name [Bag] must be plugin prefix [Bag]
plus a non-empty suffix, valid examples: [BagItem], [Bag_Item], invalid: [Bag]
```

如果边界字符合法（大写/下划线之外的可打印字符），引擎会打印其十六进制值帮助定位：

```
PluginManager::validatePrefixedName: plugin entity name [Bagitem] uses prefix [Bag]
but has invalid boundary char (0x69), valid examples: [BagItem], [Bag_Item]
```

## 七、实体定义文件 (.def)

### 7.1 文件位置

放在 `plugins/<PluginName>/entity_defs/` 目录下：

```
plugins/Bag/entity_defs/
    BagEntity.def
    types.xml
```

### 7.2 格式

与 KBE 标准 `.def` 文件完全相同：XML 格式，定义实体的属性和方法。详见 KBE 实体定义文档。

### 7.3 与主 entities.xml 的关系

- 插件实体的 `.def` **不写入** assets 根目录的 `entities.xml`。
- 引擎在 `EntityDef::initialize` 时依次加载插件实体和 assets 实体。
- 插件实体与 assets 实体共享全局命名空间，**重名会报错终止启动**。

## 八、类型定义文件 (types.xml)

### 8.1 文件位置

```
plugins/Bag/entity_defs/types.xml
```

### 8.2 格式

与 KBE 标准 `types.xml` 格式完全相同：

```xml
<root>
    <BagInventoryType>  PYTHON  </BagInventoryType>
    <BagItemDict>       PYTHON  </BagItemDict>
</root>
```

类型别名也必须遵循 prefix 命名规则。

### 8.3 空文件

如果插件不需要自定义类型，`types.xml` 可以只保留空的根节点：

```xml
<root>
</root>
```

文件必须存在，否则 `EntityDef` 会报错。

### 8.4 加载顺序

- 插件 `types.xml` 中的类型在 assets `types.xml` **之前**加载。
- assets 实体可以直接使用插件定义的类型。
- 类型别名全局唯一，重名会报错。

## 九、组件脚本

### 9.1 base/cell/client 端

脚本放在 `plugins/<PluginName>/<component>/` 下。文件名必须与实体名一致：

```
plugins/Bag/base/
    BagEntity.py       ← 实体脚本，文件名 = 实体名
    plugin_entry.py    ← 生命周期入口
```

实体脚本与 KBE 标准实体脚本写法相同，继承 `KBEngine.Entity`。

如果实体声明了 `"hasBase": true` 等，必须提供对应目录下的实体 `.py` 文件。

### 9.2 跨组件共享模块

放在 `plugins/<PluginName>/common/` 下。所有 component 的 `scriptPaths` 通常都应该包含 `"common"`：

```python
# base/BagEntity.py 和 bots/plugin_entry.py 都可以:
from bag_model import normalize_item_key
```

## 十、生命周期

### 10.1 entry 文件

在 `plugin.json` 中通过 `components.<component>.entry` 字段指定：

```json
"components": {
    "base": {
        "entry": "plugin_entry"
    }
}
```

引擎会从 `plugins/<PluginName>/base/plugin_entry.py` 导入并调用生命周期函数。

### 10.2 生命周期回调

entry 文件可以选择实现以下三个函数：

| 函数 | 说明 |
|------|------|
| `onInit(isReload)` | 组件初始化时调用。`isReload` 表示是否由 reloadScript 触发 |
| `onComponentReady(isFirstGroup)` | 本组件组的第一个/最后一个 app 就绪时调用 |
| `onFini()` | 组件关闭时调用 |

三个函数都是可选的，没有对应函数时引擎直接跳过，不会报错。

### 10.3 调用时机

```
1. 引擎初始化
2. 加载插件 schema（plugins.xml + plugin.json）
3. 安装插件 Python path
4. 初始化 Python 解释器
5. 导入插件 entry → 调用 onInit(isReload=False)
6. 组件就绪 → 调用 onComponentReady(isFirstGroup=True)
...
7. 组件关闭 → 调用 onFini()
```

### 10.4 不需要 entry 时

如果不填 `"entry"` 字段或留空，引擎只加载插件的实体和 Python path，不调用任何生命周期函数。适合纯实体/纯数据模块型的插件。

## 十一、加载顺序

### 11.1 总体顺序

```
1. 扫描 plugins.xml → 获取声明插件列表
2. 按声明顺序解析每个插件的 plugin.json
3. 按声明顺序加载插件类型定义:
   plugins/<Plugin>/entity_defs/types.xml
4. 加载 assets 自身类型定义:
   entity_defs/types.xml
5. 按声明顺序加载插件实体定义:
   plugins/<Plugin>/entity_defs/*.def
6. 加载 assets 自身实体定义:
   entities.xml → entity_defs/*.def
```

### 11.2 顺序的重要性

加载顺序影响：实体 utype 编号、DataTypes 合并、EntityDef MD5、Python path 查找优先级、SDK 输出顺序。

### 11.3 plugins.xml 声明顺序

```xml
<root>
    <plugin>Bag</plugin>      ← 第 1 个加载
    <plugin>Quest</plugin>    ← 第 2 个加载
    <plugin>Chat</plugin>     ← 第 3 个加载
</root>
```

调整声明顺序会影响上述所有行为，应谨慎安排。

## 十二、运行时覆盖范围

### 12.1 已覆盖的组件

| 组件 | 类型加载 | 实体加载 | Python path | 生命周期 |
|------|:---:|:---:|:---:|:---:|
| baseapp | 是 | 是 | 是 | 是 |
| cellapp | 是 | 是 | 是 | 是 |
| dbmgr | 是 | — | 是 | 是 |
| loginapp | 是 | — | 是 | 是 |
| logger | 是 | — | 是 | 是 |
| interfaces | 是 | — | 是 | 是 |
| bots | 是 | 是 | 是 | 是 |

### 12.2 kbcmd SDK 生成

`kbcmd` 在生成客户端 SDK 时会通过 `EntityDef` 读取插件实体和类型定义，启用的插件实体会出现在生成的 SDK 中。

### 12.3 未覆盖的组件

以下模块当前**不作为**插件的运行时目标：`client_lib`、`clientapp`、`machine`。

## 十三、常见问题

**Q: 插件实体能与 assets 实体交互吗？**

可以。插件实体和 assets 实体共享同一个实体注册表，可以互相调用方法、传递数据。唯一的限制是实体名不能冲突。

**Q: 插件能引用 assets 的类型吗？**

可以。插件类型在 assets 类型之前加载，但两者在同一 DataTypes 空间内。

**Q: assets 能引用插件类型吗？**

可以。插件类型先于 assets 类型加载到 DataTypes 中。

**Q: 插件不填 entry 能正常工作吗？**

可以。插件实体仍然会被加载和注册，可以正常创建和使用。

**Q: reloadScript 会重新加载插件吗？**

`reloadScript` 会重新导入插件 entry 并调用生命周期函数（`onInit(isReload=True)` 等），但**不会重新扫描插件列表**。新增或删除插件需要重启进程。

**Q: 插件可以只有 cell 端没有 base 端吗？**

可以。设置 `"hasBase": false, "hasCell": true` 即可。

**Q: 多个插件可以用同一个 prefix 吗？**

不可以。`prefix` 必须全局唯一，引擎会检测并拒绝启动。

**Q: 插件文件路径能用 `..` 吗？**

不可以。所有 `plugin.json` 中的路径字段都经过了路径穿越检测，`..`、绝对路径开头、盘符等都会被拒绝。

