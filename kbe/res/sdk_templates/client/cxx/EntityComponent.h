// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "KBECommon.h"

namespace KBEngine
{

class Entity;
class MemoryStream;
class ScriptModule;

class EntityComponent
{
public:
	EntityComponent();
	virtual ~EntityComponent();

public:
	virtual void onAttached(Entity*)
	{
	}
		
	virtual void onDetached(Entity*)
	{
	}

	virtual void onEnterworld()
	{
	}

	virtual void onLeaveworld()
	{
	}

	virtual ScriptModule* getScriptModule()
	{
		return NULL;
	}

	virtual void onGetBase()
	{
		// 动态生成
	}

	virtual void onGetCell()
	{
		// 动态生成
	}

	virtual void onLoseCell()
	{
		// 动态生成
	}

	virtual void onRemoteMethodCall(uint16, MemoryStream&)
	{
		// 动态生成
	}

	virtual void onUpdatePropertys(uint16, MemoryStream&, int)
	{
		// 动态生成
	}

	virtual void callPropertysSetMethods()
	{
		// 动态生成
	}

	virtual void createFromStream(MemoryStream& stream);

public:
	uint16 entityComponentPropertyID;
	uint16 componentType;
	ENTITY_ID ownerID;
	Entity* pOwner;
	KBString name_;
};

}
