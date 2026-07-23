using KBEngine;
using System;
using System.Collections;
using System.Collections.Generic;

namespace KBEngine
{
	public class Dbg
	{
		private static bool EnableProfile = false;
		static Dictionary<string, Profile> _profiles = new Dictionary<string, Profile>();

		public static void profileStart(string name)
		{
			if(!EnableProfile) return;
			Profile p = null;
			if(!_profiles.TryGetValue(name, out p))
			{
				p = new Profile(name);
				_profiles.Add(name, p);
			}

			p.start();
		}

		public static void profileEnd(string name)
		{
			if(!EnableProfile) return;
			_profiles[name].end();
		}
		
	}
}
