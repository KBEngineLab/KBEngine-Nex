// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#ifndef KBE_ENTITYDEF_H
#define KBE_ENTITYDEF_H

#include "common/common.h"
#include "common/md5.h"
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4910)
#pragma warning (disable : 4251)
#endif

#include "method.h"
#include "property.h"
#include "entity_call.h"
#include "math/math.h"
#include "pyscript/scriptobject.h"
#include "xml/xml.h"
#include "common/smartpointer.h"


namespace KBEngine{

class ScriptDefModule;
typedef SmartPointer<ScriptDefModule> ScriptDefModulePtr;

struct ReloadScriptDefStats
{
	ReloadScriptDefStats():
		ok(true),
		changedFiles(0),
		skippedFiles(0),
		reloadedModules(0),
		duplicateModulePatches(0),
		staleAttrsKept(0)
	{
	}

	bool ok;
	uint32 changedFiles;
	uint32 skippedFiles;
	uint32 reloadedModules;
	uint32 duplicateModulePatches;
	uint32 staleAttrsKept;
};

class EntityDef
{
public:
	typedef std::vector<ScriptDefModulePtr> SCRIPT_MODULES;
	typedef std::map<std::string, ENTITY_SCRIPT_UID> SCRIPT_MODULE_UID_MAP;

	typedef std::function<PyObject* (COMPONENT_ID componentID, ENTITY_ID& eid)> GetEntityFunc;
	typedef std::function<Network::Channel* (EntityCall&)> FindChannelFunc;

	EntityDef();
	~EntityDef();

	/**
		初始化
	*/
	static bool initialize(std::vector<PyTypeObject*>& scriptBaseTypes,
		COMPONENT_TYPE loadComponentType);

	static bool finalise(bool isReload = false);

	static ReloadScriptDefStats reload(bool fullReload);

	/**
		通过entity的ID尝试寻找它的实例
	*/
	static PyObject* tryGetEntity(COMPONENT_ID componentID, ENTITY_ID entityID);

	/**
		设置entityCall的__getEntityFunc函数地址
	*/
	static void setGetEntityFunc(GetEntityFunc func) {
		__getEntityFunc = func;
	};

	/**
		加载相关描述
	*/
	static bool loadAllEntityScriptModules(std::string entitiesPath,
		std::vector<PyTypeObject*>& scriptBaseTypes);

	static bool loadAllComponentScriptModules(std::string entitiesPath,
		std::vector<PyTypeObject*>& scriptBaseTypes);

	/**
		热更 Entity/Component 主脚本之前，先刷新当前组件目录下已经加载过的普通依赖模块。
		例如 cell 进程只扫描 cell 目录，base 进程只扫描 base 目录；interfaces 下的 mixin 会在
		Avatar/Monster 等主脚本重新导入前先 reload，避免 class 继承链继续引用旧的 interface 类。
		注意：这里只 reload sys.modules 中已存在的模块，不主动 import 未加载模块，避免触发跨组件脚本副作用。
		同时会根据文件版本戳过滤，只有脚本文件变化的模块才会真正 reload，并在日志中打印变更文件。
	*/
	static bool reloadDependencyScriptModules(std::string entitiesPath);

	static bool loadAllDefDescriptions(const std::string& moduleName,
		XML* defxml,
		TiXmlNode* defNode,
		ScriptDefModule* pScriptModule);

	static bool loadDefPropertys(const std::string& moduleName,
		XML* xml,
		TiXmlNode* defPropertyNode,
		ScriptDefModule* pScriptModule);

	static bool calcDefPropertyUType(const std::string& moduleName,
		const std::string& name, int iUtype, ScriptDefModule* pScriptModule, ENTITY_PROPERTY_UID& outUtype);

	static bool loadDefCellMethods(const std::string& moduleName,
		XML* xml,
		TiXmlNode* defMethodNode,
		ScriptDefModule* pScriptModule);

	static bool loadDefBaseMethods(const std::string& moduleName,
		XML* xml,
		TiXmlNode* defMethodNode,
		ScriptDefModule* pScriptModule);

	static bool loadDefClientMethods(const std::string& moduleName,
		XML* xml,
		TiXmlNode* defMethodNode,
		ScriptDefModule* pScriptModule);

	static bool loadInterfaces(const std::string& defFilePath,
		const std::string& moduleName,
		XML* defxml,
		TiXmlNode* defNode,
		ScriptDefModule* pScriptModule, bool ignoreComponents = false);

	static bool loadComponents(const std::string& defFilePath,
		const std::string& moduleName,
		XML* defxml,
		TiXmlNode* defNode,
		ScriptDefModule* pScriptModule);

	static PropertyDescription* addComponentProperty(ENTITY_PROPERTY_UID utype,
		const std::string& componentTypeName,
		const std::string& componentName,
		uint32 flags,
		bool isPersistent,
		bool isIdentifier,
		std::string indexType,
		uint32 databaseLength,
		const std::string& defaultStr,
		DETAIL_TYPE detailLevel,
		ScriptDefModule* pScriptModule,
		ScriptDefModule* pCompScriptDefModule);

	static bool loadParentClass(const std::string& defFilePath,
		const std::string& moduleName,
		XML* defxml,
		TiXmlNode* defNode,
		ScriptDefModule* pScriptModule);

	static bool loadDefInfo(const std::string& defFilePath,
		const std::string& moduleName,
		XML* defxml,
		TiXmlNode* defNode,
		ScriptDefModule* pScriptModule);

	static bool loadDetailLevelInfo(const std::string& defFilePath,
		const std::string& moduleName,
		XML* defxml,
		TiXmlNode* defNode,
		ScriptDefModule* pScriptModule);

	static bool loadVolatileInfo(const std::string& defFilePath,
		const std::string& moduleName,
		XML* defxml,
		TiXmlNode* defNode,
		ScriptDefModule* pScriptModule);

	static PyObject* loadScriptModule(std::string moduleName);

	/**
		是否加载这个脚本模块
	*/
	static bool isLoadScriptModule(ScriptDefModule* pScriptModule);

	/**
		根据当前组件类别设置是否有cell 或者base
	*/
	static void setScriptModuleHasComponentEntity(ScriptDefModule* pScriptModule, bool has);

	/**
		检查脚本模块中被定义的方法是否存在
	*/
	static bool checkDefMethod(ScriptDefModule* pScriptModule, PyObject* moduleObj,
		const std::string& moduleName);

	/**
		检查脚本模块中被定义的属性是否合法
	*/
	static bool validDefPropertyName(const std::string& name);

	/**
		通过标记来寻找到对应的脚本模块对象
	*/
	static ScriptDefModule* findScriptModule(ENTITY_SCRIPT_UID utype, bool notFoundOutErr = true);
	static ScriptDefModule* findScriptModule(const char* scriptName, bool notFoundOutErr = true);
	static ScriptDefModule* findOldScriptModule(const char* scriptName, bool notFoundOutErr = true);

	static bool installScript(PyObject* mod);
	static bool uninstallScript();

	static const SCRIPT_MODULES& getScriptModules() {
		return EntityDef::__scriptModules;
	}

	static KBE_MD5& md5(){ return __md5; }

	static bool initializeWatcher();

	static void entitydefAliasID(bool v)
	{
		__entitydefAliasID = v;
	}

	static bool entitydefAliasID()
	{
		return __entitydefAliasID;
	}

	static void entityAliasID(bool v)
	{
		__entityAliasID = v;
	}

	static bool entityAliasID()
	{
		return __entityAliasID;
	}

	static bool scriptModuleAliasID()
	{
		return __entitydefAliasID && __scriptModules.size() <= 255;
	}

	struct Context
	{
		Context()
		{
			currEntityID = 0;
			currClientappID = 0;
			currComponentType = UNKNOWN_COMPONENT_TYPE;
		}

		ENTITY_ID currEntityID;
		COMPONENT_TYPE currComponentType;
		int32 currClientappID;
	};

	static Context& context() {
		return __context;
	}

	static ScriptDefModule* registerNewScriptDefModule(const std::string& moduleName);
	static MethodDescription* createMethodDescription(ScriptDefModule* pScriptModule, ENTITY_METHOD_UID utype, COMPONENT_ID domain, const std::string& name, MethodDescription::EXPOSED_TYPE exposedType);

	static bool isReload();

	static std::vector<PyTypeObject*> getScriptBaseTypes() { return __scriptBaseTypes;  }

	/**
		是否是继承引擎底层允许的基础类的派生类
	*/
	static std::string isSubClass(PyObject* pyClass);

private:
	static SCRIPT_MODULES __scriptModules;										// 所有的扩展脚本模块都存储在这里
	static SCRIPT_MODULES __oldScriptModules;									// reload时旧的模块会放到这里用于判断

	static SCRIPT_MODULE_UID_MAP __scriptTypeMappingUType;						// 脚本类别映射utype
	static SCRIPT_MODULE_UID_MAP __oldScriptTypeMappingUType;					// reload时旧的脚本类别映射utype

	static COMPONENT_TYPE __loadComponentType;									// 所需关系的组件类别的相关数据
	static std::vector<PyTypeObject*> __scriptBaseTypes;
	static std::string __entitiesPath;

	static KBE_MD5 __md5;														// defs-md5

	static bool _isInit;

	static bool __entityAliasID;												// 优化EntityID，view范围内小于255个EntityID, 传输到client时使用1字节伪ID
	static bool __entitydefAliasID;												// 优化entity属性和方法广播时占用的带宽，entity客户端属性或者客户端不超过255个时， 方法uid和属性uid传输到client时使用1字节别名ID

	static GetEntityFunc __getEntityFunc;										// 获得一个entity的实体的函数地址

	// 设置当前操作的一些上下文
	static Context __context;
};

}

#ifdef CODE_INLINE
#include "entitydef.inl"
#endif
#endif // KBE_ENTITYDEF_H
