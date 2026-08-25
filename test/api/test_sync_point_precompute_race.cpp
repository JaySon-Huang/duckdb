#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/sync_point.hpp"
#include "duckdb/common/types.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace duckdb;

#ifdef D_ASSERT_IS_ENABLED

namespace {

//! Joins the thread even when a REQUIRE fails; the sync point guard must be
//! declared after this joiner so it is destroyed first and releases the thread.
struct RaceThreadJoiner {
	std::thread thread;
	~RaceThreadJoiner() {
		if (thread.joinable()) {
			thread.join();
		}
	}
};

//! Layout after setup (row group size 2048):
//!   partition 0: 2048 rows of 1    -> deleted before the reader starts, vacuums
//!                                     out of the tree at the mutation checkpoint
//!   partition 1: alternating 5/6   -> needs a scan (contributes 1024 rows of 5)
//!   partition 2: 2048 rows of 5    -> always true, precomputed
//! Correct count for key=5: 2048 (precomputed) + 1024 (scanned) = 3072.
static constexpr double PRECOMPUTE_RACE_EXPECTED = 2048.0 + 1024.0;

} // namespace

//! The partial precompute (PR #21831) records positional indices into the row
//! group list at plan time and the scan skips by those positions at execution
//! time. PR #24962 disabled it because a checkpoint can swap the row group
//! collection in between: the plan-time indices then refer to different (or
//! fewer) partitions, so a partition that was precomputed is scanned again and
//! its rows are counted twice. This test forces exactly that interleaving with
//! a sync point: the reader parks after capturing its indices, a second
//! connection checkpoints (which vaccums out the fully-deleted leading row
//! group and installs a renumbered collection), and only then is the reader
//! released. On a tree with the partial precompute active the count comes out
//! wrong (5120 instead of 3072, observed deterministically); on trees where the
//! optimization is disabled the hook is unreachable and the query just returns
//! the correct count.
TEST_CASE("Test partial precompute partition race reproduces PR 24962", "[api][sync_point]") {
	auto db_path = TestDirectoryPath() + "precompute_race.duckdb";
	duckdb::DuckDB db(nullptr);
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("ATTACH '" + db_path + "' AS race_db (ROW_GROUP_SIZE 2048)"));
	REQUIRE_NO_FAIL(conn.Query("CREATE TABLE race_db.t(key INTEGER)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT 1 FROM range(2048)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT CASE WHEN i % 2 = 0 THEN 5 ELSE 6 END FROM range(2048) r(i)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT 5 FROM range(2048)"));
	REQUIRE_NO_FAIL(conn.Query("CHECKPOINT race_db"));
	// commit the delete before the reader starts so the checkpoint below is
	// allowed to drop the fully-deleted leading row group
	REQUIRE_NO_FAIL(conn.Query("DELETE FROM race_db.t WHERE key=1"));
	Connection writer(db);

	auto guard = SyncPointCtl::EnableInScope("optimizer.partial_precompute.indices_captured");
	std::atomic<double> result(-1);
	RaceThreadJoiner joiner;
	joiner.thread = std::thread([&] {
		Connection reader(db);
		auto res = reader.Query("SELECT count(*) FROM race_db.t WHERE key=5");
		if (res->HasError()) {
			std::cout << "query failed: " << res->GetError() << std::endl;
			return;
		}
		result = res->GetValue<double>(0, 0);
	});

	bool precompute_active = true;
	try {
		guard.WaitAndPause(3000);
	} catch (const InternalException &) {
		precompute_active = false;
	}
	if (precompute_active) {
		// the reader is parked at the captured indices: swap the row group
		// collection under it while it waits
		REQUIRE_NO_FAIL(writer.Query("CHECKPOINT race_db"));
		guard.Next();
	}
	joiner.thread.join();
	REQUIRE(result == PRECOMPUTE_RACE_EXPECTED);
}

#endif