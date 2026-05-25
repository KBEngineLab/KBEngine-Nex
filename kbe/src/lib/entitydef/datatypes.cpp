// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "datatypes.h"
#include "resmgr/resmgr.h"

namespace KBEngine{

DataTypes::DATATYPE_MAP DataTypes::dataTypes_;
DataTypes::DATATYPE_MAP DataTypes::dataTypesLowerName_;
std::map<std::string, std::string> DataTypes::dataTypeSourceLowerName_;
DataTypes::UID_DATATYPE_MAP DataTypes::uid_dataTypes_;
DataTypes::DATATYPE_ORDERS DataTypes::dataTypesOrders_;

static size_t _g_baseTypeEndIndex = 0;

//-------------------------------------------------------------------------------------
DataTypes::DataTypes()
{
}

//-------------------------------------------------------------------------------------
DataTypes::~DataTypes()
{
	finalise();
}

//-------------------------------------------------------------------------------------
void DataTypes::finalise(void)
{
	//DATATYPE_MAP::iterator iter = dataTypes_.begin();
	//for (; iter != dataTypes_.end(); ++iter)
	//	iter->second->decRef();

	uid_dataTypes_.clear();
	dataTypeSourceLowerName_.clear();
	dataTypesLowerName_.clear();
	dataTypes_.clear();
	dataTypesOrders_.clear();

	_g_baseTypeEndIndex = 0;
}

//-------------------------------------------------------------------------------------
bool DataTypes::validTypeName(const std::string& typeName)
{
	// 不允许前面加_, 因为内部产生的一些临时结构前面使用了_, 避免误判
	if (typeName.size() > 0 && typeName[0] == '_')
		return false;

	return true;
}

//-------------------------------------------------------------------------------------
bool DataTypes::validTypeNameWithPrefix(const std::string& typeName, const std::string& prefix)
{
	if (typeName.size() <= prefix.size() || typeName.compare(0, prefix.size(), prefix) != 0)
		return false;

	char boundary = typeName[prefix.size()];
	return boundary == '_' || (boundary >= 'A' && boundary <= 'Z');
}

//-------------------------------------------------------------------------------------
bool DataTypes::initialize(const std::string& file)
{
	return initialize(file, "", "");
}

//-------------------------------------------------------------------------------------
bool DataTypes::initialize(const std::string& file, const std::string& requiredPrefix, const std::string& sourceName)
{
	// 初始化一些基础类别
	addDataType("UINT8",		new IntType<uint8>);
	addDataType("UINT16",		new IntType<uint16>);
	addDataType("UINT64",		new UInt64Type);
	addDataType("UINT32",		new UInt32Type);

	addDataType("INT8",			new IntType<int8>);
	addDataType("INT16",		new IntType<int16>);
	addDataType("INT32",		new IntType<int32>);
	addDataType("INT64",		new Int64Type);

	addDataType("STRING",		new StringType);
	addDataType("UNICODE",		new UnicodeType);
	addDataType("FLOAT",		new FloatType);
	addDataType("DOUBLE",		new DoubleType);
	addDataType("PYTHON",		new PythonType);
	addDataType("PY_DICT",		new PyDictType);
	addDataType("PY_TUPLE",		new PyTupleType);
	addDataType("PY_LIST",		new PyListType);
	addDataType("ENTITYCALL",	new EntityCallType);
	addDataType("BLOB",			new BlobType);

	addDataType("VECTOR2",		new Vector2Type);
	addDataType("VECTOR3",		new Vector3Type);
	addDataType("VECTOR4",		new Vector4Type);

	_g_baseTypeEndIndex = dataTypesOrders_.size();
	return loadTypes(file, requiredPrefix, sourceName);
}

//-------------------------------------------------------------------------------------
std::vector< std::string > DataTypes::getBaseTypeNames()
{
	std::vector< std::string > ret;
	ret.assign(dataTypesOrders_.begin(), dataTypesOrders_.begin() + _g_baseTypeEndIndex);
	return ret;
}

//-------------------------------------------------------------------------------------
bool DataTypes::loadTypes(const std::string& file)
{
	return loadTypes(file, "", "");
}

//-------------------------------------------------------------------------------------
bool DataTypes::loadTypes(const std::string& file, const std::string& requiredPrefix, const std::string& sourceName)
{
	// 允许纯脚本定义，则可能没有这个文件
	if (access(file.c_str(), 0) != 0)
		return true;

	SmartPointer<XML> xml(new XML(Resmgr::getSingleton().matchRes(file).c_str()));
	return loadTypes(xml, requiredPrefix, sourceName.empty() ? file : sourceName);
}

//-------------------------------------------------------------------------------------
bool DataTypes::loadTypes(SmartPointer<XML>& xml)
{
	return loadTypes(xml, "", "");
}

//-------------------------------------------------------------------------------------
bool DataTypes::loadTypes(SmartPointer<XML>& xml, const std::string& requiredPrefix, const std::string& sourceName)
{
	if (xml == NULL || !xml->isGood())
		return false;

	TiXmlNode* node = xml->getRootNode();

	if(node == NULL)
	{
		// root节点下没有子节点了
		return true;
	}

	XML_FOR_BEGIN(node)
	{
		std::string type = "";
		std::string aliasName = xml->getKey(node);
		TiXmlNode* childNode = node->FirstChild();

		if (!DataTypes::validTypeName(aliasName))
		{
			ERROR_MSG(fmt::format("DataTypes::loadTypes: Not allowed to use the prefix \"_\"! aliasName={}\n",
				aliasName.c_str()));

			return false;
		}

		// 插件 types.xml 会传入 requiredPrefix。这里做强约束：插件自定义 Type 必须在命名上归属该插件，
		// 避免插件类型污染全局 DataTypes 命名空间，也方便 SDK 和日志定位类型来源。
		if (!requiredPrefix.empty() && !DataTypes::validTypeNameWithPrefix(aliasName, requiredPrefix))
		{
			ERROR_MSG(fmt::format("DataTypes::loadTypes: plugin type alias [{}] must be prefix [{}] plus a non-empty suffix, valid examples: [{}ItemData], [{}_ItemData], invalid: [{}], file [{}]\n",
				aliasName, requiredPrefix, requiredPrefix, requiredPrefix, requiredPrefix, sourceName));
			return false;
		}

		if(childNode != NULL)
		{
			type = xml->getValStr(childNode);
			if(type == "FIXED_DICT")
			{
				FixedDictType* fixedDict = new FixedDictType;

				if(fixedDict->initialize(xml.get(), childNode, aliasName))
				{
					// 插件 Type 会比 assets Type 更早注册；这里必须把重复注册当作加载失败处理，
					// 否则后续 EntityDef/SDK 可能在已经报错的类型表上继续运行。
					if (!addDataType(aliasName, fixedDict, sourceName))
					{
						delete fixedDict;
						return false;
					}
				}
				else
				{
					ERROR_MSG(fmt::format("DataTypes::loadTypes: parse FIXED_DICT [{}] error!\n",
						aliasName.c_str()));

					delete fixedDict;
					return false;
				}
			}
			else if(type == "ARRAY")
			{
				FixedArrayType* fixedArray = new FixedArrayType;

				if(fixedArray->initialize(xml.get(), childNode, aliasName))
				{
					// FIXED_ARRAY 同样需要强制失败，避免插件与 assets 共用别名时静默保留旧定义。
					if (!addDataType(aliasName, fixedArray, sourceName))
					{
						delete fixedArray;
						return false;
					}
				}
				else
				{
					ERROR_MSG(fmt::format("DataTypes::loadTypes: parse ARRAY [{}] error!\n",
						aliasName.c_str()));

					delete fixedArray;
					return false;
				}
			}
			else
			{
				DataType* dataType = getDataType(type);
				if(dataType == NULL)
				{
					ERROR_MSG(fmt::format("DataTypes::loadTypes: can't fount type {} by alias[{}].\n",
						type.c_str(), aliasName.c_str()));

					return false;
				}

				// 普通别名不会新建 DataType 对象，失败时不能 delete dataType，只需要中止加载。
				if (!addDataType(aliasName, dataType, sourceName))
					return false;
			}
		}
	}
	XML_FOR_END(node);

	return true;
}

//-------------------------------------------------------------------------------------
bool DataTypes::addDataType(std::string name, DataType* dataType)
{
	return addDataType(name, dataType, "");
}

//-------------------------------------------------------------------------------------
bool DataTypes::addDataType(std::string name, DataType* dataType, const std::string& sourceName)
{
	std::string lowername = name;
	std::transform(lowername.begin(), lowername.end(), lowername.begin(), tolower);

	DATATYPE_MAP::iterator iter = dataTypesLowerName_.find(lowername);
	if (iter != dataTypesLowerName_.end())
	{
		std::map<std::string, std::string>::iterator sourceIter = dataTypeSourceLowerName_.find(lowername);
		std::string oldSource = sourceIter == dataTypeSourceLowerName_.end() || sourceIter->second.empty() ? "unknown" : sourceIter->second;
		std::string newSource = sourceName.empty() ? "unknown" : sourceName;
		ERROR_MSG(fmt::format("DataTypes::addDataType(name): type [{}] already exists, oldSource=[{}], newSource=[{}].\n",
			name, oldSource, newSource));
		return false;
	}

	dataTypesOrders_.push_back(name);
	dataType->aliasName(name);
	dataTypes_[name] = dataType;
	dataTypesLowerName_[lowername] = dataType;
	dataTypeSourceLowerName_[lowername] = sourceName;
	uid_dataTypes_[dataType->id()] = dataType;

	//dataType->incRef();

	if(g_debugEntity)
	{
		DEBUG_MSG(fmt::format("DataTypes::addDataType(name): {:p} name={}, aliasName={}, uid={}.\n",
			(void*)dataType, name, dataType->aliasName(), dataType->id()));
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool DataTypes::addDataType(DATATYPE_UID uid, DataType* dataType)
{
	UID_DATATYPE_MAP::iterator iter = uid_dataTypes_.find(uid);
	if (iter != uid_dataTypes_.end())
	{
		ERROR_MSG(fmt::format("DataTypes(uid)::addDataType: utype {} exist.\n", uid));
		return false;
	}

	uid_dataTypes_[uid] = dataType;

	if(g_debugEntity)
	{
		DEBUG_MSG(fmt::format("DataTypes::addDataType(uid): {:p} aliasName={}, uid={}.\n",
			(void*)dataType, dataType->aliasName(), uid));
	}

	return true;
}

//-------------------------------------------------------------------------------------
void DataTypes::delDataType(std::string name)
{
	DATATYPE_MAP::iterator iter = dataTypes_.find(name);
	if (iter == dataTypes_.end())
	{
		ERROR_MSG(fmt::format("DataTypes::delDataType:not found type {}.\n", name.c_str()));
	}
	else
	{
		uid_dataTypes_.erase(iter->second->id());
		iter->second->decRef();
		dataTypes_.erase(iter);

		std::string lowername = name;
		std::transform(lowername.begin(), lowername.end(), lowername.begin(), tolower);
		dataTypesLowerName_.erase(lowername);
		dataTypeSourceLowerName_.erase(lowername);
	}
}

//-------------------------------------------------------------------------------------
DataType* DataTypes::getDataType(std::string name, bool notFoundOutError)
{
	DATATYPE_MAP::iterator iter = dataTypes_.find(name);
	if (iter != dataTypes_.end())
		return iter->second.get();

	if (notFoundOutError)
	{
		ERROR_MSG(fmt::format("DataTypes::getDataType:not found type {}.\n", name.c_str()));
	}

	return NULL;
}

//-------------------------------------------------------------------------------------
DataType* DataTypes::getDataType(const char* name, bool notFoundOutError)
{
	DATATYPE_MAP::iterator iter = dataTypes_.find(name);
	if (iter != dataTypes_.end())
		return iter->second.get();

	if (notFoundOutError)
	{
		ERROR_MSG(fmt::format("DataTypes::getDataType:not found type {}.\n", name));
	}

	return NULL;
}

//-------------------------------------------------------------------------------------
DataType* DataTypes::getDataType(DATATYPE_UID uid)
{
	UID_DATATYPE_MAP::iterator iter = uid_dataTypes_.find(uid);
	if (iter != uid_dataTypes_.end())
		return iter->second;

	ERROR_MSG(fmt::format("DataTypes::getDataType:not found type {}.\n", uid));
	return NULL;
}

//-------------------------------------------------------------------------------------

}
