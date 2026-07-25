/*
This source file is part of KBEngine
*/

#include "baseapp.h"
#include "space.h"

namespace KBEngine
{

SCRIPT_METHOD_DECLARE_BEGIN(Space)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(Space)
SCRIPT_MEMBER_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(Space)
SCRIPT_GETSET_DECLARE("createToCellappIndex", pyGetCreateToCellappIndex, pySetCreateToCellappIndex, 0, 0)
SCRIPT_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(Space, 0, 0, 0, 0, 0)

class CreateSpaceTimerHandler : public TimerHandler
{
public:
	explicit CreateSpaceTimerHandler(Space* entity) : pEntity_(entity)
	{
	}

private:
	void handleTimeout(TimerHandle handle, void*) override
	{
		ScriptTimers* scriptTimers = &pEntity_->scriptTimers();
		const int timerID = ScriptTimersUtil::getIDForHandle(scriptTimers, handle);
		KBE_ASSERT(timerID > 0);

		PyObject* pyIndex = PyObject_GetAttrString(pEntity_, "createToCellappIndex");
		PyObject* pyResult = pEntity_->createCellEntityInNewSpace(pyIndex);
		Py_XDECREF(pyResult);
		Py_XDECREF(pyIndex);
	}

	void onRelease(TimerHandle handle, void*) override
	{
		pEntity_->scriptTimers().releaseTimer(handle);
		delete this;
	}

	Space* pEntity_;
};

Space::Space(ENTITY_ID id, const ScriptDefModule* pScriptModule) :
	Entity(id, pScriptModule, getScriptType(), true),
	createToCellappIndex_(0)
{
	// 延迟到脚本初始化完成后创建 Cell 空间，确保脚本已写入完整的 cellData。
	// Delay Cell space creation until script initialization has populated the complete cellData dictionary.
	CreateSpaceTimerHandler* handler = new CreateSpaceTimerHandler(this);
	ScriptTimers* timers = &scriptTimers_;
	const int timerID = ScriptTimersUtil::addTimer(&timers, 0.1f, 0.f, 0, handler);
	KBE_ASSERT(timerID > 0);
}

Space::~Space()
{
}

PyObject* Space::pyGetCreateToCellappIndex()
{
	return PyLong_FromUnsignedLong(createToCellappIndex_);
}

int Space::pySetCreateToCellappIndex(PyObject* value)
{
	createToCellappIndex_ = PyLong_AsUnsignedLong(value);
	return 0;
}

}
