#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/sync_point.hpp"
#include "duckdb/common/types.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
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

//! Expected count for key=5: partition 1 (2048 rows of 5) is precomputed, the mixed
//! partition 2 contributes 1024 rows of 5.
static constexpr double PRECOMPUTE_RACE_EXPECTED = 2048.0 + 1024.0;

//! Runs the race scenario between the partial precompute plan-time index capture
//! and the execution-time scan. Returns the observed count for key=5.
double RunPrecomputeRace(const string &variant) {
	auto db_path = TestDirectoryPath() + variant + ".duckdb";
	duckdb::DuckDB db(nullptr);
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("ATTACH '" + db_path + "' AS race_db (ROW_GROUP_SIZE 2048)"));
	REQUIRE_NO_FAIL(conn.Query("CREATE TABLE race_db.t(key INTEGER)"));
	// partition 0: 2048 rows of 1 (always false for key=5), partition 1: 2048 rows of
	// 5 (always true, precomputed), partition 2: alternating 5/6 (needs a scan)
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT 1 FROM range(2048)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT 5 FROM range(2048)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT CASE WHEN i % 2 = 0 THEN 5 ELSE 6 END FROM range(2048) r(i)"));
	REQUIRE_NO_FAIL(conn.Query("CHECKPOINT race_db"));
	Connection writer(db);

	auto guard = SyncPointCtl::EnableInScope("optimizer.partial_precompute.indices_captured");
	std::atomic<double> result(-1);
	RaceThreadJoiner joiner;
	joiner.thread = std::thread([&] {
		Connection reader(db);
		auto res = reader.Query("SELECT count(*) FROM race_db.t WHERE key=5");
		if (res->HasError()) {
			std::cout << variant << ": query failed: " << res->GetError() << std::endl;
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
		// the reader is parked at the captured indices: mutate the row group list
		writer.Query("INSERT INTO race_db.t SELECT 100 FROM range(2048)");
		writer.Query("CHECKPOINT race_db");
		guard.Next();
	}
	joiner.thread.join();
	std::cout << variant << ": precompute_active=" << precompute_active << " result=" << result << std::endl;
	return result;
}

} // namespace

TEST_CASE("Test partial precompute partition race reproduces PR 24962", "[api][sync_point]") {
	auto result = RunPrecomputeRace("precompute_race_insert_checkpoint");
	REQUIRE(result == PRECOMPUTE_RACE_EXPECTED);
}

#endif
