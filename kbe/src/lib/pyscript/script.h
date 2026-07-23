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

#ifndef KBENGINE_SCRIPT_H
#define KBENGINE_SCRIPT_H

#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/singleton.h"
#include "scriptobject.h"
#include "scriptstdouterr.h"
#include "scriptstdouterrhook.h"

namespace KBEngine{ namespace script{

/** �ű�ϵͳ·�� */
#ifdef _LP64
#define SCRIPT_PATH													\
					L"../../res/scripts;"							\
					L"../../res/scripts/common;"					\
					L"../../res/scripts/common/lib-dynload;"		\
					L"../../res/scripts/common/DLLs;"				\
					L"../../res/scripts/common/Lib;"				\
					L"../../res/scripts/common/Lib/site-packages;"	\
					L"../../res/scripts/common/Lib/dist-packages"

#else
#define SCRIPT_PATH													\
					L"../../res/scripts;"							\
					L"../../res/scripts/common;"					\
					L"../../res/scripts/common/lib-dynload;"		\
					L"../../res/scripts/common/DLLs;"				\
					L"../../res/scripts/common/Lib;"				\
					L"../../res/scripts/common/Lib/site-packages;"	\
					L"../../res/scripts/common/Lib/dist-packages"

#endif

#define APPEND_PYSYSPATH(PY_PATHS)									\
	std::wstring pySysPaths = SCRIPT_PATH;							\
	wchar_t* pwpySysResPath = strutil::char2wchar(const_cast<char*>(Resmgr::getSingleton().getPySysResPath().c_str()));	\
	strutil::kbe_replace(pySysPaths, L"../../res/", pwpySysResPath);\
	PY_PATHS += pySysPaths;											\
	free(pwpySysResPath);


PyObject * PyTuple_FromStringVector(const std::vector< std::string > & v);

template<class T>
PyObject * PyTuple_FromIntVector(const std::vector< T > & v)
{
	int sz = v.size();
	PyObject * t = PyTuple_New( sz );
	for (int i = 0; i < sz; ++i)
	{
		PyTuple_SetItem( t, i, PyLong_FromLong( v[i] ) );
	}

	return t;
}

template<>
inline PyObject * PyTuple_FromIntVector<int64>(const std::vector< int64 > & v)
{
	int sz = (int)v.size();
	PyObject * t = PyTuple_New( sz );
	for (int i = 0; i < sz; ++i)
	{
		PyTuple_SetItem( t, i, PyLong_FromLongLong( v[i] ) );
	}

	return t;
}

template<>
inline PyObject * PyTuple_FromIntVector<uint64>(const std::vector< uint64 > & v)
{
	int sz = (int)v.size();
	PyObject * t = PyTuple_New( sz );
	for (int i = 0; i < sz; ++i)
	{
		PyTuple_SetItem( t, i, PyLong_FromUnsignedLongLong( v[i] ) );
	}

	return t;
}

class Script: public Singleton<Script>
{						
public:	
	Script();
	virtual ~Script();
	
	/** 
		��װ��ж�ؽű�ģ�� 
	*/
	virtual bool install(const wchar_t* pythonHomeDir, std::wstring pyPaths, 
		const char* moduleName, COMPONENT_TYPE componentType);

	virtual bool uninstall(void);
	
	bool installExtraModule(const char* moduleName);

	/** 
		����һ����չ�ӿڵ�������չģ�� 
	*/
	bool registerExtraMethod(const char* attrName, PyMethodDef* pyFunc);

	/** 
		����һ����չ���Ե�������չģ�� 
	*/
	bool registerExtraObject(const char* attrName, PyObject* pyObj);

	/** 
		��ȡ�ű�����ģ�� 
	*/
	INLINE PyObject* getModule(void) const;

	/** 
		��ȡ�ű���չģ�� 
	*/
	INLINE PyObject* getExtraModule(void) const;

	/**
		��ȡ�ű���ʼ��ʱ����ģ��
	*/
	INLINE PyObject* getSysInitModules(void) const;

	int run_simpleString(const char* command, std::string* retBufferPtr);
	INLINE int run_simpleString(std::string command, std::string* retBufferPtr);

	int registerToModule(const char* attrName, PyObject* pyObj);
	int unregisterToModule(const char* attrName);

	INLINE ScriptStdOutErr* pyStdouterr() const;

	INLINE void pyPrint(const std::string& str);

	void setenv(const std::string& name, const std::string& value);

protected:
	PyObject* 					module_;
	PyObject*					extraModule_;		// ��չ�ű�ģ��
	PyObject*					sysInitModules_;	// ��ʼʱsys���ص�ģ��

	ScriptStdOutErr*			pyStdouterr_;
} ;

}
}

#ifdef CODE_INLINE
#include "script.inl"
#endif

#endif // KBENGINE_SCRIPT_H
