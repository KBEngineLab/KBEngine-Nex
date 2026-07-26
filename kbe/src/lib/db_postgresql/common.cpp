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

#include "common.h"
#include "db_interface_postgresql.h"
#include "db_interface/db_interface.h"
#include "db_interface/entity_table.h"

namespace KBEngine {
namespace postgresql {

DBInterfacePostgresql* pg(DBInterface* pdbi)
{
	return static_cast<DBInterfacePostgresql*>(pdbi);
}

std::string esc(DBInterface* pdbi, const std::string& value)
{
	return pg(pdbi)->escapeString(value.data(), value.size());
}

std::string tableSqlName(DBInterface* pdbi, const char* tableName)
{
	return pg(pdbi)->quoteIdentifier((std::string(ENTITY_TABLE_PERFIX "_") + tableName).c_str());
}

std::string columnSqlName(DBInterface* pdbi, const char* columnName)
{
	return pg(pdbi)->quoteIdentifier(columnName);
}

std::string hexEncode(const char* data, size_t size)
{
	static const char* digits = "0123456789abcdef";
	std::string out;
	out.resize(size * 2);

	for (size_t i = 0; i < size; ++i)
	{
		uint8 ch = static_cast<uint8>(data[i]);
		out[i * 2] = digits[(ch >> 4) & 0x0f];
		out[i * 2 + 1] = digits[ch & 0x0f];
	}

	return out;
}

}
}
