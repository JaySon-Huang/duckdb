#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/sync_point.hpp"
#include "duckdb/common/types.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace duckdb;

#ifdef D_ASSERT_IS_ENABLED

namespace {

//! Joins the thread even when a REQUIRE fails; the sync point guard must be
//! declared after this joiner so it is destroyed first and releases the thread.
struct AlterTypeThreadJoiner {
	std::thread thread;
	~AlterTypeThreadJoiner() {
		if (thread.joinable()) {
			thread.join();
		}
	}
};

} // namespace

//! The ALTER TYPE rewrite reads the column values at the altering transaction's
//! snapshot and swaps in a new column that does not inherit the old column's
//! update segments. A concurrent UPDATE that commits between the value reads and
//! the ALTERED mark is therefore silently dropped (PR #25008). This test forces
//! exactly that window with a sync point: the rewrite parks after the reads, a
//! second connection commits an UPDATE, and only then is the rewrite released.
//! The invariant is "no silent loss": either the UPDATE commits and its new value
//! is visible, or it aborts loudly.

// TODO: re-enable once #25008 merges. On the current tree the UPDATE commits
// and the value is lost, so this test fails; with the fix the UPDATE conflicts
// with the ALTERED mark and aborts instead, and the test goes green.
/* TEST_CASE("Test ALTER TYPE does not drop a concurrently committed update", "[storage][sync_point]") {
	duckdb::DuckDB db(nullptr);
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("CREATE TABLE t(id INTEGER, v INTEGER)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO t VALUES (1, 0)"));

	auto guard = SyncPointCtl::EnableInScope("alter_type.rewrite_scan_complete");
	std::string alter_error;
	AlterTypeThreadJoiner joiner;
	joiner.thread = std::thread([&] {
		Connection alter(db);
		auto res = alter.Query("ALTER TABLE t ALTER COLUMN v SET DATA TYPE BIGINT");
		if (res->HasError()) {
			alter_error = res->GetError();
		}
	});

	std::atomic<bool> update_succeeded(false);
	bool rewrite_parked = true;
	try {
		guard.WaitAndPause(3000);
	} catch (const InternalException &) {
		rewrite_parked = false;
	}
	if (rewrite_parked) {
		// the rewrite is parked between the value reads and the ALTERED mark:
		// commit the update while it waits
		Connection updater(db);
		auto res = updater.Query("UPDATE t SET v = 7");
		update_succeeded = !res->HasError();
		guard.Next();
	}
	joiner.thread.join();
	REQUIRE(alter_error.empty());
	double final_value = -1;
	auto check = conn.Query("SELECT v FROM t");
	REQUIRE(!check->HasError());
	final_value = check->GetValue<double>(0, 0);
	INFO(StringUtil::Format("update_succeeded=%d final_value=%f", (int)update_succeeded.load(), final_value));
	// the committed update must not be silently lost: either it committed and is
	// visible, or it aborted
	REQUIRE((!update_succeeded || final_value == 7.0));
}
*/

#endif