
#include "DataTypes.h"
#include "MemoryStream.h"
#include "EntityDef.h"
#include "KBDebug.h"
#include "Bundle.h"

namespace KBEngine
{

KBVariant* DATATYPE_INT8::createFromStream(MemoryStream& stream)
{
	int8 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_INT8::addToStream(Bundle& stream, KBVariant& v)
{
	int8 val = v;
	stream << val;
}

KBVariant* DATATYPE_INT8::parseDefaultValStr(const KBString& v)
{
	int8 val = (int8)std::stoi(v);
	return new KBVariant(val);
}

bool DATATYPE_INT8::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::Int8;
}

KBVariant* DATATYPE_INT16::createFromStream(MemoryStream& stream)
{
	int16 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_INT16::addToStream(Bundle& stream, KBVariant& v)
{
	int16 val = v;
	stream << val;
}

KBVariant* DATATYPE_INT16::parseDefaultValStr(const KBString& v)
{
	int16 val = (int16)std::stoi(v);
	return new KBVariant(val);
}

bool DATATYPE_INT16::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::Int16;
}

KBVariant* DATATYPE_INT32::createFromStream(MemoryStream& stream)
{
	int32 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_INT32::addToStream(Bundle& stream, KBVariant& v)
{
	int32 val = v;
	stream << val;
}

KBVariant* DATATYPE_INT32::parseDefaultValStr(const KBString& v)
{
	int32 val = (int32)std::stoi(v);
	return new KBVariant(val);
}

bool DATATYPE_INT32::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::Int32;
}

KBVariant* DATATYPE_INT64::createFromStream(MemoryStream& stream)
{
	int64 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_INT64::addToStream(Bundle& stream, KBVariant& v)
{
	int64 val = v;
	stream << val;
}

KBVariant* DATATYPE_INT64::parseDefaultValStr(const KBString& v)
{
	int64 val = (int64)std::stoll(v);
	return new KBVariant(val);
}

bool DATATYPE_INT64::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::Int64;
}

KBVariant* DATATYPE_UINT8::createFromStream(MemoryStream& stream)
{
	uint8 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_UINT8::addToStream(Bundle& stream, KBVariant& v)
{
	uint8 val = v;
	stream << val;
}

KBVariant* DATATYPE_UINT8::parseDefaultValStr(const KBString& v)
{
	uint8 val = (uint8)std::stoi(v);
	return new KBVariant(val);
}

bool DATATYPE_UINT8::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::UInt8;
}

KBVariant* DATATYPE_UINT16::createFromStream(MemoryStream& stream)
{
	uint16 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_UINT16::addToStream(Bundle& stream, KBVariant& v)
{
	uint16 val = v;
	stream << val;
}

KBVariant* DATATYPE_UINT16::parseDefaultValStr(const KBString& v)
{
	uint16 val = (uint16)std::stoi(v);
	return new KBVariant(val);
}

bool DATATYPE_UINT16::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::UInt16;
}

KBVariant* DATATYPE_UINT32::createFromStream(MemoryStream& stream)
{
	uint32 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_UINT32::addToStream(Bundle& stream, KBVariant& v)
{
	uint32 val = v;
	stream << val;
}

KBVariant* DATATYPE_UINT32::parseDefaultValStr(const KBString& v)
{
	uint32 val = (uint32)std::stoi(v);
	return new KBVariant(val);
}

bool DATATYPE_UINT32::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::UInt32;
}

KBVariant* DATATYPE_UINT64::createFromStream(MemoryStream& stream)
{
	uint64 val = 0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_UINT64::addToStream(Bundle& stream, KBVariant& v)
{
	uint64 val = v;
	stream << val;
}

KBVariant* DATATYPE_UINT64::parseDefaultValStr(const KBString& v)
{
	uint64 val = (uint64)std::stoll(v);
	return new KBVariant(val);
}

bool DATATYPE_UINT64::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::UInt64;
}

KBVariant* DATATYPE_FLOAT::createFromStream(MemoryStream& stream)
{
	float val = 0.f;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_FLOAT::addToStream(Bundle& stream, KBVariant& v)
{
	float val = v;
	stream << val;
}

KBVariant* DATATYPE_FLOAT::parseDefaultValStr(const KBString& v)
{
	float val = (float)std::stof(v);
	return new KBVariant(val);
}

bool DATATYPE_FLOAT::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::Float;
}

KBVariant* DATATYPE_DOUBLE::createFromStream(MemoryStream& stream)
{
	double val = 0.0;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_DOUBLE::addToStream(Bundle& stream, KBVariant& v)
{
	double val = v;
	stream << val;
}

KBVariant* DATATYPE_DOUBLE::parseDefaultValStr(const KBString& v)
{
	double val = (double)std::stof(v);
	return new KBVariant(val);
}

bool DATATYPE_DOUBLE::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::Double;
}

KBVariant* DATATYPE_STRING::createFromStream(MemoryStream& stream)
{
	KBString val;
	stream >> val;
	return new KBVariant(val);
}

void DATATYPE_STRING::addToStream(Bundle& stream, KBVariant& v)
{
	KBString val = v;
	stream << val;
}

KBVariant* DATATYPE_STRING::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(v);
}

bool DATATYPE_STRING::isSameType(KBVariant& v)
{

	return v.type() == KBVariant::Type::String || v.type() == KBVariant::Type::Ansichar ;
}

KBVariant* DATATYPE_VECTOR2::createFromStream(MemoryStream& stream)
{
	KBVector2f val;
	stream >> val.x >> val.y;
	return new KBVariant(val);
}

void DATATYPE_VECTOR2::addToStream(Bundle& stream, KBVariant& v)
{
	KBVector2f val = v;
	stream << val.x << val.y;
}

KBVariant* DATATYPE_VECTOR2::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(KBVector2f());
}

bool DATATYPE_VECTOR2::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::KBVector2f;
}

KBVariant* DATATYPE_VECTOR3::createFromStream(MemoryStream& stream)
{
	KBVector3f val;
	stream >> val.x >> val.y >> val.z;
	return new KBVariant(val);
}

void DATATYPE_VECTOR3::addToStream(Bundle& stream, KBVariant& v)
{
	KBVector3f val = v;
	stream << val.x << val.y << val.z;
}

KBVariant* DATATYPE_VECTOR3::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(KBVector3f());
}

bool DATATYPE_VECTOR3::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::KBVector3f;
}

KBVariant* DATATYPE_VECTOR4::createFromStream(MemoryStream& stream)
{
	KBVector4f val;
	stream >> val.x >> val.y >> val.z >> val.w;
	return new KBVariant(val);
}

void DATATYPE_VECTOR4::addToStream(Bundle& stream, KBVariant& v)
{
	KBVector4f val = v;
	stream << val.x << val.y << val.z << val.w;
}

KBVariant* DATATYPE_VECTOR4::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(KBVector4f());
}

bool DATATYPE_VECTOR4::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::KBVector4f;
}

KBVariant* DATATYPE_PYTHON::createFromStream(MemoryStream& stream)
{
	KBArray<uint8> val;
	stream.readBlob(val);
	return new KBVariant(val);
}

void DATATYPE_PYTHON::addToStream(Bundle& stream, KBVariant& v)
{
	KBArray<uint8> val = v;
	stream.appendBlob(val);
}

KBVariant* DATATYPE_PYTHON::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(KBArray<uint8>());
}

bool DATATYPE_PYTHON::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::ByteArray;
}

KBVariant* DATATYPE_UNICODE::createFromStream(MemoryStream& stream)
{
	KBString val;
	stream.readUTF8String(val);
	return new KBVariant(val);
}

void DATATYPE_UNICODE::addToStream(Bundle& stream, KBVariant& v)
{
	KBString val = v;
	stream.appendUTF8String(val);
}

KBVariant* DATATYPE_UNICODE::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(v);
}

bool DATATYPE_UNICODE::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::String || v.type() == KBVariant::Type::Widechar
		|| v.type() == KBVariant::Type::Ansichar;
}

KBVariant* DATATYPE_ENTITYCALL::createFromStream(MemoryStream& stream)
{
	KBArray<uint8> val;
	stream.readBlob(val);
	return new KBVariant(val);
}

void DATATYPE_ENTITYCALL::addToStream(Bundle& stream, KBVariant& v)
{
	KBArray<uint8> val = v;
	stream.appendBlob(val);
}

KBVariant* DATATYPE_ENTITYCALL::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(KBArray<uint8>());
}

bool DATATYPE_ENTITYCALL::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::ByteArray;
}

KBVariant* DATATYPE_BLOB::createFromStream(MemoryStream& stream)
{
	KBArray<uint8> val;
	stream.readBlob(val);
	return new KBVariant(val);
}

void DATATYPE_BLOB::addToStream(Bundle& stream, KBVariant& v)
{
	KBArray<uint8> val = v;
	stream.appendBlob(val);
}

KBVariant* DATATYPE_BLOB::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(KBArray<uint8>());
}

bool DATATYPE_BLOB::isSameType(KBVariant& v)
{
	return v.type() == KBVariant::Type::ByteArray;
}

void DATATYPE_ARRAY::bind()
{
	if (tmpset_uitemtype == -1)
		vtype->bind();
	else
		if (EntityDef::id2datatypes.Contains(tmpset_uitemtype))
			vtype = EntityDef::id2datatypes[tmpset_uitemtype];
}

KBVariant* DATATYPE_ARRAY::createFromStream(MemoryStream& stream)
{
	KBVariant::KBVarArray val;

	uint32 size;
	stream >> size;

	while (size > 0)
	{
		size--;
		val.Add(*vtype->createFromStream(stream));
	};

	return new KBVariant(val);
}

void DATATYPE_ARRAY::addToStream(Bundle& stream, KBVariant& v)
{
	if (v.type() == KBVariant::Type::KBVarArray)
	{
		KBVariant::KBVarArray* val = v.asKBVarArray();
		uint32 size = static_cast<uint32>(val->Num());

		stream << size;

		for (uint32 i = 0; i<size; ++i)
		{
			vtype->addToStream(stream, (*val)[i]);
		}
	}
	else
	{
		KBVariant::KBVarBytes val = v;
		uint32 size = static_cast<uint32>(val.Num());

		stream << size;

		for (uint32 i = 0; i<size; ++i)
		{
			KBVariant tmpv = val[i];
			vtype->addToStream(stream, tmpv);
		}
	}
}

KBVariant* DATATYPE_ARRAY::parseDefaultValStr(const KBString& v)
{
	return new KBVariant(KBVariant::KBVarArray());
}

bool DATATYPE_ARRAY::isSameType(KBVariant& v)
{
	if (v.type() != KBVariant::Type::KBVarArray && v.type() != KBVariant::Type::ByteArray)
		return false;

	if (v.type() == KBVariant::Type::KBVarArray)
	{
		KBVariant::KBVarArray* val = v.asKBVarArray();

		for (uint32 i = 0; i < (uint32)val->Num(); ++i)
		{
			if (!vtype->isSameType((*val)[i]))
				return false;
		}
	}
	else
	{
		KBVariant::KBVarBytes val = v;

		for (uint32 i = 0; i < (uint32)val.Num(); ++i)
		{
			KBVariant tmpv = val[i];
			if (!vtype->isSameType(tmpv))
				return false;
		}
	}

	return true;
}

void DATATYPE_FIXED_DICT::bind()
{
	if (dicttype_map.Num() > 0)
	{
		for (auto& item : dicttype_map)
		{
			if (EntityDef::id2datatypes.Contains(item.second))
				dicttype.Add(item.first, EntityDef::id2datatypes[item.second]);
		}

		dicttype_map.Clear();
	}
	else
	{
		for (auto& item : dicttype)
		{
			item.second->bind();
		}
	}
}

KBVariant* DATATYPE_FIXED_DICT::createFromStream(MemoryStream& stream)
{
	KBVariant::KBVarMap val;

	for (auto& item : dicttype)
	{
		val.Add(item.first, *item.second->createFromStream(stream));
	}

	return new KBVariant(val);
}

void DATATYPE_FIXED_DICT::addToStream(Bundle& stream, KBVariant& v)
{
	KBVariant::KBVarMap* val = v.asKBVarMap();

	for (auto& item : dicttype)
	{
		item.second->addToStream(stream, (*val)[item.first]);
	}
}

KBVariant* DATATYPE_FIXED_DICT::parseDefaultValStr(const KBString& v)
{
	KBVariant::KBVarMap val;

	for (auto& item : dicttype)
	{
		val.Add(item.first, *item.second->parseDefaultValStr(KBString(KBTEXT(""))));
	}

	return new KBVariant(val);
}

bool DATATYPE_FIXED_DICT::isSameType(KBVariant& v)
{
	if (v.type() != KBVariant::Type::KBVarMap)
		return false;

	KBVariant::KBVarMap* val = v.asKBVarMap();  // 获取指针
	// if (!val) return false;                     // 防止空指针

	for (auto& item : dicttype)
	{
		if (!item.second->isSameType((*val)[item.first]))
			return false;
	}

	return true;
}

}