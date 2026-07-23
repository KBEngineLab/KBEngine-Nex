// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "KBECommon.h"
// #include "KBVar.h"
#include  "KBVariant.h"


/*
	entitydef所支持的基本数据类型
	改模块中的类抽象出了所有的支持类型并提供了这些类型的数据序列化成二进制数据与反序列化操作（主要用于网络通讯的打包与解包）
*/
namespace KBEngine
{

class Bundle;
class MemoryStream;

class DATATYPE_BASE
{
public:
		DATATYPE_BASE()
		{
		}
		
		virtual ~DATATYPE_BASE()
		{
		}

		virtual void bind()
		{
		}

		virtual KBVariant* createFromStream(MemoryStream& stream)
		{
			return nullptr;
		}
		
		virtual void addToStream(Bundle& stream, KBVariant& v)
		{
		}
		
		virtual KBVariant* parseDefaultValStr(const KBString& v)
		{
			return nullptr;
		}
		
		virtual bool isSameType(KBVariant& v)
		{
			return false;
		}
		
		virtual KBString c_str() const {
			return KBTEXT("UNKNOWN");
		}

protected:

};

class DATATYPE_INT8 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("INT8");
	}
};

class DATATYPE_INT16 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("INT16");
	}
};

class DATATYPE_INT32 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(KBEngine::Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("INT32");
	}
};

class DATATYPE_INT64 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("INT64");
	}
};

class DATATYPE_UINT8 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("UINT8");
	}
};

class DATATYPE_UINT16 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("UINT6");
	}
};

class DATATYPE_UINT32 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("UINT32");
	}
};

class DATATYPE_UINT64 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("UINT64");
	}
};

class DATATYPE_FLOAT : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("FLOAT");
	}
};

class DATATYPE_DOUBLE : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("DOUBLE");
	}
};

class DATATYPE_STRING : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("STRING");
	}
};

class DATATYPE_VECTOR2 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("VECTOR2");
	}
};

class DATATYPE_VECTOR3 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("VECTOR3");
	}
};

class DATATYPE_VECTOR4 : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("VECTOR4");
	}
};

class DATATYPE_PYTHON : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("PYTHON");
	}
};

class DATATYPE_UNICODE : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("UNICODE|STRING");
	}
};

class DATATYPE_ENTITYCALL : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("ENTITYCALL");
	}
};

class DATATYPE_BLOB : public DATATYPE_BASE
{
public:
	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("BLOB|KBArray<uint8>");
	}
};

class DATATYPE_ARRAY : public DATATYPE_BASE
{
public:
	DATATYPE_ARRAY() :
		vtype(NULL),
		tmpset_uitemtype(-1)
	{

	}

	virtual void bind() override;

	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("KB_ARRAY");
	}

public:
	DATATYPE_BASE* vtype;
	int tmpset_uitemtype;
};

class DATATYPE_FIXED_DICT : public DATATYPE_BASE
{
public:
	DATATYPE_FIXED_DICT() :
		implementedBy(),
		dicttype(),
		dicttype_map()
	{

	}

	virtual void bind() override;

	virtual KBVariant* createFromStream(MemoryStream& stream) override;
	virtual void addToStream(Bundle& stream, KBVariant& v) override;

	virtual KBVariant* parseDefaultValStr(const KBString& v) override;

	virtual bool isSameType(KBVariant& v) override;

	virtual KBString c_str() const {
		return KBTEXT("KB_FIXED_DICT");
	}

public:
	KBString implementedBy;
	KBMap<KBString, DATATYPE_BASE*> dicttype;
	KBMap<KBString, uint16> dicttype_map;
};

}
