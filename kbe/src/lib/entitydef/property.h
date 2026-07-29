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


#ifndef KBENGINE_DEF_PROPERTY_H
#define KBENGINE_DEF_PROPERTY_H

#include "common/common.h"
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4910)
#pragma warning (disable : 4251)
#endif

#include "fixedarray.h"
#include "fixeddict.h"
#include "datatype.h"
#include "common/refcountable.h"
#include "common/memorystream.h"


namespace KBEngine{

class RefCountable;
class PropertyDescription : public RefCountable
{
public:	
	PropertyDescription(ENTITY_PROPERTY_UID utype, 
		std::string dataTypeName, 
		std::string name, 
		uint32 flags, 
		bool isPersistent, 
		DataType* dataType, 
		bool isIdentifier, 
		std::string indexType,
		uint32 databaseLength, 
		std::string defaultStr, 
		DETAIL_TYPE detailLevel);

	virtual ~PropertyDescription();
	
	/** 
		获取这个属性的detailLevel 
	*/
	INLINE int8 getDetailLevel(void) const;
	
	/** 
		是否是一个保存到数据库中的属性 
	*/
	INLINE bool isPersistent(void) const;
	
	/** 
		获取这个属性的数据类别 
	*/
	INLINE DataType* getDataType(void) const;
	
	/** 
		获取属性的标志 cell_public等 
	*/
	INLINE uint32 getFlags(void) const;
	
	/** 
		获取属性名称 
	*/
	INLINE const char* getName(void) const;
	
	/** 
		获取字符串数据类别名 UINT32, BAG..
	*/
	INLINE const char* getDataTypeName(void) const;
	
	/** 
		获取初始值字符串
	*/
	INLINE const char* getDefaultValStr(void) const;

	/**
		获取仅供工具和数据库使用的属性说明，该元数据不参与网络协议MD5。
		Gets the property description used by tools and databases; this metadata does not participate in the network protocol MD5.
	*/
	INLINE const char* getDescriptionStr(void) const;

	/** 
		属性的数字类别， 用于网络上传输识别 
	*/
	INLINE ENTITY_PROPERTY_UID getUType(void) const;
	
	/** 
		获取属性索引类别
	*/
	INLINE const char* indexType(void) const;

	/** 
		别名id， 当暴露的方法或者广播的属性总个数小于255时
		我们不使用utype而使用1字节的aliasID来传输
	*/
	INLINE int16 aliasID() const;
	INLINE uint8 aliasIDAsUint8() const;
	INLINE void aliasID(int16 v);

	/** 
		设置这个属性为索引键 
	*/
	INLINE void setIdentifier(bool isIdentifier);
	
	/** 
		设置这个属性在数据库中的长度 
	*/
	INLINE void setDatabaseLength(uint32 databaseLength);
	INLINE uint32 getDatabaseLength() const;

	/** 
		获取这个属性描述在def文件中被定义的默认值 
	*/
	PyObject* newDefaultVal(void);
	
	/** 
		获得属性描述的总数量 
	*/
	static uint32 getDescriptionCount(void){ return propertyDescriptionCount_; }
	static void resetDescriptionCount(void){ propertyDescriptionCount_ = 0; }

	/** 
		根据类型产生一个描述实例 
	*/
	static PropertyDescription* createDescription(ENTITY_PROPERTY_UID utype, 
		std::string& dataTypeName, 
		std::string& name,
		uint32 flags, 
		bool isPersistent, 
		DataType* dataType, 
		bool isIdentifier, 
		std::string indexType,
		uint32 databaseLength,
		std::string& defaultStr, 
		DETAIL_TYPE detailLevel,
		const std::string& descriptionStr = std::string());
	
	/** 
		脚本请求设置这个属性的值 
	*/
	virtual PyObject* onSetValue(PyObject* parentObj, PyObject* value);	

	virtual void addToStream(MemoryStream* mstream, PyObject* pyValue);
	virtual PyObject* createFromStream(MemoryStream* mstream);
	virtual PyObject* parseDefaultStr(const std::string& defaultVal);

	virtual void addPersistentToStream(MemoryStream* mstream, PyObject* pyValue);
	virtual PyObject* createFromPersistentStream(MemoryStream* mstream);

	INLINE bool hasCell(void) const;
	INLINE bool hasBase(void) const;
	INLINE bool hasClient(void) const;
	
protected:	
	static uint32				propertyDescriptionCount_;						// 所有的属性描述的数量	
	std::string					name_;											// 这个属性的名称
	std::string					dataTypeName_;									// 这个属性的字符串数据类别名
	uint32						flags_;											// 这个属性的一些标志  比如 cell_public
	bool						isPersistent_;									// 是否是一个存储到数据库的属性
	DataType*					dataType_;										// 这个属性的数据类别
	bool						isIdentifier_;									// 是否是一个索引键
	uint32						databaseLength_;								// 这个属性在数据库中的长度
	ENTITY_PROPERTY_UID			utype_;											// 这个属性的数字类别， 用于网络上传输识别
	std::string					defaultValStr_;									// 这个属性的默认值
	DETAIL_TYPE					detailLevel_;									// 这个属性的lod详情级别 看common中的:属性的lod广播级别范围的定义
	int16						aliasID_;										// 别名id， 当暴露的方法或者广播的属性总个数小于255时， 我们不使用utype而使用1字节的aliasID来传输
	std::string					indexType_;										// 属性的索引类别，UNIQUE, INDEX，分别对应无设置、唯一索引、普通索引
	// 属性说明不参与协议摘要，使数据库注释可以独立维护而不影响客户端兼容性。
	// Property descriptions stay outside the protocol digest so database comments can evolve without affecting client compatibility.
	std::string					descriptionStr_;
};

class FixedDictDescription : public PropertyDescription
{
public:	
	FixedDictDescription(ENTITY_PROPERTY_UID utype, 
		std::string dataTypeName,
		std::string name, 
		uint32 flags, 
		bool isPersistent, 
		DataType* dataType, 
		bool isIdentifier, 
		std::string indexType,
		uint32 databaseLength, 
		std::string defaultStr, 
		DETAIL_TYPE detailLevel);

	virtual ~FixedDictDescription();
	
	/** 
		脚本请求设置这个属性的值 
	*/
	PyObject* onSetValue(PyObject* parentObj, PyObject* value);	

	virtual void addPersistentToStream(MemoryStream* mstream, PyObject* pyValue);
	virtual PyObject* createFromPersistentStream(MemoryStream* mstream);

	typedef std::vector<std::pair<std::string, KBEShared_ptr<PropertyDescription> > > CHILD_PROPERTYS;
	
protected:
	CHILD_PROPERTYS childPropertys_;
};

class ArrayDescription : public PropertyDescription
{
public:	
	ArrayDescription(ENTITY_PROPERTY_UID utype, 
		std::string dataTypeName, 
		std::string name, 
		uint32 flags, 
		bool isPersistent, 
		DataType* dataType, 
		bool isIdentifier,
		std::string indexType,
		uint32 databaseLength, 
		std::string defaultStr, 
		DETAIL_TYPE detailLevel);

	virtual ~ArrayDescription();
	
	/** 
		脚本请求设置这个属性的值 
	*/
	PyObject* onSetValue(PyObject* parentObj, PyObject* value);

	virtual void addPersistentToStream(MemoryStream* mstream, PyObject* pyValue);
	virtual PyObject* createFromPersistentStream(MemoryStream* mstream);
	
protected:	
};

class VectorDescription : public PropertyDescription
{
public:	
	VectorDescription(ENTITY_PROPERTY_UID utype, 
		std::string dataTypeName, 
		std::string name, 
		uint32 flags, 
		bool isPersistent, 
		DataType* dataType, 
		bool isIdentifier, 
		std::string indexType,
		uint32 databaseLength, 
		std::string defaultStr, 
		DETAIL_TYPE detailLevel, 
		uint8 elemCount);

	virtual ~VectorDescription();
	
	/** 
		脚本请求设置这个属性的值 
	*/
	PyObject* onSetValue(PyObject* parentObj, PyObject* value);
	
protected:	
	uint8 elemCount_;
};

}

#ifdef CODE_INLINE
#include "property.inl"
#endif
#endif // KBENGINE_DEF_PROPERTY_H

