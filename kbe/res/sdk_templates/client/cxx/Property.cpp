
#include "Property.h"
#include "KBDebug.h"

namespace KBEngine
{

Property::Property():
	name(KBTEXT("")),
	pUtype(NULL),
	properUtype(0),
	properFlags(0),
	aliasID(-1),
	defaultValStr(KBTEXT("")),
	pDefaultVal(NULL)
{
}

Property::~Property()
{
	KBE_SAFE_RELEASE(pDefaultVal);
}

}