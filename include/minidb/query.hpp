#pragma once
#include "minidb/catalog.hpp"
#include "minidb/database.hpp"
namespace minidb {
ResultSet execute_select(PageTransaction &tx, Catalog &catalog, const Statement &statement);
}
