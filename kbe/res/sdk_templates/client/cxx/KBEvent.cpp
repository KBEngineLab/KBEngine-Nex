#include "KBEvent.h"
#include "KBDebug.h"

// out = KBE 插件层 -> 表现层/业务层。
// events_out_ 保存 out 方向的事件注册表：
// key 是事件名，value 是监听这个事件的所有回调。
KBMap<KBString, KBArray<KBEvent::EventObj>> KBEvent::events_out_;

// firedEvents_out_ 保存已经触发、但还没有进入本轮执行的 out 事件。
// fireOut 在暂停时，或者 outEventsImmediately 为 false 时，会把事件放到这里。
KBArray<KBEvent::FiredEvent*> KBEvent::firedEvents_out_;

// doingEvents_out_ 保存本轮正在执行的 out 事件快照。
// processOutEvents 会先把 firedEvents_out_ 搬到这里，再逐个执行。
// 这样回调执行过程中再次 fireOut 的事件会留到下一轮，避免重入顺序混乱。
KBArray<KBEvent::FiredEvent*> KBEvent::doingEvents_out_;

// in = 表现层/业务层 -> KBE 插件层。
// events_in_ 保存 in 方向的事件注册表。
KBMap<KBString, KBArray<KBEvent::EventObj>> KBEvent::events_in_;

// firedEvents_in_ 保存已经触发、但还没有进入本轮执行的 in 事件。
// fireIn 永远只入队，不在触发点直接执行。
KBArray<KBEvent::FiredEvent*> KBEvent::firedEvents_in_;

// doingEvents_in_ 保存本轮正在执行的 in 事件快照。
// processInEvents 不受 pause 控制。
KBArray<KBEvent::FiredEvent*> KBEvent::doingEvents_in_;

// out 事件是否尽量立即执行。
// true：fireOut 在未 pause 时直接调用监听者。
// false：fireOut 总是入队，等待 processOutEvents。
bool KBEvent::outEventsImmediately = true;

// pause 只控制 out 事件。
// 它会影响 fireOut 的即时执行和 processOutEvents 的队列消费。
// fireIn/processInEvents 不受该标记影响。
bool KBEvent::isPauseOut_ = false;

KBEvent::KBEvent()
{
}

KBEvent::~KBEvent()
{
}

void KBEvent::clear()
{
	// clear 会同时清空注册表和所有待执行事件，通常用于 SDK/应用重置。
	events_out_.Clear();
	events_in_.Clear();
	clearFiredEvents();
}

void KBEvent::clearFiredEvents()
{
	// 只清队列，不清注册表。
	// doing 队列也必须清理，否则正在延迟执行的事件仍可能回调到已经失效的对象。
	clearFiredEvents_(firedEvents_out_);
	clearFiredEvents_(doingEvents_out_);
	clearFiredEvents_(firedEvents_in_);
	clearFiredEvents_(doingEvents_in_);

	// 与 C#/TS 模板保持一致：清理队列时解除暂停状态。
	isPauseOut_ = false;
}

void KBEvent::clearFiredEvents_(KBArray<FiredEvent*>& firedEvents)
{
	// FiredEvent 是手动 new 出来的，清队列时必须逐个释放。
	// args 是 shared_ptr，置空后引用计数自然释放。
	while (firedEvents.Num() > 0)
	{
		FiredEvent* event = firedEvents.Pop();
		event->args = nullptr;
		delete event;
	}
}

bool KBEvent::registerEvent(const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr)
{
	// 旧接口保留为 out 注册，兼容已有 KBENGINE_REGISTER_EVENT 宏和用户代码。
	return registerOut(eventName, funcName, func, objPtr);
}

bool KBEvent::registerOut(const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr)
{
	// 注册 KBE -> 表现层方向的监听。
	return register_(events_out_, eventName, funcName, func, objPtr);
}

bool KBEvent::registerIn(const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr)
{
	// 注册 表现层 -> KBE 方向的监听。
	return register_(events_in_, eventName, funcName, func, objPtr);
}

bool KBEvent::register_(KBMap<KBString, KBArray<EventObj>>& events, const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr)
{
	// 注册前先移除同对象同函数的旧注册，避免重复监听导致一次 fire 调用多次。
	deregister_(events, objPtr, eventName, funcName);

	KBArray<EventObj>* eo_array = nullptr;
	KBArray<EventObj>* eo_array_find = events.Find(eventName);

	if (!eo_array_find)
	{
		events.Add(eventName, KBArray<EventObj>());
		eo_array = &(*events.Find(eventName));
	}
	else
	{
		eo_array = &(*eo_array_find);
	}

	EventObj eo;
	eo.funcName = funcName;
	eo.method = func;
	eo.objPtr = objPtr;
	eo_array->Add(eo);
	return true;
}

bool KBEvent::deregister(void* objPtr, const KBString& eventName, const KBString& funcName)
{
	// 旧接口为了兼容历史用法，同时尝试从 out 和 in 中注销。
	// 显式区分方向时应使用 deregisterOut / deregisterIn。
	bool retOut = deregisterOut(objPtr, eventName, funcName);
	bool retIn = deregisterIn(objPtr, eventName, funcName);
	return retOut || retIn;
}

bool KBEvent::deregisterOut(void* objPtr, const KBString& eventName, const KBString& funcName)
{
	// 注销注册表前先清理 out 队列中已经排队的同一回调。
	// 这样对象销毁或取消监听后，不会再收到延迟事件。
	removeFiredEventOut(objPtr, eventName, funcName);
	return deregister_(events_out_, objPtr, eventName, funcName);
}

bool KBEvent::deregisterIn(void* objPtr, const KBString& eventName, const KBString& funcName)
{
	// 注销注册表前先清理 in 队列中已经排队的同一回调。
	removeFiredEventIn(objPtr, eventName, funcName);
	return deregister_(events_in_, objPtr, eventName, funcName);
}

bool KBEvent::deregister_(KBMap<KBString, KBArray<EventObj>>& events, void* objPtr, const KBString& eventName, const KBString& funcName)
{
	KBArray<EventObj>* eo_array_find = events.Find(eventName);
	if (!eo_array_find || (*eo_array_find).Num() == 0)
	{
		return false;
	}

	bool removed = false;
	// 从后向前遍历，避免 RemoveAt 后数组下标变化导致漏删。
	for (size_t i = eo_array_find->Num(); i-- > 0; )
	{
		EventObj& item = (*eo_array_find)[i];
		if (objPtr == item.objPtr && (funcName.empty() || funcName == item.funcName))
		{
			eo_array_find->RemoveAt(i, 1);
			removed = true;
		}
	}

	return removed;
}

bool KBEvent::deregister(void* objPtr)
{
	// 旧接口：注销该对象在 out/in 两个方向的全部监听。
	bool retOut = deregisterOut(objPtr);
	bool retIn = deregisterIn(objPtr);
	return retOut || retIn;
}

bool KBEvent::deregisterOut(void* objPtr)
{
	// 清理该对象所有 out 待执行事件，并移除所有 out 注册。
	removeFiredEventOut(objPtr);
	return deregister_(events_out_, objPtr);
}

bool KBEvent::deregisterIn(void* objPtr)
{
	// 清理该对象所有 in 待执行事件，并移除所有 in 注册。
	removeFiredEventIn(objPtr);
	return deregister_(events_in_, objPtr);
}

bool KBEvent::deregister_(KBMap<KBString, KBArray<EventObj>>& events, void* objPtr)
{
	bool removed = false;
	for (auto& item : events)
	{
		KBArray<EventObj>& eo_array = item.second;

		// 每个事件列表都从后向前删，避免漏删同一对象注册的多个回调。
		for (size_t i = eo_array.Num(); i-- > 0; )
		{
			if (objPtr == eo_array[i].objPtr)
			{
				eo_array.RemoveAt(i, 1);
				removed = true;
			}
		}
	}

	return removed;
}

void KBEvent::fire(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData)
{
	// 旧接口保留为 out 触发，兼容已有 KBENGINE_EVENT_FIRE 宏和用户代码。
	fireOut(eventName, pEventData);
}

void KBEvent::fireOut(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData)
{
	// out 事件在允许即时执行且未 pause 时会直接回调，否则进入 out 队列。
	fire_(events_out_, firedEvents_out_, eventName, pEventData, outEventsImmediately);
}

void KBEvent::fireIn(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData)
{
	// in 事件始终进入队列，等待 processInEvents。
	// pause 不影响 fireIn 和 processInEvents。
	fire_(events_in_, firedEvents_in_, eventName, pEventData, false);
}

void KBEvent::fireAll(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData)
{
	// fireAll 同时向 in/out 两个方向派发，并且都采用入队方式。
	// 这样可以统一由 processInEvents / processOutEvents 控制执行时机。
	fire_(events_in_, firedEvents_in_, eventName, pEventData, false);
	fire_(events_out_, firedEvents_out_, eventName, pEventData, false);
}

void KBEvent::fire_(KBMap<KBString, KBArray<EventObj>>& events, KBArray<FiredEvent*>& firedEvents, const KBString& eventName, std::shared_ptr<UKBEventData> pEventData, bool eventsImmediately)
{
	// 允许调用方传空事件数据，内部补一个基础数据对象。
	if (!pEventData)
	{
		pEventData = std::make_shared<UKBEventData>();
	}

	KBArray<EventObj>* eo_array_find = events.Find(eventName);
	if (!eo_array_find || (*eo_array_find).Num() == 0)
	{
		//SCREEN_WARNING_MSG("KBEvent::fire_(): event(%s) not found!", *eventName);
		return;
	}

	pEventData->eventName = eventName;

	if (eventsImmediately && !isPauseOut_)
	{
		// 即时模式：直接调用当前注册表中的监听者。
		// 注意：这里和原 C++ 版本保持一致，直接遍历当前注册列表。
		for (auto& item : (*eo_array_find))
		{
			item.method(pEventData);
		}
	}
	else
	{
		// 延迟模式：按当前监听者生成快照入队。
		// 入队保存 EventObj，而不是只保存事件名，是为了固定本次 fire 时的监听者集合。
		for (auto& item : (*eo_array_find))
		{
			FiredEvent* event = new FiredEvent;
			event->evt = item;
			event->eventName = eventName;
			event->args = pEventData;
			firedEvents.Emplace(event);
		}
	}
}

void KBEvent::processOutEvents()
{
	// out 队列受 pause 控制。
	processFiredEvents_(firedEvents_out_, doingEvents_out_, true);
}

void KBEvent::processInEvents()
{
	// in 队列不受 pause 控制。
	processFiredEvents_(firedEvents_in_, doingEvents_in_, false);
}

void KBEvent::processFiredEvents_(KBArray<FiredEvent*>& firedEvents, KBArray<FiredEvent*>& doingEvents, bool checkPause)
{
	// 先把 fired 队列搬到 doing 队列。
	// 这样本轮处理期间新触发的事件会继续留在 fired 队列，下一轮再处理。
	for (auto event : firedEvents)
	{
		doingEvents.Emplace(event);
	}
	firedEvents.Clear();

	// checkPause 为 true 时，队列消费会被 pause 挡住。
	// out 调用传 true，in 调用传 false。
	while (doingEvents.Num() > 0 && (!checkPause || !isPauseOut_))
	{
		FiredEvent* event = doingEvents[0];
		doingEvents.RemoveAt(0, 1);

		// 执行并释放 FiredEvent。
		// args 是 shared_ptr，多个监听者可能共享同一个事件数据对象。
		event->evt.method(event->args);
		event->args = nullptr;
		delete event;
	}
}

void KBEvent::pause()
{
	// 只暂停 out 事件。
	isPauseOut_ = true;
}

void KBEvent::resume()
{
	// 恢复后只主动处理 out 队列；in 队列由调用方显式 processInEvents。
	isPauseOut_ = false;
	processOutEvents();
}

bool KBEvent::isPause()
{
	return isPauseOut_;
}

void KBEvent::removeFiredEvent(void* objPtr, const KBString& eventName /*= KBTEXT("")*/, const KBString& funcName /*= KBTEXT("")*/)
{
	// 旧接口默认只清理 out 队列，保持与旧 fire/registerEvent 的 out 兼容语义一致。
	removeFiredEventOut(objPtr, eventName, funcName);
}

void KBEvent::removeFiredEventOut(void* objPtr, const KBString& eventName /*= KBTEXT("")*/, const KBString& funcName /*= KBTEXT("")*/)
{
	// fired 和 doing 都要清理，防止对象注销后仍被本轮延迟回调。
	removeFiredEvent_(firedEvents_out_, objPtr, eventName, funcName);
	removeFiredEvent_(doingEvents_out_, objPtr, eventName, funcName);
}

void KBEvent::removeFiredEventIn(void* objPtr, const KBString& eventName /*= KBTEXT("")*/, const KBString& funcName /*= KBTEXT("")*/)
{
	// fired 和 doing 都要清理，防止对象注销后仍被本轮延迟回调。
	removeFiredEvent_(firedEvents_in_, objPtr, eventName, funcName);
	removeFiredEvent_(doingEvents_in_, objPtr, eventName, funcName);
}

void KBEvent::removeFiredEvent_(KBArray<FiredEvent*>& firedEvents, void* objPtr, const KBString& eventName /*= KBTEXT("")*/, const KBString& funcName /*= KBTEXT("")*/)
{
	// 从后向前删除，避免 RemoveAt 后下标变化。
	for (size_t i = firedEvents.Num(); i-- > 0; )
	{
		FiredEvent* item = firedEvents[i];

		// eventName 和 funcName 都为空时，表示删除该对象的全部待执行事件。
		// 否则只删除事件名匹配、并且函数名为空或匹配的待执行事件。
		bool matchedEvent = (eventName.length() == 0 && funcName.length() == 0) || (item->eventName == eventName && (funcName.length() == 0 || item->evt.funcName == funcName));
		if (matchedEvent && item->evt.objPtr == objPtr)
		{
			firedEvents.RemoveAt(i, 1);
			item->args = nullptr;
			delete item;
		}
	}
}
