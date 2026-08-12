#include "entitydef/method_utype_allocator.h"

#include <cstdlib>
#include <iostream>

namespace
{

void require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << std::endl;
		std::exit(EXIT_FAILURE);
	}
}

}

int main()
{
	using namespace KBEngine;

	MethodUTypeAllocator baseAllocator;
	ENTITY_METHOD_UID exposedBeforePrivate = 0;
	ENTITY_METHOD_UID privateMethod = 0;
	require(baseAllocator.allocateClientVisible(exposedBeforePrivate),
		"failed to allocate the first exposed method UType");
	require(baseAllocator.allocateServerPrivate(privateMethod),
		"failed to allocate a private method UType");
	require(exposedBeforePrivate == 1, "exposed method UTypes must start at the low end");
	require(privateMethod == 65535, "private method UTypes must start at the high end");

	ENTITY_METHOD_UID exposedAfterPrivate = 0;
	require(baseAllocator.allocateClientVisible(exposedAfterPrivate),
		"failed to allocate the second exposed method UType");
	require(exposedAfterPrivate == 2,
		"adding a private method must not renumber later client-visible methods");

	MethodUTypeAllocator clientAllocator;
	ENTITY_METHOD_UID clientMethod = 0;
	require(clientAllocator.allocateClientVisible(clientMethod),
		"failed to allocate a client method UType");
	require(clientMethod == 1,
		"different communication domains must be able to use the same UType");

	MethodUTypeAllocator anotherEntityAllocator;
	ENTITY_METHOD_UID anotherEntityMethod = 0;
	require(anotherEntityAllocator.allocateClientVisible(anotherEntityMethod),
		"failed to allocate a method for another entity module");
	require(anotherEntityMethod == 1,
		"different entity modules must have independent method UType spaces");

	MethodUTypeAllocator explicitAllocator;
	require(!explicitAllocator.reserve(0), "zero is not a valid explicit method UType");
	require(explicitAllocator.reserve(1), "failed to reserve an explicit method UType");
	ENTITY_METHOD_UID afterExplicit = 0;
	require(explicitAllocator.allocateClientVisible(afterExplicit),
		"failed to allocate after an explicit UType");
	require(afterExplicit == 2, "automatic allocation must skip explicit UTypes");

	explicitAllocator.reset();
	require(!explicitAllocator.isReserved(1), "reset must release all UType reservations");

	std::cout << "METHOD_UTYPE_ALLOCATOR_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
