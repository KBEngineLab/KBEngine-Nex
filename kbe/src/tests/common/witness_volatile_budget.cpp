#include "server/cellapp/witness_volatile_budget.h"

#include <cstdlib>
#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

bool testBoundedBudgetAllowsOneCompleteUpdate()
{
	KBEngine::WitnessVolatileBudget budget(16);
	budget.recordBundleGrowth(100, 112);
	if (!require(budget.canSend(false), "budget stopped before reaching its byte limit"))
		return false;

	budget.recordBundleGrowth(112, 120);
	return require(!budget.canSend(false), "budget admitted another update after reaching its limit") &&
		require(budget.canSend(true), "structural update was blocked by the volatile byte budget") &&
		require(budget.bytesSent() == 20, "budget did not retain the complete encoded update size");
}

bool testUnlimitedAndShrinkingBundle()
{
	KBEngine::WitnessVolatileBudget budget(0);
	budget.recordBundleGrowth(20, 12);
	budget.recordBundleGrowth(12, 100000);
	return require(budget.canSend(false), "zero byte limit did not preserve unlimited mode") &&
		require(budget.bytesSent() == 99988, "bundle growth accounting underflowed or drifted");
}
}

int main()
{
	if (!testBoundedBudgetAllowsOneCompleteUpdate() || !testUnlimitedAndShrinkingBundle())
		return EXIT_FAILURE;

	std::cout << "WITNESS_VOLATILE_BUDGET_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
