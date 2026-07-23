
using KBEngine;
using System; 
using System.Collections;

namespace KBEngine
{

/*
	KBEngine的数学相关模块
*/
public class KBEMath 
{
	public static float KBE_FLT_MAX = float.MaxValue;
	
	public static float int82angle(SByte angle, bool half)
	{
		float halfv = 128f;
		if(half == true)
			halfv = 254f;
		
		halfv = ((float)angle) * ((float)System.Math.PI / halfv);
		return halfv;
	}
	
	public static bool almostEqual(float f1, float f2, float epsilon)
	{
		return Math.Abs( f1 - f2 ) < epsilon;
	}

	public static bool isNumeric(object v)
	{
		return v is sbyte || v is byte ||
			v is short || v is ushort ||
			v is int || v is uint ||
			v is long || v is ulong ||
			v is char || v is decimal || v is float ||
			v is double || v is Int16 || v is Int64 ||
			v is UInt16 || v is UInt64 || 
			v is Boolean || v is bool;
	}
}



public struct KBVector2 : IEquatable<KBVector2>
{
	public float x;
	public float y;

	public KBVector2(float x, float y)
	{
		this.x = x;
		this.y = y;
	}

	public void Set(float x, float y)
	{
		this.x = x;
		this.y = y;
	}

	public float Distance(KBVector2 other)
	{
		float dx = this.x - other.x;
		float dy = this.y - other.y;
		return (float)Math.Sqrt(dx * dx + dy * dy);
	}

	public static float Distance(KBVector2 a, KBVector2 b) => a.Distance(b);

	public float this[int index]
	{
		get => index switch
		{
			0 => x,
			1 => y,
			_ => throw new IndexOutOfRangeException("KBVector2 index must be 0 or 1.")
		};
		set
		{
			switch (index)
			{
				case 0: x = value; break;
				case 1: y = value; break;
				default: throw new IndexOutOfRangeException("KBVector2 index must be 0 or 1.");
			}
		}
	}

	public override string ToString() => $"({x}, {y})";

	public bool Equals(KBVector2 other) => x == other.x && y == other.y;

	public override bool Equals(object obj) => obj is KBVector2 other && Equals(other);

	public override int GetHashCode() => HashCode.Combine(x, y);

	public static bool operator ==(KBVector2 left, KBVector2 right) => left.Equals(right);

	public static bool operator !=(KBVector2 left, KBVector2 right) => !left.Equals(right);
	
	
#if GODOT
	//  转换为 Godot.Vector2
	public static implicit operator Godot.Vector2(KBVector2 v) => new Godot.Vector2(v.x, v.y);

	//  从 Godot.Vector2 转换
	public static implicit operator KBVector2(Godot.Vector2 v) => new KBVector2(v.X, v.Y);
#endif

#if UNITY_5_3_OR_NEWER
	//  转换为 UnityEngine.Vector2
	public static implicit operator UnityEngine.Vector2(KBVector2 v) => new UnityEngine.Vector2(v.x, v.y);

	//  从 UnityEngine.Vector2 转换
	public static implicit operator KBVector2(UnityEngine.Vector2 v) => new KBVector2(v.x, v.y);
#endif
}


public struct KBVector3 : IEquatable<KBVector3>
{
	public float x;
	public float y;
	public float z;

	public KBVector3(float x = 0, float y = 0, float z = 0)
	{
		this.x = x;
		this.y = y;
		this.z = z;
	}

	public void Set(float x, float y, float z)
	{
		this.x = x;
		this.y = y;
		this.z = z;
	}

	public float Distance(KBVector3 other)
	{
		float dx = x - other.x;
		float dy = y - other.y;
		float dz = z - other.z;
		return (float)Math.Sqrt(dx * dx + dy * dy + dz * dz);
	}

	public static float Distance(KBVector3 a, KBVector3 b) => a.Distance(b);

	public float this[int index]
	{
		get => index switch
		{
			0 => x,
			1 => y,
			2 => z,
			_ => throw new IndexOutOfRangeException("KBVector3 index must be 0, 1, or 2.")
		};
		set
		{
			switch (index)
			{
				case 0: x = value; break;
				case 1: y = value; break;
				case 2: z = value; break;
				default: throw new IndexOutOfRangeException("KBVector3 index must be 0, 1, or 2.");
			}
		}
	}

	public override string ToString() => $"({x}, {y}, {z})";

	public bool Equals(KBVector3 other) => x == other.x && y == other.y && z == other.z;

	public override bool Equals(object obj) => obj is KBVector3 other && Equals(other);

	public override int GetHashCode() => HashCode.Combine(x, y, z);

	public static bool operator ==(KBVector3 left, KBVector3 right) => left.Equals(right);

	public static bool operator !=(KBVector3 left, KBVector3 right) => !left.Equals(right);


#if GODOT
	//  转换为 Godot.Vector3
	public static implicit operator Godot.Vector3(KBVector3 v) => new Godot.Vector3(v.x, v.y, v.z);

	//  从 Godot.Vector3 转换
	public static implicit operator KBVector3(Godot.Vector3 v) => new KBVector3(v.X, v.Y, v.Z);
#endif

#if UNITY_5_3_OR_NEWER
	//  转换为 UnityEngine.Vector3
	public static implicit operator UnityEngine.Vector3(KBVector3 v) => new UnityEngine.Vector3(v.x, v.y, v.z);

	//  从 UnityEngine.Vector3 转换
	public static implicit operator KBVector3(UnityEngine.Vector3 v) => new KBVector3(v.x, v.y, v.z);
#endif
	

}


public struct KBVector4 : IEquatable<KBVector4>
{
	public float x;
	public float y;
	public float z;
	public float w;

	public KBVector4(float x, float y, float z, float w)
	{
		this.x = x;
		this.y = y;
		this.z = z;
		this.w = w;
	}

	public void Set(float x, float y, float z, float w)
	{
		this.x = x;
		this.y = y;
		this.z = z;
		this.w = w;
	}

	public float this[int index]
	{
		get => index switch
		{
			0 => x,
			1 => y,
			2 => z,
			3 => w,
			_ => throw new IndexOutOfRangeException("KBVector4 index must be 0 to 3.")
		};
		set
		{
			switch (index)
			{
				case 0: x = value; break;
				case 1: y = value; break;
				case 2: z = value; break;
				case 3: w = value; break;
				default: throw new IndexOutOfRangeException("KBVector4 index must be 0 to 3.");
			}
		}
	}

	public override string ToString() => $"({x}, {y}, {z}, {w})";

	public bool Equals(KBVector4 other) => x == other.x && y == other.y && z == other.z && w == other.w;

	public override bool Equals(object obj) => obj is KBVector4 other && Equals(other);

	public override int GetHashCode() => HashCode.Combine(x, y, z, w);

	public static bool operator ==(KBVector4 left, KBVector4 right) => left.Equals(right);

	public static bool operator !=(KBVector4 left, KBVector4 right) => !left.Equals(right);
	
	
#if GODOT
	//  转换为 Godot.Vector4
	public static implicit operator Godot.Vector4(KBVector4 v) => new Godot.Vector4(v.x, v.y, v.z,v.w);

	//  从 Godot.Vector4 转换
	public static implicit operator KBVector4(Godot.Vector4 v) => new KBVector4(v.X, v.Y, v.Z,v.W);
#endif

#if UNITY_5_3_OR_NEWER
	//  转换为 UnityEngine.Vector4
	public static implicit operator UnityEngine.Vector4(KBVector4 v) => new UnityEngine.Vector4(v.x, v.y, v.z,v.w);

	//  从 UnityEngine.Vector4 转换
	public static implicit operator KBVector4(UnityEngine.Vector4 v) => new KBVector4(v.x, v.y, v.z,v.w);
#endif
}


}
