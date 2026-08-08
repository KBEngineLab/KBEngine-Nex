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

#include "script.h"
#include "pyprofile.h"
#include "pyobject_pointer.h"
#include "common/memorystream.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

namespace KBEngine{ 
namespace script{

namespace
{
struct ProfileStatRow
{
	uint32 callCount = 0;
	uint32 recursiveCallCount = 0;
	double totalTime = 0.0;
	double inlineTime = 0.0;
	std::string functionName;
};

bool pyObjectToString(PyObject* object, std::string& value)
{
	if (!object)
		return false;

	PyObject* pyString = PyObject_Str(object);
	if (!pyString)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	const char* utf8 = PyUnicode_AsUTF8(pyString);
	if (utf8)
		value.assign(utf8);
	else
		SCRIPT_ERROR_CHECK();

	Py_DECREF(pyString);
	return utf8 != NULL;
}

bool pyAttrToString(PyObject* object, const char* attrName, std::string& value)
{
	PyObject* attr = PyObject_GetAttrString(object, attrName);
	if (!attr)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	bool ok = pyObjectToString(attr, value);
	Py_DECREF(attr);
	return ok;
}

bool pyAttrToUInt32(PyObject* object, const char* attrName, uint32& value)
{
	PyObject* attr = PyObject_GetAttrString(object, attrName);
	if (!attr)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	unsigned long result = PyLong_AsUnsignedLong(attr);
	Py_DECREF(attr);
	if (PyErr_Occurred())
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	value = static_cast<uint32>(result);
	return true;
}

bool pyAttrToDouble(PyObject* object, const char* attrName, double& value)
{
	PyObject* attr = PyObject_GetAttrString(object, attrName);
	if (!attr)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	double result = PyFloat_AsDouble(attr);
	Py_DECREF(attr);
	if (PyErr_Occurred())
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	value = result;
	return true;
}

std::string formatCodeName(PyObject* code)
{
	if (code && PyCode_Check(code))
	{
		std::string filename;
		std::string functionName;
		uint32 firstLine = 0;
		if (pyAttrToString(code, "co_filename", filename) &&
			pyAttrToString(code, "co_name", functionName) &&
			pyAttrToUInt32(code, "co_firstlineno", firstLine))
		{
			return fmt::format("{}:{}({})", filename, firstLine, functionName);
		}
	}

	std::string fallback;
	if (pyObjectToString(code, fallback))
		return fallback;

	return "<unknown>";
}

bool buildProfileStatRow(PyObject* profilerEntry, ProfileStatRow& row)
{
	if (!pyAttrToUInt32(profilerEntry, "callcount", row.callCount) ||
		!pyAttrToUInt32(profilerEntry, "reccallcount", row.recursiveCallCount) ||
		!pyAttrToDouble(profilerEntry, "totaltime", row.totalTime) ||
		!pyAttrToDouble(profilerEntry, "inlinetime", row.inlineTime))
	{
		return false;
	}

	PyObject* code = PyObject_GetAttrString(profilerEntry, "code");
	if (!code)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	row.functionName = formatCodeName(code);
	Py_DECREF(code);
	return true;
}

std::string formatCallCount(uint32 callCount, uint32 recursiveCallCount)
{
	if (recursiveCallCount > 0 && recursiveCallCount < callCount)
		return fmt::format("{}/{}", callCount, recursiveCallCount);

	return fmt::format("{}", callCount);
}

std::string buildPStatsText(const std::vector<ProfileStatRow>& rows, double elapsedSeconds)
{
	uint64 totalCalls = 0;
	uint64 primitiveCalls = 0;
	for (const ProfileStatRow& row : rows)
	{
		totalCalls += row.callCount;
		primitiveCalls += row.callCount >= row.recursiveCallCount ? row.callCount - row.recursiveCallCount : row.callCount;
	}

	std::ostringstream stream;
	stream << "         " << totalCalls << " function calls";
	if (primitiveCalls != totalCalls)
		stream << " (" << primitiveCalls << " primitive calls)";
	stream << " in " << std::fixed << std::setprecision(3) << elapsedSeconds << " seconds\n\n";
	stream << "   Ordered by: internal time\n\n";
	stream << "   ncalls  tottime  percall  cumtime  percall filename:lineno(function)\n";

	for (const ProfileStatRow& row : rows)
	{
		double perCallInline = row.callCount > 0 ? row.inlineTime / static_cast<double>(row.callCount) : 0.0;
		double perCallTotal = row.callCount > 0 ? row.totalTime / static_cast<double>(row.callCount) : 0.0;
		stream << std::setw(9) << formatCallCount(row.callCount, row.recursiveCallCount)
			<< " " << std::setw(8) << std::fixed << std::setprecision(3) << row.inlineTime
			<< " " << std::setw(8) << std::fixed << std::setprecision(3) << perCallInline
			<< " " << std::setw(8) << std::fixed << std::setprecision(3) << row.totalTime
			<< " " << std::setw(8) << std::fixed << std::setprecision(3) << perCallTotal
			<< " " << row.functionName << "\n";
	}

	return stream.str();
}
}

PyProfile::PROFILES PyProfile::profiles_;
bool PyProfile::isInit = false;
PyObject* PyProfile::profileMethod_ = NULL;
Script* PyProfile::pScript_ = NULL;

//-------------------------------------------------------------------------------------
bool PyProfile::initialize(Script* pScript)
{
	if (isInit)
		return true;

	PyObject* cProfileModule = PyImport_ImportModule("cProfile");

	if (!cProfileModule)
	{
		ERROR_MSG("can't import cProfile!\n");
		PyErr_PrintEx(0);
		return false;
	}

	profileMethod_ = PyObject_GetAttrString(cProfileModule, "Profile");
	Py_DECREF(cProfileModule);

	isInit = profileMethod_ != NULL;
	pScript_ = pScript;
	return isInit;
}

//-------------------------------------------------------------------------------------
void PyProfile::finalise(void)
{
	profiles_.clear();

	Py_XDECREF(profileMethod_);
	profileMethod_ = NULL;
}

//-------------------------------------------------------------------------------------
bool PyProfile::start(std::string profile, bool logMessage)
{
	PyProfile::PROFILES::iterator iter = profiles_.find(profile);
	if(iter != profiles_.end())
	{
		ERROR_MSG(fmt::format("PyProfile::start: profile({}) already exists!\n", profile));
		return false;
	}

	PyObject* pyRet = PyObject_CallFunction(profileMethod_, 
		const_cast<char*>(""));
	
	if(!pyRet)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}
	
	PyObject* pyRet1 = PyObject_CallMethod(pyRet, const_cast<char*>("enable"),
		const_cast<char*>(""));

	if(!pyRet1)
	{
		SCRIPT_ERROR_CHECK();
		Py_DECREF(pyRet);
		return false;
	}

	Py_DECREF(pyRet1);

	profiles_[profile] = pyRet;

	if(logMessage)
	{
		char buf[MAX_BUF];
		kbe_snprintf(buf, MAX_BUF, "print(\"PyProfile::start: profile=%s.\")", profile.c_str());
		pScript_->run_simpleString(buf, NULL);
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool PyProfile::stop(std::string profile, bool logMessage)
{
	PyProfile::PROFILES::iterator iter = profiles_.find(profile);
	if(iter == profiles_.end())
	{
		ERROR_MSG(fmt::format("PyProfile::stop: profile({}) is not exists!\n", profile));
		return false;
	}

	PyObject* pyRet = PyObject_CallMethod(iter->second.get(), const_cast<char*>("disable"),
		const_cast<char*>(""));
	
	if(!pyRet)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}
	
	Py_DECREF(pyRet);

	if(logMessage)
	{
		char buf[MAX_BUF];
		kbe_snprintf(buf, MAX_BUF, "print(\"PyProfile::stop: profile=%s.\")", profile.c_str());
		pScript_->run_simpleString(buf, NULL);
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool PyProfile::remove(std::string profile)
{
	PyProfile::PROFILES::iterator iter = profiles_.find(profile);
	if(iter == profiles_.end())
	{
		ERROR_MSG(fmt::format("PyProfile::remove: profile({}) is not exists!\n", profile));
		return false;
	}

	Py_DECREF(iter->second.get());

	profiles_.erase(iter);
	return true;
}

//-------------------------------------------------------------------------------------
void PyProfile::print_stats(const std::string& sort, const std::string& profileName)
{
	PyProfile::PROFILES::iterator iter = profiles_.find(profileName.c_str());
	if(iter == profiles_.end())
	{
		return;
	}

	PyObject* pyRet = PyObject_CallMethod(iter->second.get(), const_cast<char*>("print_stats"),
		const_cast<char*>("s"), const_cast<char*>(sort.c_str()));
	
	if(pyRet)
		Py_DECREF(pyRet);
	else
		SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
size_t PyProfile::addToStream(std::string profile, MemoryStream* s)
{
	PyProfile::PROFILES::iterator iter = profiles_.find(profile);
	if(iter == profiles_.end())
	{
		ERROR_MSG(fmt::format("PyProfile::getstats: profile({}) is not exists!\n", profile));
		return 0;
	}

	PyObject* pyStats = PyObject_CallMethod(iter->second.get(), const_cast<char*>("getstats"), const_cast<char*>(""));
	if (!pyStats)
	{
		ERROR_MSG("PyProfile::addToStream: can't get profile stats!\n");
		SCRIPT_ERROR_CHECK();
		return 0;
	}

	std::vector<ProfileStatRow> rows;
	Py_ssize_t statsSize = PySequence_Size(pyStats);
	if (statsSize < 0)
	{
		Py_DECREF(pyStats);
		SCRIPT_ERROR_CHECK();
		return 0;
	}

	rows.reserve(static_cast<size_t>(statsSize));
	double elapsedSeconds = 0.0;
	for (Py_ssize_t i = 0; i < statsSize; ++i)
	{
		PyObject* item = PySequence_GetItem(pyStats, i);
		if (!item)
		{
			SCRIPT_ERROR_CHECK();
			continue;
		}

		ProfileStatRow row;
		if (buildProfileStatRow(item, row))
		{
			elapsedSeconds += row.inlineTime;
			rows.push_back(row);
		}
		Py_DECREF(item);
	}

	Py_DECREF(pyStats);

	std::sort(rows.begin(), rows.end(), [](const ProfileStatRow& lhs, const ProfileStatRow& rhs)
	{
		return lhs.inlineTime > rhs.inlineTime;
	});

	std::string retBufferPtr = buildPStatsText(rows, elapsedSeconds);

	(*s) << retBufferPtr;
	return retBufferPtr.size();
}

//-------------------------------------------------------------------------------------
bool PyProfile::dump(std::string profile, std::string fileName)
{
	/* 加载结果
		import pstats
		p = pstats.Stats("*.prof")
		p.sort_stats("time").print_stats()
	*/

	PyProfile::PROFILES::iterator iter = profiles_.find(profile);
	if(iter == profiles_.end())
	{
		ERROR_MSG(fmt::format("PyProfile::dump: profile({}) is not exists!\n", profile));
		return false;
	}

	FILE* f = fopen(fileName.c_str(), "wb");
	if(f == NULL)
	{
		ERROR_MSG(fmt::format("PyProfile::dump: profile({}) can't open fileName={}!\n", profile, fileName));
		return false;
	}

	PyObject* pyRet = PyObject_CallMethod(iter->second.get(), const_cast<char*>("dump_stats"),
		const_cast<char*>("s"), fileName.c_str());

	SCRIPT_ERROR_CHECK();

	if(!pyRet)
	{
		ERROR_MSG(fmt::format("PyProfile::dump: save to {} error!\n", fileName));
		return false;
	}
	else
	{
		DEBUG_MSG(fmt::format("PyProfile::dump: save to {}.\n", fileName));
	}

	Py_DECREF(pyRet);

	pyRet = PyObject_CallMethod(iter->second.get(), const_cast<char*>("print_stats"),
		const_cast<char*>("s"), const_cast<char*>("time"));
	
	SCRIPT_ERROR_CHECK();
	
	if(pyRet)
		Py_DECREF(pyRet);

	return true;
}

//-------------------------------------------------------------------------------------

}
}
