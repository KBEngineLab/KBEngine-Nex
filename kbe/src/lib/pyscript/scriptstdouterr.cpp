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


#include "scriptstdouterr.h"

#ifndef CODE_INLINE
#include "scriptstdouterr.inl"
#endif

namespace KBEngine{ namespace script {

//-------------------------------------------------------------------------------------
ScriptStdOutErr::ScriptStdOutErr():
pStderr_(NULL),
pStdout_(NULL),
pyPrint_(NULL),
isInstall_(false),
sbuffer_(),
errorBuffer_()
{
}

//-------------------------------------------------------------------------------------
ScriptStdOutErr::~ScriptStdOutErr()
{
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErr::info_msg(const char* msg, uint32 msglen)
{
	if (msg == NULL || msglen == 0)
		return;

	sbuffer_.append(msg, msglen);
	emitCompleteLines(sbuffer_, false);
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErr::error_msg(const char* msg, uint32 msglen)
{
	if (msg == NULL || msglen == 0)
		return;

	errorBuffer_.append(msg, msglen);
	emitCompleteLines(errorBuffer_, true);
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErr::emitCompleteLines(std::string& buffer, bool isError)
{
	/*
		Python 可以在一次 write() 中传入多行，也可以将一行拆成多次 write()；只有完整行才会在此刻进入日志。
		Python may pass multiple lines in one write() or split one line across writes; only complete lines enter the log here.
	*/
	std::string::size_type lineBegin = 0;
	std::string::size_type newline = buffer.find('\n', lineBegin);
	while (newline != std::string::npos)
	{
		const std::string line = buffer.substr(lineBegin, newline - lineBegin + 1);
		if (isError)
			SCRIPT_ERROR_MSG(line);
		else
			SCRIPT_INFO_MSG(line);

		lineBegin = newline + 1;
		newline = buffer.find('\n', lineBegin);
	}

	if (lineBegin > 0)
		buffer.erase(0, lineBegin);
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErr::flushBuffer(std::string& buffer, bool isError)
{
	if (buffer.empty())
		return;

	/*
		flush 的半行需要独立结束日志记录，否则下一次 stdout/stderr 写入会在控制台和日志文件中粘到同一行。
		A partial line flushed by Python must terminate its log record or the next stdout/stderr write will be joined to it in consoles and log files.
	*/
	if (buffer[buffer.size() - 1] != '\n')
		buffer += '\n';

	if (isError)
		SCRIPT_ERROR_MSG(buffer);
	else
		SCRIPT_INFO_MSG(buffer);

	buffer.clear();
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErr::flush_info()
{
	flushBuffer(sbuffer_, false);
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErr::flush_error()
{
	flushBuffer(errorBuffer_, true);
}

//-------------------------------------------------------------------------------------
bool ScriptStdOutErr::install(void)
{
	pStderr_ = new ScriptStdErr(this);
	pStdout_ = new ScriptStdOut(this);
	if (!pStderr_->install())
	{
		Py_DECREF(pStderr_);
		Py_DECREF(pStdout_);
		pStderr_ = NULL;
		pStdout_ = NULL;
		return false;
	}

	if (!pStdout_->install())
	{
		PyObject* errorType = NULL;
		PyObject* errorValue = NULL;
		PyObject* errorTraceback = NULL;
		PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
		pStderr_->uninstall();
		Py_DECREF(pStderr_);
		Py_DECREF(pStdout_);
		pStderr_ = NULL;
		pStdout_ = NULL;
		PyErr_Restore(errorType, errorValue, errorTraceback);
		return false;
	}

	PyObject * m = PyImport_ImportModule("builtins");
	if (!m)
	{
		ERROR_MSG("ScriptStdOutErr: Failed to import builtins module\n");
		PyObject* errorType = NULL;
		PyObject* errorValue = NULL;
		PyObject* errorTraceback = NULL;
		PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
		pStdout_->uninstall();
		pStderr_->uninstall();
		Py_DECREF(pStderr_);
		Py_DECREF(pStdout_);
		pStderr_ = NULL;
		pStdout_ = NULL;
		PyErr_Restore(errorType, errorValue, errorTraceback);
		return false;
	}

	pyPrint_ = PyObject_GetAttrString(m, "print");
	Py_DECREF(m);
	if (!pyPrint_)
	{
		PyObject* errorType = NULL;
		PyObject* errorValue = NULL;
		PyObject* errorTraceback = NULL;
		PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
		pStdout_->uninstall();
		pStderr_->uninstall();
		Py_DECREF(pStderr_);
		Py_DECREF(pStdout_);
		pStderr_ = NULL;
		pStdout_ = NULL;
		PyErr_Restore(errorType, errorValue, errorTraceback);
		return false;
	}

	isInstall_ = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool ScriptStdOutErr::uninstall(void)
{
	flush_info();
	flush_error();
	bool success = true;

	if (pStderr_)
	{
		if(!pStderr_->uninstall())
			success = false;

		Py_DECREF(pStderr_);
		pStderr_ = NULL;
	}

	if (pStdout_)
	{
		if(!pStdout_->uninstall())
			success = false;

		Py_DECREF(pStdout_);
		pStdout_ = NULL;
	}

	if (pyPrint_)
	{
		Py_DECREF(pyPrint_);
		pyPrint_ = NULL;
	}

	isInstall_ = false;
	return success;
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErr::pyPrint(const std::string& str)
{
	PyObject* pyRet = PyObject_CallFunction(pyPrint_, 
		const_cast<char*>("(s)"), str.c_str());
	
	SCRIPT_ERROR_CHECK();
	
	if(pyRet)
	{
		S_RELEASE(pyRet);
	}
}

//-------------------------------------------------------------------------------------

}
}
