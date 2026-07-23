// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "KBECommon.h"


namespace KBEngine
{

class Property;
class Method;
class Entity;

/*
	一个entitydef中定义的脚本模块的描述类
	包含了某个entity定义的属性与方法以及该entity脚本模块的名称与模块ID
*/
class ScriptModule
{
public:
	ScriptModule(const KBString& moduleName, int type);
	virtual ~ScriptModule();

	Entity* createEntity();

public:
	KBString name;
	bool usePropertyDescrAlias;
	bool useMethodDescrAlias;

	KBMap<KBString, Property*> propertys;
	KBMap<uint16, Property*> idpropertys;

	KBMap<KBString, Method*> methods;
	KBMap<KBString, Method*> base_methods;
	KBMap<KBString, Method*> cell_methods;

	KBMap<uint16, Method*> idmethods;
	KBMap<uint16, Method*> idbase_methods;
	KBMap<uint16, Method*> idcell_methods;

	uint16 utype;
};

}
