#include "common/timer.h"

#include <cstdlib>
#include <iostream>

namespace
{

class CountingHandler : public KBEngine::TimerHandler
{
public:
	CountingHandler() : calls_(0) {}

	void handleTimeout(KBEngine::TimerHandle, void*) override
	{
		++calls_;
	}

	int calls() const { return calls_; }

private:
	int calls_;
};

bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

}

int main()
{
	KBEngine::Timers timers;
	CountingHandler periodic;
	KBEngine::TimerHandle periodicHandle = timers.add(10, 10, &periodic, NULL);

	if (!require(timers.process(10) == 1 && periodic.calls() == 1,
		"periodic timer did not fire at its initial deadline"))
		return EXIT_FAILURE;

	if (!require(timers.process(55) == 1 && periodic.calls() == 2,
		"overdue periodic timer replayed more than one callback"))
		return EXIT_FAILURE;

	if (!require(timers.skippedIntervals() == 3,
		"overdue periodic timer did not record three skipped intervals"))
		return EXIT_FAILURE;

	if (!require(timers.process(55) == 0 && timers.process(60) == 1,
		"periodic timer did not preserve its original phase"))
		return EXIT_FAILURE;

	periodicHandle.cancel();
	timers.process(60);

	CountingHandler first;
	CountingHandler second;
	CountingHandler third;
	timers.add(100, 0, &first, NULL);
	timers.add(100, 0, &second, NULL);
	timers.add(100, 0, &third, NULL);

	if (!require(timers.process(100, 2) == 2 && timers.size() == 1,
		"timer callback budget did not preserve the remaining due timer"))
		return EXIT_FAILURE;

	if (!require(timers.budgetExhaustions() == 1 && timers.lastFired() == 2,
		"timer callback budget metrics are incorrect"))
		return EXIT_FAILURE;

	if (!require(timers.process(100, 2) == 1 && timers.empty(),
		"deferred due timer did not run in the following process round"))
		return EXIT_FAILURE;

	std::cout << "TIMER_FAIRNESS_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
