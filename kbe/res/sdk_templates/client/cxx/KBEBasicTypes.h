//
// Created by KBEngineLab on 2025/11/21.
//


#pragma once

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring> // memcpy
#include <locale>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>


#if defined(__UNREAL__) || defined(UE_BUILD_DEBUG) || defined(UE_SERVER) || defined(UE_GAME) || defined(UE_CLIENT) || defined(UE_BUILD_DEVELOPMENT) || defined(UE_BUILD_SHIPPING)
    #define KBE_PLATFORM_UE 1
#elif defined(CC_TARGET_PLATFORM)
    #define KBE_PLATFORM_COCOS 1
#else
    #define KBE_PLATFORM_CPP 1
#endif


// #ifdef _WIN32
// #include <windows.h>
// #endif


using KBCHAR = char;               // 全平台 char
#define KBTEXT(x) x                // 全平台普通字符串
using KBStringBase = std::string;

struct KBString : public KBStringBase
{
    using Base = KBStringBase;
    using Base::Base;

    KBString() {}

    KBString(const char* str) : Base(str ? str : "") {}

    KBString(const std::string& s) : Base(s) {}


    

    // 支持 wchar_t*（如果用户传进来了）——直接转 UTF-8
    KBString(const wchar_t* wstr)
    {
        if (!wstr) return;
        std::string utf8 = WCharToUTF8(wstr);
        this->assign(utf8);
    }

    // wchar_t → UTF8 辅助函数（可选）
    // static std::string WCharToUTF8(const wchar_t* wstr)
    // {
    //     if (!wstr) return "";

    //     int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
    //                                           NULL, 0, NULL, NULL);

    //     std::string result(size_needed, '\0');

    //     WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
    //                         &result[0], size_needed, NULL, NULL);

    //     // 去掉最后一个 '\0'
    //     if (!result.empty() && result.back() == '\0')
    //         result.pop_back();

    //     return result;
    // }

    // static std::string WCharToUTF8(const wchar_t* wstr)
    // {
    //     if (!wstr) return "";

    //     std::wstring ws(wstr);

    //     std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    //     return conv.to_bytes(ws);
    // }

    // 返回 UTF-8 字节长度（不含长度前缀）
    size_t UTF8ByteSize() const
    {
        return this->size(); // KBString 已经存储的是 UTF-8 bytes
    }
    
    static std::string WCharToUTF8(const wchar_t* wstr)
    {
        if (!wstr) return "";

        std::string result;

        while (*wstr)
        {
            wchar_t wc = *wstr++;

            if (wc <= 0x7F)
            {
                result.push_back(static_cast<char>(wc));
            }
            else if (wc <= 0x7FF)
            {
                result.push_back(static_cast<char>(0xC0 | ((wc >> 6) & 0x1F)));
                result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
            }
            else if (wc <= 0xFFFF)
            {
                result.push_back(static_cast<char>(0xE0 | ((wc >> 12) & 0x0F)));
                result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
            }
    #if WCHAR_MAX > 0xFFFF
            else if (wc <= 0x10FFFF)
            {
                result.push_back(static_cast<char>(0xF0 | ((wc >> 18) & 0x07)));
                result.push_back(static_cast<char>(0x80 | ((wc >> 12) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
            }
    #endif
            else
            {
                throw std::runtime_error("Invalid wchar_t value");
            }
        }

        return result;
    }

    // ---- 转 std::string ----
    operator std::string() const {
        return *this; // 本来就是 UTF-8 string
    }

    // ---- AppendInt ----
    void AppendInt(int value)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", value);
        this->append(buf);
        // this->append(std::to_string(value));
    }

    // Append const char*
    void Append(const char* str)
    {
        this->append(str);
    }

    // Append KBString
    void Append(const KBString& other)
    {
        this->append(other);
    }

    // Append wchar_t*（转 UTF8）
    void Append(const wchar_t* wstr)
    {
        this->append(WCharToUTF8(wstr));
    }

    // ---- Printf ----
    template<typename... Args>
    static KBString Printf(const char* fmt, Args... args)
    {
        int size = snprintf(nullptr, 0, fmt, args...) + 1;
        std::string buffer(size, '\0');
        snprintf(&buffer[0], size, fmt, args...);
        buffer.pop_back(); // 去掉 '\0'
        return KBString(buffer);
    }

    // ---- 返回 char 数组（带 '\0'）----
    std::vector<char> GetCharArray() const
    {
        std::vector<char> arr(begin(), end());
        arr.push_back('\0');
        return arr;
    }

    // ---- 比较 const char* ----
    bool operator==(const char* rhs) const
    {
        return rhs && this->compare(rhs) == 0;
    }

    bool operator!=(const char* rhs) const
    {
        return !(*this == rhs);
    }

    // ---- 比较 const wchar_t*（自动转 UTF8）----
    bool operator==(const wchar_t* rhs) const
    {
        return this->compare(WCharToUTF8(rhs)) == 0;
    }

    bool operator!=(const wchar_t* rhs) const
    {
        return !(*this == rhs);
    }

    // 前置 *
    const char* operator*() const
    {
        return this->c_str();
    }

    KBString& operator=(const char* str)
    {
        this->assign(str ? str : "");
        return *this;
    }

    KBString& operator=(const wchar_t* wstr)
    {
        this->assign(WCharToUTF8(wstr));
        return *this;
    }

    
#if defined(KBE_PLATFORM_UE)
    // 添加 FString 构造
    KBString(const FString& fstr)
    {
        // FString::Utf8() 返回 UTF-8 const char*
        this->assign(TCHAR_TO_UTF8(*fstr));
    }

    // 添加 operator= 支持 FString
    KBString& operator=(const FString& fstr)
    {
        this->assign(TCHAR_TO_UTF8(*fstr));
        return *this;
    }

    operator FString() const
    {
        return UTF8_TO_TCHAR(this->c_str());
    }
#endif
};


inline size_t KBStrlen(const KBCHAR* str)
{
    return str ? strlen(str) : 0;
}


// TCHAR -> std::string (ANSI / UTF-8)
inline std::string TCHARToANSI(const char* str)
{
    return str ? std::string(str) : std::string();
}




// char* -> KBCHAR* (ANSI_TO_TCHAR)
inline std::string ANSIToTCHAR(const char* str)
{
    return str ? std::string(str) : std::string();
}


// UTF-8 字符串 -> KBString
inline KBString UTF8ToTCHAR(const char* utf8Str)
{
    return utf8Str ? KBString(utf8Str) : KBString();
}





// hash 特化
namespace std
{
    template<>
    struct hash<KBString>
    {
        size_t operator()(const KBString& s) const noexcept
        {
            return std::hash<std::string>()(s);
        }
    };
}



// ==============================
// ✅ 基础整型别名
// ==============================
using int8   = int8_t;
using int16  = int16_t;
using int32  = int32_t;
using int64  = int64_t;

using uint8  = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

// ==============================
// ✅ 指针相关别名
// ==============================
using uintptr = uintptr_t;
using intptr  = intptr_t;

// ==============================
// ✅ 字符类型
// ==============================
// 注意：wchar_t 在不同平台宽度不同（Win32=2字节，Linux/macOS=4字节）
using char8  = char;
using char16 = char16_t;
using char32 = char32_t;

// ==============================
// ✅ 常用宏定义
// ==============================

// 字节数常量
constexpr uint8  BYTE_BITS  = 8;
constexpr uint16 WORD_BITS  = 16;
constexpr uint32 DWORD_BITS = 32;
constexpr uint64 QWORD_BITS = 64;

// 类型最大最小值（使用 C++17 constexpr）
constexpr uint8  MAX_UINT8  = UINT8_MAX;
constexpr uint16 MAX_UINT16 = UINT16_MAX;
constexpr uint32 MAX_UINT32 = UINT32_MAX;
constexpr uint64 MAX_UINT64 = UINT64_MAX;

constexpr int8   MIN_INT8   = INT8_MIN;
constexpr int16  MIN_INT16  = INT16_MIN;
constexpr int32  MIN_INT32  = INT32_MIN;
constexpr int64  MIN_INT64  = INT64_MIN;

constexpr int8   MAX_INT8   = INT8_MAX;
constexpr int16  MAX_INT16  = INT16_MAX;
constexpr int32  MAX_INT32  = INT32_MAX;
constexpr int64  MAX_INT64  = INT64_MAX;


// -------------------------
// 检测是否在 Unreal Engine 下
// -------------------------
#if defined(__UNREAL__) || defined(UE_BUILD_DEBUG) || defined(UE_BUILD_SHIPPING)
    #include "CoreMinimal.h" // 引入 FVector 等
    #define KBVECTOR_USE_UE 1
#else
    #define KBVECTOR_USE_UE 0
#endif

// -------------------------
// KBVector2f
// -------------------------
struct KBVector2f
{
    float x, y;

    KBVector2f() : x(0.f), y(0.f) {}
    KBVector2f(float _x, float _y) : x(_x), y(_y) {}

    void Set(float _x, float _y)
    {
        x = _x;
        y = _y;
    }

#if KBVECTOR_USE_UE
    KBVector2f(const FVector2D& v) : x(v.X), y(v.Y) {}
    operator FVector2D() const { return FVector2D(x, y); }
#endif

    float length() const { return std::sqrt(x*x + y*y); }
    void normalize() { float len = length(); if(len>0) { x/=len; y/=len; } }
    static float dot(const KBVector2f& a, const KBVector2f& b) { return a.x*b.x + a.y*b.y; }

    std::vector<uint8_t> toBytes() const
    {
        std::vector<uint8_t> bytes(sizeof(float)*2);
        std::memcpy(bytes.data(), &x, sizeof(float));
        std::memcpy(bytes.data() + sizeof(float), &y, sizeof(float));
        return bytes;
    }

    static KBVector2f fromBytes(const std::vector<uint8_t>& bytes)
    {
        if (bytes.size() != sizeof(float)*2)
            throw std::runtime_error("Invalid byte size for KBVector2f");
        KBVector2f v;
        std::memcpy(&v.x, bytes.data(), sizeof(float));
        std::memcpy(&v.y, bytes.data() + sizeof(float), sizeof(float));
        return v;
    }

};

// -------------------------
// KBVector3f
// -------------------------
struct KBVector3f
{
    float x, y, z;

    KBVector3f() : x(0.f), y(0.f), z(0.f) {}
    KBVector3f(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

#if KBVECTOR_USE_UE
    KBVector3f(const FVector& v) : x(v.X), y(v.Y), z(v.Z) {}
    operator FVector() const { return FVector(x, y, z); }
#endif

    void Set(float _x, float _y, float _z)
    {
        x = _x;
        y = _y;
        z = _z;
    }

    float length() const { return std::sqrt(x*x + y*y + z*z); }
    void normalize() { float len = length(); if(len>0) { x/=len; y/=len; z/=len; } }
    static float dot(const KBVector3f& a, const KBVector3f& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
    static KBVector3f cross(const KBVector3f& a, const KBVector3f& b)
    {
        return KBVector3f(
            a.y*b.z - a.z*b.y,
            a.z*b.x - a.x*b.z,
            a.x*b.y - a.y*b.x
        );
    }

    KBVector3f operator+(const KBVector3f& b) const { return KBVector3f(x+b.x, y+b.y, z+b.z); }
    KBVector3f operator-(const KBVector3f& b) const { return KBVector3f(x-b.x, y-b.y, z-b.z); }
    KBVector3f operator*(float s) const { return KBVector3f(x*s, y*s, z*s); }

    std::vector<uint8_t> toBytes() const
    {
        std::vector<uint8_t> bytes(sizeof(float)*3);
        std::memcpy(bytes.data(), &x, sizeof(float));
        std::memcpy(bytes.data() + sizeof(float), &y, sizeof(float));
        std::memcpy(bytes.data() + 2*sizeof(float), &z, sizeof(float));
        return bytes;
    }

    static KBVector3f fromBytes(const std::vector<uint8_t>& bytes)
    {
        if (bytes.size() != sizeof(float)*3)
            throw std::runtime_error("Invalid byte size for KBVector3f");
        KBVector3f v;
        std::memcpy(&v.x, bytes.data(), sizeof(float));
        std::memcpy(&v.y, bytes.data() + sizeof(float), sizeof(float));
        std::memcpy(&v.z, bytes.data() + 2*sizeof(float), sizeof(float));
        return v;
    }
};

// -------------------------
// KBVector4f
// -------------------------
struct KBVector4f
{
    float x, y, z, w;

    KBVector4f() : x(0.f), y(0.f), z(0.f), w(0.f) {}
    KBVector4f(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

#if KBVECTOR_USE_UE
    KBVector4f(const FVector4& v) : x(v.X), y(v.Y), z(v.Z), w(v.W) {}
    operator FVector4() const { return FVector4(x, y, z, w); }
#endif

    void Set(float _x, float _y, float _z, float _w)
    {
        x = _x;
        y = _y;
        z = _z;
        w = _w;
    }

    float length() const { return std::sqrt(x*x + y*y + z*z + w*w); }
    void normalize() { float len = length(); if(len>0) { x/=len; y/=len; z/=len; w/=len; } }
    static float dot(const KBVector4f& a, const KBVector4f& b) { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }

    KBVector4f operator+(const KBVector4f& b) const { return KBVector4f(x+b.x, y+b.y, z+b.z, w+b.w); }
    KBVector4f operator-(const KBVector4f& b) const { return KBVector4f(x-b.x, y-b.y, z-b.z, w-b.w); }
    KBVector4f operator*(float s) const { return KBVector4f(x*s, y*s, z*s, w*s); }

    std::vector<uint8_t> toBytes() const
    {
        std::vector<uint8_t> bytes(sizeof(float)*4);
        std::memcpy(bytes.data(), &x, sizeof(float));
        std::memcpy(bytes.data() + sizeof(float), &y, sizeof(float));
        std::memcpy(bytes.data() + 2*sizeof(float), &z, sizeof(float));
        std::memcpy(bytes.data() + 3*sizeof(float), &w, sizeof(float));
        return bytes;
    }

    static KBVector4f fromBytes(const std::vector<uint8_t>& bytes)
    {
        if (bytes.size() != sizeof(float)*4)
            throw std::runtime_error("Invalid byte size for KBVector4f");
        KBVector4f v;
        std::memcpy(&v.x, bytes.data(), sizeof(float));
        std::memcpy(&v.y, bytes.data() + sizeof(float), sizeof(float));
        std::memcpy(&v.z, bytes.data() + 2*sizeof(float), sizeof(float));
        std::memcpy(&v.w, bytes.data() + 3*sizeof(float), sizeof(float));
        return v;
    }
};


template<typename T>
class KBArray
{
public:
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    KBArray() = default;
    KBArray(std::initializer_list<T> init) : mData(init) {}


    // UE TArray 构造与赋值
#if defined(KBE_PLATFORM_UE)
    // ===========================
    // TArray 构造与赋值
    // ===========================
    KBArray(const TArray<T>& arr)
    {
        mData.reserve(arr.Num());
        for (auto& elem : arr)
            mData.push_back(elem);
    }

    KBArray& operator=(const TArray<T>& arr)
    {
        mData.clear();
        mData.reserve(arr.Num());
        for (auto& elem : arr)
            mData.push_back(elem);
        return *this;
    }

    // 转换为 TArray
    TArray<T> ToTArray() const
    {
        TArray<T> arr;
        arr.Reserve(mData.size());
        for (auto& elem : mData)
            arr.Add(elem);
        return arr;
    }

    // 可选隐式转换
    operator TArray<T>() const
    {
        return ToTArray();
    }
#endif


    void Add(const T& value) { mData.push_back(value); }
    void Add(T&& value) { mData.push_back(std::move(value)); }

    void RemoveAt(size_t index)
    {
        if (index < mData.size())
            mData.erase(mData.begin() + index);
    }

    void RemoveAt(size_t index, size_t count)
    {
        if (index >= mData.size())
            return;

        // 修正 count 防止越界
        if (index + count > mData.size())
            count = mData.size() - index;

        mData.erase(mData.begin() + index, mData.begin() + index + count);
    }


    template<typename... Args>
    T& Emplace(Args&&... args)
    {
        mData.emplace_back(std::forward<Args>(args)...);
        return mData.back();
    }

    bool Remove(const T& item)
    {
        auto it = std::find(mData.begin(), mData.end(), item);
        if (it != mData.end())
        {
            mData.erase(it);
            return true;
        }
        return false;
    }



    void Clear() { mData.clear(); }

    size_t Num() const { return mData.size(); }

    T& operator[](size_t index) { return mData[index]; }
    const T& operator[](size_t index) const { return mData[index]; }

    iterator begin() { return mData.begin(); }
    iterator end() { return mData.end(); }
    const_iterator begin() const { return mData.begin(); }
    const_iterator end() const { return mData.end(); }

    // ===========================
    // EmplaceAt_GetRef 方法
    // ===========================
    template<typename... Args>
    T& EmplaceAt_GetRef(size_t index, Args&&... args)
    {
        if (index > mData.size())
            index = mData.size(); // 超出范围就插到末尾

        auto it = mData.begin() + index;
        it = mData.emplace(it, std::forward<Args>(args)...);
        return *it;
    }

    // ===========================
    // Find 方法
    // ===========================
    int Find(const T& value) const
    {
        for (size_t i = 0; i < mData.size(); ++i)
        {
            if (mData[i] == value)
                return static_cast<int>(i);
        }
        return -1; // 没找到，类似 UE 的 INDEX_NONE
    }

    T Pop()
    {
        if (mData.empty())
            throw std::out_of_range("KBArray::Pop() called on empty array");

        T value = std::move(mData.back());
        mData.pop_back();
        return value;
    }

    // 获取内部数组原生指针
    T* GetData()
    {
        return mData.data();
    }

    // const 版本
    const T* GetData() const
    {
        return mData.data();
    }


    // 设置数组大小为 newNum
    // 不初始化新元素（保持原始内存）
    // 如果 newNum 小于当前大小，则截断数组
    void SetNumUninitialized(size_t newNum)
    {
        if (newNum <= mData.size())
        {
            // 缩小数组，直接截断
            mData.erase(mData.begin() + newNum, mData.end());
        }
        else
        {
            // 扩容但不构造对象
            size_t oldSize = mData.size();
            mData.reserve(newNum);              // 确保容量够
            mData.insert(mData.end(), newNum - oldSize, T()); // ⚠️ 默认构造
        }
    }

    // 预分配容量
    void Reserve(size_t newCapacity)
    {
        if (newCapacity > mData.capacity())
        {
            mData.reserve(newCapacity);
        }
    }

    void SetNum(size_t newSize)
    {
        size_t oldSize = mData.size();
        if (newSize > oldSize)
        {
            // 扩容并默认构造新元素
            mData.reserve(newSize);
            for (size_t i = oldSize; i < newSize; ++i)
            {
                mData.push_back(T()); // 默认构造
            }
        }
        else if (newSize < oldSize)
        {
            // 删除多余元素
            mData.erase(mData.begin() + newSize, mData.end());
        }
    }

    // ===========================
    // Contains 方法
    // ===========================
    bool Contains(const T& value) const
    {
        return std::find(mData.begin(), mData.end(), value) != mData.end();
    }


    bool operator==(const KBArray<T>& other) const
    {
        return mData == other.mData;
    }

    bool operator!=(const KBArray<T>& other) const
    {
        return mData != other.mData;
    }


private:
    std::vector<T> mData;
};



template<typename Key, typename Value>
class KBMap
{
public:
    using iterator = typename std::unordered_map<Key, Value>::iterator;
    using const_iterator = typename std::unordered_map<Key, Value>::const_iterator;

    KBMap() = default;

    void Add(const Key& key, const Value& value) { mData[key] = value; }
    void Add(Key&& key, Value&& value) { mData[std::forward<Key>(key)] = std::forward<Value>(value); }

    bool Remove(const Key& key) { return mData.erase(key) > 0; }

    void Clear() { mData.clear(); }

    bool Contains(const Key& key) const { return mData.find(key) != mData.end(); }

    Value* Find(const Key& key)
    {
        auto it = mData.find(key);
        if(it != mData.end())
            return &it->second;
        return nullptr;
    }

    const Value* Find(const Key& key) const
    {
        auto it = mData.find(key);
        if(it != mData.end())
            return &it->second;
        return nullptr;
    }



    // ========================
    // 新增 FindRef 方法
    // ========================
    // Value 是对象
    Value* FindRefObject(const Key& key)
    {
        auto it = mData.find(key);
        if (it != mData.end())
            return &it->second;
        return nullptr;
    }

    // Value 是指针
    Value FindRefPointer(const Key& key)
    {
        auto it = mData.find(key);
        if (it != mData.end())
            return it->second;
        return nullptr;
    }

    Value FindRef(const Key& key)
    {
        auto it = mData.find(key);
        if (it != mData.end())
            return (it->second);   // 解引用返回对象
        return Value{};             // 返回默认对象
    }

    Value& operator[](const Key& key) { return mData[key]; }
    const Value& operator[](const Key& key) const { return mData.at(key); }

    iterator begin() { return mData.begin(); }
    iterator end() { return mData.end(); }
    const_iterator begin() const { return mData.begin(); }
    const_iterator end() const { return mData.end(); }


    size_t Num() const { return mData.size(); }

private:
    std::unordered_map<Key, Value> mData;
};


struct KBRotator
{
    float pitch;
    float yaw;
    float roll;

    KBRotator() : pitch(0), yaw(0), roll(0) {}
    KBRotator(float p, float y, float r) : pitch(p), yaw(y), roll(r) {}
};
