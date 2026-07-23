/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef KBE_SCRIPTOBJECT_H
#define KBE_SCRIPTOBJECT_H

#include <vector>	
#include "Python.h"     
#include "py_macros.h" 
#include "helper/debug_helper.h"
#include "common/common.h"
#include <structmember.h>
#include <assert.h>

namespace KBEngine{ namespace script{

// python��Ĭ�Ͽշ���ֵ
#define S_Return { Py_INCREF(Py_None); return Py_None; }			
#define S_RETURN S_Return	
#define PY_RETURN S_Return	

#define S_INCREF(pyObj)																		\
	if(pyObj){																				\
		Py_INCREF(pyObj);																	\
	}																						\

#define S_DECREF(pyObj)																		\
	if(pyObj){																				\
		Py_DECREF(pyObj);																	\
	}																						\

// python�Ķ����ͷ�
#define S_RELEASE(pyObj)																	\
	if(pyObj){																				\
		Py_DECREF(pyObj);																	\
		pyObj = NULL;																		\
	}																						\

/// �����ǰ�ű������Ĵ�����Ϣ	
#define SCRIPT_ERROR_CHECK()																\
{																							\
 	if (PyErr_Occurred())																	\
 	{																						\
		PyErr_PrintEx(0);																	\
	}																						\
}

// �ű�����ͷ ��ͨ����pythonĬ�Ϸ������ʽ�����Ķ��� ��
#define SCRIPT_OBJECT_HREADER(CLASS, SUPERCLASS)											\
	SCRIPT_HREADER_BASE(CLASS, SUPERCLASS);													\
	/** python�����Ķ���������python���ͷ�
	*/																						\
	static void _tp_dealloc(PyObject* self)													\
	{																						\
		CLASS::_scriptType.tp_free(self);													\
	}																						\

// �����ű�����ͷ �����ģ��ͨ�����ṩ��python�ű��н��м̳е�һ�������� ��
#define BASE_SCRIPT_HREADER(CLASS, SUPERCLASS)												\
	SCRIPT_HREADER_BASE(CLASS, SUPERCLASS);													\
	/** python�����Ķ���������python���ͷ�
	*/																						\
	static void _tp_dealloc(PyObject* self)													\
	{																						\
		static_cast<CLASS*>(self)->~CLASS();												\
		CLASS::_scriptType.tp_free(self);													\
	}																						\

// ʵ���ű�����ͷ ������ű���������c++�н���new������ ��
#define INSTANCE_SCRIPT_HREADER(CLASS, SUPERCLASS)											\
	SCRIPT_HREADER_BASE(CLASS, SUPERCLASS);													\
	/** c++new�����Ķ��������delete����
	*/																						\
	static void _tp_dealloc(PyObject* self)													\
	{																						\
		delete static_cast<CLASS*>(self);													\
	}																						\

																							
#define SCRIPT_HREADER_BASE(CLASS, SUPERCLASS)												\
	/* ��ǰ�ű�ģ������ */																	\
	static PyTypeObject _scriptType;														\
	typedef CLASS ThisClass;																\
																							\
	static PyObject* _tp_repr(PyObject* self)												\
	{																						\
		return static_cast<CLASS*>(self)->tp_repr();										\
	}																						\
																							\
	static PyObject* _tp_str(PyObject* self)												\
	{																						\
		return static_cast<CLASS*>(self)->tp_str();											\
	}																						\
																							\
	/** �ű�ģ������python�д���
	*/																						\
	static PyObject* _tp_new(PyTypeObject* type, PyObject* args, PyObject* kwds)			\
	{																						\
		return CLASS::tp_new(type, args, kwds);												\
	}																						\
																							\
	/** python �����ȡ��ģ������Ի��߷���
	*/																						\
	static PyObject* _tp_getattro(PyObject* self, PyObject* name)							\
	{																						\
		return static_cast<CLASS*>(self)->onScriptGetAttribute(name);						\
	}																						\
																							\
	/** python �������ñ�ģ������Ի��߷���
	*/																						\
	static int _tp_setattro(PyObject* self, PyObject* name, PyObject* value)				\
	{																						\
		return (value != NULL) ?															\
				static_cast<CLASS*>(self)->onScriptSetAttribute(name, value):				\
				static_cast<CLASS*>(self)->onScriptDelAttribute(name);						\
	}																						\
																							\
	/** python �����ʼ����ģ�����
	*/																						\
	static int _tp_init(PyObject* self, PyObject *args, PyObject* kwds)						\
	{																						\
		return static_cast<CLASS*>(self)->onScriptInit(self, args, kwds);					\
	}																						\
																							\
public:																						\
	/* ���ս�Ҫ����װ���ű�ģ���еķ����ͳ�Ա����б�*/											\
	static PyMethodDef* _##CLASS##_lpScriptmethods;											\
	static PyMemberDef* _##CLASS##_lpScriptmembers;											\
	static PyGetSetDef* _##CLASS##_lpgetseters;												\
	/* ��ģ����Ҫ��©���ű��ķ����ͳ�Ա�� ���ջᱻ���뵽�����2��ָ���б��� */					\
	static PyMethodDef _##CLASS##_scriptMethods[];											\
	static PyMemberDef _##CLASS##_scriptMembers[];											\
	static PyGetSetDef _##CLASS##_scriptGetSeters[];										\
																							\
	static bool _##CLASS##_py_installed;													\
																							\
	/** getset��ֻ������
	*/																						\
	static int __py_readonly_descr(PyObject* self, PyObject* value, void* closure)			\
	{																						\
		PyErr_Format(PyExc_TypeError,														\
		"Sorry, this attribute %s.%s is read-only", (self != NULL ? self->ob_type->tp_name\
		: #CLASS), (closure != NULL ? (char*)closure : "unknown"));							\
		PyErr_PrintEx(0);																	\
		return 0;																			\
	}																						\
																							\
	/** getset��ֻд����
	*/																						\
	static int __py_writeonly_descr(PyObject* self, PyObject* value, void* closure)			\
	{																						\
		PyErr_Format(PyExc_TypeError,														\
		"Sorry, this attribute %s.%s is write-only", (self != NULL ? self->ob_type->tp_name\
		: #CLASS), (closure != NULL ? (char*)(closure) : "unknown"));						\
		PyErr_PrintEx(0);																	\
		return 0;																			\
	}																						\
																							\
	/** ����ӿڿ��Ի�õ�ǰģ��Ľű���� 
	*/																						\
	static PyTypeObject* getScriptType(void)												\
	{																						\
		return &_scriptType;																\
	}																						\
	static PyTypeObject* getBaseScriptType(void)											\
	{																						\
		if(strcmp("ScriptObject", #SUPERCLASS) == 0)										\
			return 0;																		\
		return SUPERCLASS::getScriptType();													\
	}																						\
																							\
	static long calcDictOffset(void)														\
	{																						\
		if(strcmp("ScriptObject", #SUPERCLASS) == 0)										\
			return 0;																		\
		return 0;																			\
	}																						\
																							\
	/** �������м̳�ģ��ı�¶�������� 
	*/																						\
	static int calcTotalMethodCount(void)													\
	{																						\
		int nlen = 0;																		\
		while(true)																			\
		{																					\
			PyMethodDef* pmf = &_##CLASS##_scriptMethods[nlen];								\
			if(!pmf->ml_doc && !pmf->ml_flags && !pmf->ml_meth && !pmf->ml_name)			\
				break;																		\
			nlen++;																			\
		}																					\
																							\
		if(strcmp(#CLASS, #SUPERCLASS) == 0)												\
			return nlen;																	\
		return SUPERCLASS::calcTotalMethodCount() + nlen;									\
	}																						\
																							\
	/** �������м̳�ģ��ı�¶��Ա���� 
	*/																						\
	static int calcTotalMemberCount(void)													\
	{																						\
		int nlen = 0;																		\
		while(true)																			\
		{																					\
			PyMemberDef* pmd = &_##CLASS##_scriptMembers[nlen];								\
			if(!pmd->doc && !pmd->flags && !pmd->type && !pmd->name && !pmd->offset)		\
				break;																		\
			nlen++;																			\
		}																					\
																							\
		if(strcmp(#CLASS, #SUPERCLASS) == 0)												\
			return nlen;																	\
		return SUPERCLASS::calcTotalMemberCount() + nlen;									\
	}																						\
																							\
	/** �������м̳�ģ��ı�¶getset���� 
	*/																						\
	static int calcTotalGetSetCount(void)													\
	{																						\
		int nlen = 0;																		\
		while(true)																			\
		{																					\
			PyGetSetDef* pgs = &_##CLASS##_scriptGetSeters[nlen];							\
			if(!pgs->doc && !pgs->get && !pgs->set && !pgs->name && !pgs->closure)			\
				break;																		\
			nlen++;																			\
		}																					\
																							\
		if(strcmp(#CLASS, #SUPERCLASS) == 0)												\
			return nlen;																	\
		return SUPERCLASS::calcTotalGetSetCount() + nlen;									\
	}																						\
																							\
	/** �����и����Լ���ǰģ��ı�¶��Ա�ͷ�����װ������Ҫ����ű����б��� 
	*/																						\
	static void setupScriptMethodAndAttribute(PyMethodDef* lppmf, PyMemberDef* lppmd,		\
	PyGetSetDef* lppgs)																		\
	{																						\
		int i = 0;																			\
		PyMethodDef* pmf = NULL;															\
		PyMemberDef* pmd = NULL;															\
		PyGetSetDef* pgs = NULL;															\
																							\
		while(true){																		\
			pmf = &_##CLASS##_scriptMethods[i];												\
			if(!pmf->ml_doc && !pmf->ml_flags && !pmf->ml_meth && !pmf->ml_name)			\
				break;																		\
			i++;																			\
			*(lppmf++) = *pmf;																\
		}																					\
																							\
		i = 0;																				\
		while(true){																		\
			pmd = &_##CLASS##_scriptMembers[i];												\
			if(!pmd->doc && !pmd->flags && !pmd->type && !pmd->name && !pmd->offset)		\
				break;																		\
			i++;																			\
			*(lppmd++) = *pmd;																\
		}																					\
																							\
		i = 0;																				\
		while(true){																		\
			pgs = &_##CLASS##_scriptGetSeters[i];											\
			if(!pgs->doc && !pgs->get && !pgs->set && !pgs->name && !pgs->closure)			\
				break;																		\
			i++;																			\
			*(lppgs++) = *pgs;																\
		}																					\
																							\
		if(strcmp(#CLASS, #SUPERCLASS) == 0){												\
			*(lppmf) = *pmf;																\
			*(lppmd) = *pmd;																\
			*(lppgs) = *pgs;																\
			return;																			\
		}																					\
																							\
		SUPERCLASS::setupScriptMethodAndAttribute(lppmf, lppmd, lppgs);						\
	}																						\
																							\
	/** ע��ű�ģ��
		@param mod: ��Ҫ�������ģ��
	*/																						\
	static void registerScript(PyObject* mod, const char* name = #CLASS)					\
	{																						\
		int nMethodCount = CLASS::calcTotalMethodCount();									\
		int nMemberCount = CLASS::calcTotalMemberCount();									\
		int nGetSetCount = CLASS::calcTotalGetSetCount();									\
																							\
		_##CLASS##_lpScriptmethods = new PyMethodDef[nMethodCount + 2];						\
		_##CLASS##_lpScriptmembers	= new PyMemberDef[nMemberCount + 2];					\
		_##CLASS##_lpgetseters		= new PyGetSetDef[nGetSetCount + 2];					\
																							\
		setupScriptMethodAndAttribute(_##CLASS##_lpScriptmethods,							\
									  _##CLASS##_lpScriptmembers,							\
									  _##CLASS##_lpgetseters);								\
																							\
		_scriptType.tp_methods		= _##CLASS##_lpScriptmethods;							\
		_scriptType.tp_members		= _##CLASS##_lpScriptmembers;							\
		_scriptType.tp_getset		= _##CLASS##_lpgetseters;								\
																							\
		if (PyType_Ready(&_scriptType) < 0){												\
			ERROR_MSG("registerScript(): PyType_Ready(" #CLASS ") error!");					\
			PyErr_Print();																	\
			return;																			\
		}																					\
																							\
		if(mod)																				\
		{																					\
			Py_INCREF(&_scriptType);														\
			if(PyModule_AddObject(mod, name, (PyObject *)&_scriptType) < 0)					\
			{																				\
				ERROR_MSG(fmt::format("registerScript(): PyModule_AddObject({}) error!", name));\
			}																				\
		}																					\
																							\
		SCRIPT_ERROR_CHECK();																\
		_##CLASS##_py_installed = true;														\
	}																						\
																							\
	/** ��װ��ǰ�ű�ģ�� 
		@param mod: ��Ҫ�������ģ��
	*/																						\
	static void installScript(PyObject* mod, const char* name = #CLASS)						\
	{																						\
		CLASS::onInstallScript(mod);														\
																							\
		registerScript(mod, name);															\
		ScriptObject::scriptObjectTypes[name] = &_scriptType;								\
	}																						\
																							\
	/** ע���ű�ģ��
	*/																						\
	static void unregisterScript(void)														\
	{																						\
		SAFE_RELEASE_ARRAY(_##CLASS##_lpScriptmethods);										\
		SAFE_RELEASE_ARRAY(_##CLASS##_lpScriptmembers);										\
		SAFE_RELEASE_ARRAY(_##CLASS##_lpgetseters);											\
																							\
		if(_##CLASS##_py_installed)															\
			Py_DECREF(&_scriptType);														\
	}																						\
																							\
	/** ж�ص�ǰ�ű�ģ�� 
	*/																						\
	static void uninstallScript(void)														\
	{																						\
		CLASS::onUninstallScript();															\
		unregisterScript();																	\
	}																						\




/** �������ʽ�ĳ�ʼ��һ���ű�ģ�飬 ��һЩ��Ҫ����Ϣ��䵽python��type������
*/
#define SCRIPT_INIT(CLASS, CALL, SEQ, MAP, ITER, ITERNEXT)									\
		TEMPLATE_SCRIPT_INIT(;,CLASS, CLASS, CALL, SEQ, MAP, ITER, ITERNEXT)				\


#define TEMPLATE_SCRIPT_INIT(TEMPLATE_HEADER, TEMPLATE_CLASS,								\
	CLASS, CALL, SEQ, MAP, ITER, ITERNEXT)													\
	TEMPLATE_HEADER PyMethodDef* TEMPLATE_CLASS::_##CLASS##_lpScriptmethods = NULL;			\
	TEMPLATE_HEADER PyMemberDef* TEMPLATE_CLASS::_##CLASS##_lpScriptmembers = NULL;			\
	TEMPLATE_HEADER PyGetSetDef* TEMPLATE_CLASS::_##CLASS##_lpgetseters = NULL;				\
																							\
	TEMPLATE_HEADER																			\
	PyTypeObject TEMPLATE_CLASS::_scriptType =												\
	{																						\
		PyVarObject_HEAD_INIT(&PyType_Type, 0)												\
		#CLASS,													/* tp_name            */	\
		sizeof(TEMPLATE_CLASS),									/* tp_basicsize       */	\
		0,														/* tp_itemsize        */	\
		(destructor)TEMPLATE_CLASS::_tp_dealloc,				/* tp_dealloc         */	\
		0,														/* tp_print           */	\
		0,														/* tp_getattr         */	\
		0,														/* tp_setattr         */	\
		0,														/* tp_compare         */	\
		TEMPLATE_CLASS::_tp_repr,								/* tp_repr            */	\
		0,														/* tp_as_number       */	\
		SEQ,													/* tp_as_sequence     */	\
		MAP,													/* tp_as_mapping      */	\
		0,														/* tp_hash            */	\
		CALL,													/* tp_call            */	\
		TEMPLATE_CLASS::_tp_str,								/* tp_str             */	\
		(getattrofunc)CLASS::_tp_getattro,						/* tp_getattro        */	\
		(setattrofunc)CLASS::_tp_setattro,						/* tp_setattro        */	\
		0,														/* tp_as_buffer       */	\
		Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,				/* tp_flags           */	\
		"KBEngine::" #CLASS " objects.",						/* tp_doc             */	\
		0,														/* tp_traverse        */	\
		0,														/* tp_clear           */	\
		0,														/* tp_richcompare     */	\
		0,														/* tp_weaklistoffset  */	\
		(getiterfunc)ITER,										/* tp_iter            */	\
		(iternextfunc)ITERNEXT,									/* tp_iternext        */	\
		0,														/* tp_methods         */	\
		0,														/* tp_members         */	\
		0,														/* tp_getset          */	\
		TEMPLATE_CLASS::getBaseScriptType(),					/* tp_base            */	\
		0,														/* tp_dict            */	\
		0,														/* tp_descr_get       */	\
		0,														/* tp_descr_set       */	\
		TEMPLATE_CLASS::calcDictOffset(),						/* tp_dictoffset      */	\
		(initproc)TEMPLATE_CLASS::_tp_init,						/* tp_init            */	\
		0,														/* tp_alloc           */	\
		TEMPLATE_CLASS::_tp_new,								/* tp_new             */	\
		PyObject_GC_Del,										/* tp_free            */	\
	};																						\

// BASE_SCRIPT_HREADER������ű���ʼ��, �����ɽű��̳�
#define BASE_SCRIPT_INIT(CLASS, CALL, SEQ, MAP, ITER, ITERNEXT)								\
	PyMethodDef* CLASS::_##CLASS##_lpScriptmethods = NULL;									\
	PyMemberDef* CLASS::_##CLASS##_lpScriptmembers = NULL;									\
	PyGetSetDef* CLASS::_##CLASS##_lpgetseters = NULL;										\
																							\
	PyTypeObject CLASS::_scriptType =														\
	{																						\
		PyVarObject_HEAD_INIT(NULL, 0)														\
		#CLASS,													/* tp_name            */	\
		sizeof(CLASS),											/* tp_basicsize       */	\
		0,														/* tp_itemsize        */	\
		(destructor)CLASS::_tp_dealloc,							/* tp_dealloc         */	\
		0,														/* tp_print           */	\
		0,														/* tp_getattr         */	\
		0,														/* tp_setattr         */	\
		0,														/* void *tp_reserved  */	\
		CLASS::_tp_repr,										/* tp_repr            */	\
		0,														/* tp_as_number       */	\
		SEQ,													/* tp_as_sequence     */	\
		MAP,													/* tp_as_mapping      */	\
		0,														/* tp_hash            */	\
		CALL,													/* tp_call            */	\
		CLASS::_tp_str,											/* tp_str             */	\
		(getattrofunc)CLASS::_tp_getattro,						/* tp_getattro        */	\
		(setattrofunc)CLASS::_tp_setattro,						/* tp_setattro        */	\
		0,														/* tp_as_buffer       */	\
		Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,				/* tp_flags           */	\
		0,														/* tp_doc             */	\
		0,														/* tp_traverse        */	\
		0,														/* tp_clear           */	\
		0,														/* tp_richcompare     */	\
		0,														/* tp_weaklistoffset  */	\
		(getiterfunc)ITER,										/* tp_iter            */	\
		(iternextfunc)ITERNEXT,									/* tp_iternext        */	\
		0,														/* tp_methods         */	\
		0,														/* tp_members         */	\
		0,														/* tp_getset          */	\
		CLASS::getBaseScriptType(),								/* tp_base            */	\
		0,														/* tp_dict            */	\
		0,														/* tp_descr_get       */	\
		0,														/* tp_descr_set       */	\
		CLASS::calcDictOffset(),								/* tp_dictoffset      */	\
		0,														/* tp_init            */	\
		0,														/* tp_alloc           */	\
		0,														/* tp_new             */	\
		PyObject_GC_Del,										/* tp_free            */	\
		0,														/* tp_is_gc           */	\
		0,														/* tp_bases           */	\
		0,														/* tp_mro             */	\
		0,														/* tp_cache           */	\
		0,														/* tp_subclasses      */	\
		0,														/* tp_weaklist        */	\
		0,														/* tp_del			  */	\
	};																						\


class ScriptObject: public PyObject
{
	/** 
		���໯ ��һЩpy�������������� 
	*/
	SCRIPT_OBJECT_HREADER(ScriptObject, ScriptObject)							
public:	
	ScriptObject(PyTypeObject* pyType, bool isInitialised = false);
	~ScriptObject();

	// ���е�kbe�ű����
	typedef KBEUnordered_map<std::string, PyTypeObject*> SCRIPTOBJECT_TYPES;
	static SCRIPTOBJECT_TYPES scriptObjectTypes;
	static PyTypeObject* getScriptObjectType(const std::string& name);

	/** 
		�ű��������ü��� 
	*/
	void incRef() const				{ Py_INCREF((PyObject*)this); }
	void decRef() const				{ Py_DECREF((PyObject*)this); }

	int refCount() const			{ return int(((PyObject*)this)->ob_refcnt); }
	
	/** 
		��ö�������� 
	*/
	PyObject* tp_repr();
	PyObject* tp_str();

	DECLARE_PY_GET_MOTHOD(py__module__);
	DECLARE_PY_GET_MOTHOD(py__qualname__);
	DECLARE_PY_GET_MOTHOD(py__name__);

	/** 
		�ű����󴴽�һ���ö��� 
	*/
	static PyObject* tp_new(PyTypeObject* type, PyObject* args, 
		PyObject* kwds);

	/** 
		�ű������ȡ���Ի��߷��� 
	*/
	PyObject* onScriptGetAttribute(PyObject* attr);						

	/** 
		�ű������������Ի��߷��� 
	*/
	int onScriptSetAttribute(PyObject* attr, PyObject* value);			

	/** 
		�ű�����ɾ��һ������ 
	*/
	int onScriptDelAttribute(PyObject* attr);

	/** 
		�ű������ʼ�� 
	*/
	int onScriptInit(PyObject* self, PyObject *args, 
		PyObject* kwds);

	/** 
		��ȡ�����������
	*/
	const char* scriptName() const { return ob_type->tp_name; }

	/** 
		�ű�����װʱ������ 
	*/
	static void onInstallScript(PyObject* mod) {}

	/** 
		�ű���ж��ʱ������ 
	*/
	static void onUninstallScript() {}
} ;

}
}


#ifdef CODE_INLINE
#include "scriptobject.inl"
#endif

#endif // KBE_SCRIPTOBJECT_H
