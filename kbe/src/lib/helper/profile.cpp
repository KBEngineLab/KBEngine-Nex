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

#include "profile.h"
#include "profile_latency.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include "helper/watcher.h"

#ifndef CODE_INLINE
#include "profile.inl"
#endif


namespace KBEngine
{

namespace
{
uint64 profileLatencyMaxAgeStamps()
{
	const double defaultSeconds = 10.0;
	const char* configured = std::getenv("KBE_PERF_PROFILE_LATENCY_WINDOW_SECONDS");
	if (configured == NULL || configured[0] == '\0')
		return static_cast<uint64>(defaultSeconds * stampsPerSecondD());

	char* end = NULL;
	errno = 0;
	const double seconds = std::strtod(configured, &end);
	if (errno != 0 || end == configured || *end != '\0' || !std::isfinite(seconds) ||
		seconds < 1.0 || seconds > 1800.0)
	{
		WARNING_MSG(fmt::format(
			"ProfileVal: ignoring invalid KBE_PERF_PROFILE_LATENCY_WINDOW_SECONDS='{}'\n",
			configured));
		return static_cast<uint64>(defaultSeconds * stampsPerSecondD());
	}

	return static_cast<uint64>(seconds * stampsPerSecondD());
}
}

ProfileGroup* g_pDefaultGroup = NULL;
TimeStamp ProfileVal::warningPeriod_;
TimeStamp ProfileVal::warningLogInterval_;

//-------------------------------------------------------------------------------------
uint64 runningTime()
{
	return ProfileGroup::defaultGroup().runningTime();
}

//-------------------------------------------------------------------------------------
ProfileGroup::ProfileGroup(std::string name):
name_(name)
{
	stampsPerSecond();

	ProfileVal * pRunningTime = new ProfileVal("RunningTime", this);
	pRunningTime->start();
}

//-------------------------------------------------------------------------------------
ProfileGroup::~ProfileGroup()
{
	delete this->pRunningTime();
}

//-------------------------------------------------------------------------------------
void ProfileGroup::finalise(void)
{
	SAFE_RELEASE(g_pDefaultGroup);
}

//-------------------------------------------------------------------------------------
TimeStamp ProfileGroup::runningTime() const
{
	return timestamp() - this->pRunningTime()->lastTime_;
}

//-------------------------------------------------------------------------------------
void ProfileGroup::add( ProfileVal * pVal )
{
	profiles_.push_back( pVal );
}

//-------------------------------------------------------------------------------------
ProfileGroup & ProfileGroup::defaultGroup()
{
	if(g_pDefaultGroup == NULL)
		g_pDefaultGroup = new ProfileGroup();
	return *g_pDefaultGroup;
}

//-------------------------------------------------------------------------------------
bool ProfileGroup::initializeWatcher()
{
	return true;
}

//-------------------------------------------------------------------------------------
ProfileVal::ProfileVal(std::string name, ProfileGroup * pGroup, bool collectLatency):
	name_(name),
	pProfileGroup_(pGroup),
	lastTime_(0),
	sumTime_(0),
	lastIntTime_(0),
	sumIntTime_(0),
	lastQuantity_(0),
	sumQuantity_(0),
	count_(0),
	inProgress_(0),
	initWatcher_(false),
	collectLatencyRequested_(collectLatency),
	pLatencyWindow_(NULL)
{
	if (pProfileGroup_ == NULL)
	{
		pProfileGroup_ = &ProfileGroup::defaultGroup();
	}

	if (!name_.empty())
	{
		pProfileGroup_->add( this );
	}
}

//-------------------------------------------------------------------------------------
ProfileVal::~ProfileVal()
{
	SAFE_RELEASE(pLatencyWindow_);

	if (pProfileGroup_)
	{
		// pProfileGroup_.erase(std::remove( pProfileGroup_->begin(), pProfileGroup_->end(), this ), pProfileGroup_->end());
	}
}

//-------------------------------------------------------------------------------------
bool ProfileVal::initializeWatcher()
{
	if(initWatcher_)
		return false;

	initWatcher_ = true;

	// 静态 ProfileVal 早于配置加载构造，延迟到 Watcher 初始化后才能遵守生产开关并避免无用的大窗口分配。
	// Static ProfileVal instances predate configuration loading, so defer allocation until Watcher initialization can honor the production switch.
	if (collectLatencyRequested_ && g_performanceProbesEnabled && pLatencyWindow_ == NULL)
	{
		pLatencyWindow_ = new ProfileLatencyWindow(
			ProfileLatencyWindow::DEFAULT_CAPACITY,
			profileLatencyMaxAgeStamps());
	}

	char buf[MAX_BUF];
	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/lastTime", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, &lastTime_, &TimeStamp::stamp);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/sumTime", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, &sumTime_, &TimeStamp::stamp);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/lastIntTime", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, &lastIntTime_, &TimeStamp::stamp);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/sumIntTime", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, &sumIntTime_, &TimeStamp::stamp);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/lastQuantity", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, lastQuantity_);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/sumQuantity", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, sumQuantity_);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/count", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, count_);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/inProgress", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, inProgress_);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/slowCalls", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, this, &ProfileVal::slowCallCount);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/warningLogs", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, this, &ProfileVal::warningLogCount);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/suppressedWarnings", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, this, &ProfileVal::suppressedWarningCount);

	kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/slowMaxMicros", pProfileGroup_->name(), name_.c_str());
	WATCH_OBJECT(buf, this, &ProfileVal::maxSlowMicros);

	if (pLatencyWindow_)
	{
		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/count", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyCount);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/meanMicros", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyMeanMicros);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/p50Micros", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyP50Micros);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/p95Micros", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyP95Micros);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/p99Micros", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyP99Micros);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/p999Micros", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyP999Micros);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/maxMicros", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyMaxMicros);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/p999Available", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyP999Available);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/windowCapacity", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyWindowCapacity);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/windowMaxAgeSeconds", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyWindowMaxAgeSeconds);

		kbe_snprintf(buf, MAX_BUF, "cprofiles/%s/%s/latency/windowAllocatedBytes", pProfileGroup_->name(), name_.c_str());
		WATCH_OBJECT(buf, this, &ProfileVal::latencyWindowAllocatedBytes);
	}

	return true;
}

//-------------------------------------------------------------------------------------
void ProfileVal::recordLatency(TimeStamp duration, TimeStamp completedAt)
{
	pLatencyWindow_->record(duration, completedAt);
}

namespace
{
double latencyStampsToMicros(double stamps)
{
	return stamps / stampsPerSecondD() * 1000000.0;
}
}

//-------------------------------------------------------------------------------------
uint64 ProfileVal::latencyCount()
{
	return pLatencyWindow_ ? pLatencyWindow_->snapshot(timestamp()).count : 0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::latencyMeanMicros()
{
	return pLatencyWindow_ ? latencyStampsToMicros(pLatencyWindow_->snapshot(timestamp()).meanStamps) : 0.0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::latencyP50Micros()
{
	return pLatencyWindow_ ? latencyStampsToMicros(static_cast<double>(
		pLatencyWindow_->snapshot(timestamp()).p50Stamps)) : 0.0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::latencyP95Micros()
{
	return pLatencyWindow_ ? latencyStampsToMicros(static_cast<double>(
		pLatencyWindow_->snapshot(timestamp()).p95Stamps)) : 0.0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::latencyP99Micros()
{
	return pLatencyWindow_ ? latencyStampsToMicros(static_cast<double>(
		pLatencyWindow_->snapshot(timestamp()).p99Stamps)) : 0.0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::latencyP999Micros()
{
	return pLatencyWindow_ ? latencyStampsToMicros(static_cast<double>(
		pLatencyWindow_->snapshot(timestamp()).p999Stamps)) : 0.0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::latencyMaxMicros()
{
	return pLatencyWindow_ ? latencyStampsToMicros(static_cast<double>(
		pLatencyWindow_->snapshot(timestamp()).maxStamps)) : 0.0;
}

//-------------------------------------------------------------------------------------
bool ProfileVal::latencyP999Available()
{
	return pLatencyWindow_ && pLatencyWindow_->snapshot(timestamp()).p999Available;
}

//-------------------------------------------------------------------------------------
uint64 ProfileVal::latencyWindowCapacity()
{
	return pLatencyWindow_ ? pLatencyWindow_->capacity() : 0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::latencyWindowMaxAgeSeconds()
{
	return pLatencyWindow_ ? static_cast<double>(pLatencyWindow_->maxAgeStamps()) / stampsPerSecondD() : 0.0;
}

//-------------------------------------------------------------------------------------
uint64 ProfileVal::latencyWindowAllocatedBytes()
{
	return pLatencyWindow_ ? pLatencyWindow_->allocatedBytes() : 0;
}

//-------------------------------------------------------------------------------------
double ProfileVal::maxSlowMicros() const
{
	return latencyStampsToMicros(static_cast<double>(warningLimiter_.maxSlowDuration()));
}

//-------------------------------------------------------------------------------------
} 

