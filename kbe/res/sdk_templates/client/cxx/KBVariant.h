#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <type_traits>
#include <sstream>
#include <cstring>

#include "KBEBasicTypes.h"

namespace EKBVarTypes
{
    const int32_t Empty      = 0;
    const int32_t Ansichar   = 1;
    const int32_t Bool       = 2;
    const int32_t ByteArray  = 3;
    const int32_t Double     = 4;
    const int32_t Float      = 5;
    const int32_t Int8       = 6;
    const int32_t Int16      = 7;
    const int32_t Int32      = 8;
    const int32_t Int64      = 9;
    const int32_t String     = 10;
    const int32_t Widechar   = 11;
    const int32_t UInt8      = 12;
    const int32_t UInt16     = 13;
    const int32_t UInt32     = 14;
    const int32_t UInt64     = 15;
    const int32_t Vector     = 16;
    const int32_t Vector2d   = 17;
    const int32_t Vector4    = 18;
    const int32_t KBVarArray = 19;
    const int32_t KBVarMap   = 20;
    const int32_t KBVarBytes = 21;
};

namespace KBEngine {

class KBVariant {
public:
    using KBVarArray = KBArray<KBVariant>;
    using KBVarMap   = KBMap<std::string, KBVariant>;
    using KBVarBytes = KBArray<uint8_t>;
    using KBStringPtr = std::shared_ptr<KBString>;

    enum class Type : uint8_t {
        Null = 0,
        Ansichar,
        Bool,
        ByteArray,
        Double,
        Float,
        Int8,
        Int16,
        Int32,
        Int64,
        UInt8,
        UInt16,
        UInt32,
        UInt64,
        String,
        KBString,
        Widechar,
        KBVarArray,
        KBVarMap,
        KBVarBytes,
        KBVector2f,
        KBVector3f,
        KBVector4f,
        Any = 100
    };

private:
    Type type_ = Type::Null;

    union {
        int8_t    v_int8;
        int16_t   v_int16;
        int32_t   v_int32;
        int64_t   v_int64;
        uint8_t   v_uint8;
        uint16_t  v_uint16;
        uint32_t  v_uint32;
        uint64_t  v_uint64;
        float     v_float;
        double    v_double;
        bool      v_bool;
        char      v_ansichar;
        wchar_t   v_widechar;
    } data_;

    std::shared_ptr<void> ptr_;

    template<typename T>
    T* getPtr() const { return reinterpret_cast<T*>(ptr_.get()); }

public:
    KBVariant() : type_(Type::Null) {}
    ~KBVariant() = default;

    // Copy / Move
    KBVariant(const KBVariant& o) { *this = o; }
    KBVariant& operator=(const KBVariant& o) {
        if (this == &o) return *this;
        type_ = o.type_;
        data_ = o.data_;
        ptr_.reset();
        if (o.ptr_) clonePtr(o);
        return *this;
    }

    KBVariant(KBVariant&& o) noexcept : type_(o.type_), data_(o.data_), ptr_(std::move(o.ptr_)) {
        o.type_ = Type::Null;
    }
    KBVariant& operator=(KBVariant&& o) noexcept {
        if(this != &o) {
            type_ = o.type_;
            data_ = o.data_;
            ptr_ = std::move(o.ptr_);
            o.type_ = Type::Null;
        }
        return *this;
    }

    // ================= Constructors =================
    KBVariant(int8_t v)    { setInt8(v); }
    KBVariant(int16_t v)   { setInt16(v); }
    KBVariant(int32_t v)   { setInt32(v); }
    KBVariant(int64_t v)   { setInt64(v); }
    KBVariant(uint8_t v)   { setUInt8(v); }
    KBVariant(uint16_t v)  { setUInt16(v); }
    KBVariant(uint32_t v)  { setUInt32(v); }
    KBVariant(uint64_t v)  { setUInt64(v); }
    KBVariant(float v)     { setFloat(v); }
    KBVariant(double v)    { setDouble(v); }
    KBVariant(bool v)      { setBool(v); }
    KBVariant(char v)      { setAnsichar(v); }
    KBVariant(wchar_t v)   { setWidechar(v); }
    KBVariant(const std::string& s) { setString(s); }
    KBVariant(const KBVarArray& a) { setKBVarArray(a); }
    KBVariant(const KBVarMap& m)   { setKBVarMap(m); }
    KBVariant(const KBVarBytes& b) { setKBVarBytes(b); }
    KBVariant(const KBString& s) { setKBString(s); }
    KBVariant(const KBVector2f& v) { setKBVector2f(v); }
    KBVariant(const KBVector3f& v) { setKBVector3f(v); }
    KBVariant(const KBVector4f& v) { setKBVector4f(v); }



    // ================= Setters =================
    void setInt8(int8_t v)       { type_ = Type::Int8; data_.v_int8 = v; ptr_.reset(); }
    void setInt16(int16_t v)     { type_ = Type::Int16; data_.v_int16 = v; ptr_.reset(); }
    void setInt32(int32_t v)     { type_ = Type::Int32; data_.v_int32 = v; ptr_.reset(); }
    void setInt64(int64_t v)     { type_ = Type::Int64; data_.v_int64 = v; ptr_.reset(); }
    void setUInt8(uint8_t v)     { type_ = Type::UInt8; data_.v_uint8 = v; ptr_.reset(); }
    void setUInt16(uint16_t v)   { type_ = Type::UInt16; data_.v_uint16 = v; ptr_.reset(); }
    void setUInt32(uint32_t v)   { type_ = Type::UInt32; data_.v_uint32 = v; ptr_.reset(); }
    void setUInt64(uint64_t v)   { type_ = Type::UInt64; data_.v_uint64 = v; ptr_.reset(); }
    void setFloat(float v)       { type_ = Type::Float; data_.v_float = v; ptr_.reset(); }
    void setDouble(double v)     { type_ = Type::Double; data_.v_double = v; ptr_.reset(); }
    void setBool(bool v)         { type_ = Type::Bool; data_.v_bool = v; ptr_.reset(); }
    void setAnsichar(char v)     { type_ = Type::Ansichar; data_.v_ansichar = v; ptr_.reset(); }
    void setWidechar(wchar_t v)  { type_ = Type::Widechar; data_.v_widechar = v; ptr_.reset(); }

    void setString(const std::string& s) { type_ = Type::String; ptr_ = std::make_shared<std::string>(s); }
    void setKBVarArray(const KBVarArray& a) { type_ = Type::KBVarArray; ptr_ = std::make_shared<KBVarArray>(a); }
    void setKBVarMap(const KBVarMap& m)     { type_ = Type::KBVarMap; ptr_ = std::make_shared<KBVarMap>(m); }
    void setKBVarBytes(const KBVarBytes& b) { type_ = Type::KBVarBytes; ptr_ = std::make_shared<KBVarBytes>(b); }

    void setKBString(const KBString& s)
    {
        type_ = Type::KBString;
        ptr_ = std::make_shared<KBString>(s);
    }

    void setKBVector2f(const KBVector2f& v)
    {
        type_ = Type::KBVector2f;
        ptr_ = std::make_shared<KBVector2f>(v);
    }

    void setKBVector3f(const KBVector3f& v)
    {
        type_ = Type::KBVector3f;
        ptr_ = std::make_shared<KBVector3f>(v);
    }

    void setKBVector4f(const KBVector4f& v)
    {
        type_ = Type::KBVector4f;
        ptr_ = std::make_shared<KBVector4f>(v);
    }



    // ================= Getters =================
    int8_t       asInt8() const   { return data_.v_int8; }
    int16_t      asInt16() const  { return data_.v_int16; }
    int32_t      asInt32() const  { return data_.v_int32; }
    int64_t      asInt64() const  { return data_.v_int64; }
    uint8_t      asUInt8() const  { return data_.v_uint8; }
    uint16_t     asUInt16() const { return data_.v_uint16; }
    uint32_t     asUInt32() const { return data_.v_uint32; }
    uint64_t     asUInt64() const { return data_.v_uint64; }
    float        asFloat() const  { return data_.v_float; }
    double       asDouble() const { return data_.v_double; }
    bool         asBool() const   { return data_.v_bool; }
    char         asAnsichar() const { return data_.v_ansichar; }
    wchar_t      asWidechar() const { return data_.v_widechar; }
    const std::string* asString() const { return getPtr<std::string>(); }
    KBVarArray*  asKBVarArray() { return getPtr<KBVarArray>(); }
    const KBVarArray* asKBVarArray() const { return getPtr<KBVarArray>(); }
    KBVarMap*    asKBVarMap() { return getPtr<KBVarMap>(); }
    const KBVarMap* asKBVarMap() const { return getPtr<KBVarMap>(); }
    KBVarBytes*  asKBVarBytes() { return getPtr<KBVarBytes>(); }
    const KBVarBytes* asKBVarBytes() const { return getPtr<KBVarBytes>(); }
    KBString* asKBString() { return getPtr<KBString>(); }
    const KBString* asKBString() const { return getPtr<KBString>(); }
    KBVector2f* asKBVector2f() { return getPtr<KBVector2f>(); }
    const KBVector2f* asKBVector2f() const { return getPtr<KBVector2f>(); }

    KBVector3f* asKBVector3f() { return getPtr<KBVector3f>(); }
    const KBVector3f* asKBVector3f() const { return getPtr<KBVector3f>(); }

    KBVector4f* asKBVector4f() { return getPtr<KBVector4f>(); }
    const KBVector4f* asKBVector4f() const { return getPtr<KBVector4f>(); }


    Type type() const { return type_; }

    // ================= Conversion operators =================
    operator int8_t() const { return asInt8(); }
    operator int16_t() const { return asInt16(); }
    operator int32_t() const { return asInt32(); }
    operator int64_t() const { return asInt64(); }
    operator uint8_t() const { return asUInt8(); }
    operator uint16_t() const { return asUInt16(); }
    operator uint32_t() const { return asUInt32(); }
    operator uint64_t() const { return asUInt64(); }
    operator float() const { return asFloat(); }
    operator double() const { return asDouble(); }
    operator bool() const { return asBool(); }
    operator char() const { return asAnsichar(); }
    operator wchar_t() const { return asWidechar(); }
    operator KBVarBytes() const { return *asKBVarBytes(); }
    operator KBString() const { return *asKBString(); }
    operator KBVector2f() const { return *asKBVector2f(); }
    operator KBVector3f() const { return *asKBVector3f(); }
    operator KBVector4f() const { return *asKBVector4f(); }



    // ================= MakeXXX helpers =================
    static KBVariant MakeInt8(int8_t v)     { return KBVariant(v); }
    static KBVariant MakeInt16(int16_t v)   { return KBVariant(v); }
    static KBVariant MakeInt32(int32_t v)   { return KBVariant(v); }
    static KBVariant MakeInt64(int64_t v)   { return KBVariant(v); }
    static KBVariant MakeUInt8(uint8_t v)   { return KBVariant(v); }
    static KBVariant MakeUInt16(uint16_t v) { return KBVariant(v); }
    static KBVariant MakeUInt32(uint32_t v) { return KBVariant(v); }
    static KBVariant MakeUInt64(uint64_t v) { return KBVariant(v); }
    static KBVariant MakeFloat(float v)     { return KBVariant(v); }
    static KBVariant MakeDouble(double v)   { return KBVariant(v); }
    static KBVariant MakeBool(bool v)       { return KBVariant(v); }
    static KBVariant MakeAnsichar(char v)   { return KBVariant(v); }
    static KBVariant MakeWidechar(wchar_t v){ return KBVariant(v); }
    static KBVariant MakeString(const std::string& v) { return KBVariant(v); }
    static KBVariant MakeKBVarArray(const KBVarArray& v){ return KBVariant(v); }
    static KBVariant MakeKBVarMap(const KBVarMap& v)   { return KBVariant(v); }
    static KBVariant MakeKBVarBytes(const KBVarBytes& v){ return KBVariant(v); }
    static KBVariant MakeKBString(const KBString& v) { return KBVariant(v); }
    static KBVariant MakeKBVector2f(const KBVector2f& v) { return KBVariant(v); }
    static KBVariant MakeKBVector3f(const KBVector3f& v) { return KBVariant(v); }
    static KBVariant MakeKBVector4f(const KBVector4f& v) { return KBVariant(v); }



private:
    void clonePtr(const KBVariant& o) {
        switch(o.type_) {
            case Type::String:
                ptr_ = std::make_shared<std::string>(*o.getPtr<std::string>());
                break;
            case Type::KBVarArray:
                ptr_ = std::make_shared<KBVarArray>(*o.getPtr<KBVarArray>());
                break;
            case Type::KBVarMap:
                ptr_ = std::make_shared<KBVarMap>(*o.getPtr<KBVarMap>());
                break;
            case Type::KBVarBytes:
                ptr_ = std::make_shared<KBVarBytes>(*o.getPtr<KBVarBytes>());
                break;
            case Type::KBString:
                ptr_ = std::make_shared<KBString>(*o.getPtr<KBString>());
                break;

            case Type::KBVector2f:
                ptr_ = std::make_shared<KBVector2f>(*o.getPtr<KBVector2f>());
                break;
            case Type::KBVector3f:
                ptr_ = std::make_shared<KBVector3f>(*o.getPtr<KBVector3f>());
                break;
            case Type::KBVector4f:
                ptr_ = std::make_shared<KBVector4f>(*o.getPtr<KBVector4f>());
                break;

            default:
                ptr_.reset();
                break;
        }
    }

};

} // namespace KBEngine
