# DetailLevels 属性分级同步与使用教程

**日期**：2026-08-11

**适用范围**：CellApp `ALL_CLIENTS`、`OTHER_CLIENTS` 类属性广播

**不适用范围**：位置/方向 Volatile 更新、客户端方法调用、OWN_CLIENT 私有属性

## 1. 迁移结论

旧 1.x/Nex 的 `DetailLevel` 只有属性变化当下的距离过滤，没有保存每条 AOI 观察关系的当前等级。观察者在远处错过属性更新后，即使走近也不会自动获得最新值。

当前实现补齐以下闭环：

1. 每条 `EntityRef` 关系保存 `NEAR`、`MEDIUM`、`FAR` 或 `NONE` 当前等级。
2. 观察者移动和目标实体移动都会重新计算等级。
3. 向更近等级切换时，补发新可见等级内的当前属性。
4. 向更远等级切换时不发送清理消息，只停止后续受限属性更新。
5. `hyst` 用作离开当前等级的滞回距离，避免边界抖动反复补发。
6. 距离使用 XZ 平方距离，不执行平方根，也不受地形高度差影响。

首次进入 AOI 仍发送全部 OTHER_CLIENT 初始属性，以保持现有客户端 Entity 创建协议和默认值行为。`DetailLevels` 控制的是进入 AOI 后的增量更新；后续靠近时会补齐此前被过滤的最新值。

## 2. 完整配置示例

推荐把 `<DetailLevels>` 写在具体宿主实体 `.def` 的根节点下，与 `<Properties>`、`<Interfaces>` 同级：

```xml
<root>
	<DetailLevels>
		<NEAR>
			<radius>15</radius>
			<hyst>2</hyst>
		</NEAR>
		<MEDIUM>
			<radius>50</radius>
			<hyst>3</hyst>
		</MEDIUM>
		<FAR>
			<radius>100</radius>
			<hyst>5</hyst>
		</FAR>
	</DetailLevels>

	<Properties>
		<combatPose>
			<Type>UINT8</Type>
			<Flags>ALL_CLIENTS</Flags>
			<DetailLevel>NEAR</DetailLevel>
		</combatPose>

		<name>
			<Type>UNICODE</Type>
			<Flags>ALL_CLIENTS</Flags>
			<DetailLevel>MEDIUM</DetailLevel>
		</name>

		<guildID>
			<Type>UINT32</Type>
			<Flags>ALL_CLIENTS</Flags>
			<DetailLevel>FAR</DetailLevel>
		</guildID>
	</Properties>
</root>
```

`<DetailLevel>` 省略时默认为 `FAR`。它只限制属性发送给其他客户端的部分；`ALL_CLIENTS` 属性仍会无距离限制地同步给实体自己的客户端。

## 3. radius 和 hyst 语义

三个 `radius` 都是以实体为中心的 XZ 绝对距离：

```text
nearMax   = NEAR.radius
mediumMax = MEDIUM.radius
farMax    = FAR.radius
```

配置必须满足：

```text
0 <= NEAR.radius <= MEDIUM.radius <= FAR.radius
```

上面的示例行为如下：

| 等级 | 进入最大距离 | 向外离开距离 |
| --- | ---: | ---: |
| NEAR | 15 | 17 |
| MEDIUM | 50 | 53 |
| FAR | 100 | 105 |
| NONE | 大于 FAR 离开距离 | 重新进入 FAR.radius 后恢复 |

`hyst` 不参与其他等级半径计算，只用于实体向外移动时延迟离开当前等级。例如 `NEAR.radius=15`、`NEAR.hyst=2` 表示进入 NEAR 的边界是 15，已经处于 NEAR 时移动到 17 以内仍保持 NEAR。

`radius` 和 `hyst` 必须是有限的非负数；只要存在 `<DetailLevels>`，就必须完整提供 `NEAR`、`MEDIUM`、`FAR` 以及每级的 `radius`、`hyst`。残缺配置或半径顺序错误会让 EntityDef 加载失败，而不是带着半配置继续启动。

## 4. 属性可见规则

关系等级越小代表距离越近：

| 当前关系等级 | 可接收属性 |
| --- | --- |
| NEAR | NEAR、MEDIUM、FAR |
| MEDIUM | MEDIUM、FAR |
| FAR | FAR |
| NONE | 不接收受 DetailLevel 控制的增量属性 |

例如观察者在 FAR 时，目标的 `combatPose` 和 `name` 发生变化不会发送；观察者进入 MEDIUM 后补发最新 `name`，进入 NEAR 后再补发最新 `combatPose`。

补发的是当前最终值，不重放远处期间的每次中间变化，因此不会造成无意义的历史消息堆积。

## 5. Interface 和 Component 使用方式

`<DetailLevel>` 可以声明在 Interface 属性中，最终会合并到宿主实体属性表。距离阈值推荐放在具体宿主实体 `.def`，避免多个 Interface 都定义 `<DetailLevels>` 时产生加载顺序覆盖。

组件子属性可以直接声明等级：

```xml
<Properties>
	<castProgress>
		<Type>FLOAT</Type>
		<Flags>ALL_CLIENTS</Flags>
		<DetailLevel>NEAR</DetailLevel>
	</castProgress>
</Properties>
```

组件子属性的等级标签来自组件 `.def`，但距离阈值使用宿主实体的 `<DetailLevels>`。靠近补发时按“组件父属性 UID + 子属性 UID”发送单个子属性，不会重发整个组件，也不会把 `OWN_CLIENT` 子属性带给其他客户端。

## 6. 与 WitnessVolatileLod 的区别

两套机制互不替代：

| 机制 | 控制对象 | 行为 |
| --- | --- | --- |
| `DetailLevels` | EntityDef 属性 | 按距离过滤 OTHER_CLIENT 属性变化，靠近时补发最终值 |
| `witness_volatile_lod` | 位置和方向 | 高密度同屏时降低中远距离刷新频率，不丢失最终位姿 |

不要用 `DetailLevels` 控制位置更新，也不要期待 `witness_volatile_lod` 自动过滤普通属性。

## 7. 性能建议

1. `NEAR` 适合战斗姿态、短距离交互状态等只有近处需要的属性。
2. `MEDIUM` 适合名称、外观摘要、队伍标记等中距离需要的属性。
3. `FAR` 适合远处仍必须保持正确的阵营、可见性或基础状态。
4. 高频大对象不要仅依赖 DetailLevel；优先拆成较小、职责明确的属性。
5. 滞回值应覆盖正常移动和网络修正造成的小幅边界抖动，但不要大到明显扩大属性广播范围。
6. 未配置 `<DetailLevels>` 时三个等级半径保持 `FLT_MAX`，所有属性继续按旧行为发送，便于渐进迁移。

每条 AOI 关系只增加一个固定大小等级字段，不维护每关系动态属性日志。观察者同一 Tick 多次移动只登记一次等级扫描；目标移动复用现有 Witness dirty 队列。CPU 开销主要是 XZ 平方距离比较，内存不会随漏发属性次数增长。

## 8. 验收步骤

1. 为测试实体配置 15、50、100 三个最终边界。
2. 在 FAR 或 NONE 距离修改 NEAR、MEDIUM、FAR 三种属性。
3. 确认远处客户端只立即收到当前等级允许的属性。
4. 依次进入 FAR、MEDIUM、NEAR，确认每次只补齐新可见等级的最新值。
5. 在 15、50、100 边界附近往返，确认未越过对应 `radius+hyst` 时不会反复切级。
6. 验证组件子属性使用父 UID + 子 UID 正常更新。
7. 验证自己的客户端始终收到 `ALL_CLIENTS` 属性，不受距离等级影响。
8. 删除 `<DetailLevels>` 后回归旧行为，确认所有等级属性均正常广播。

Release 构建和便携状态机测试命令：

```powershell
cmake --build kbe/src/out/build/windows-ninja-multi --config Release --target cellapp kbe_test_witness_volatile_budget -j 8
kbe/src/out/cmake/windows-ninja/bin/Release/kbe_test_witness_volatile_budget.exe
```
