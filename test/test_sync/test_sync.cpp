/**
 * Host tests for include/sonos_sync_logic.h: the background upkeep schedule,
 * the deferred bring-up back-off and the daily memory decision.
 *
 *     pio test -e native -f test_sync
 */
#include <unity.h>
#include "sonos_sync_logic.h"
using namespace sonos_sync;

void setUp(void) {}
void tearDown(void) {}

static const uint32_t H = 60UL * 60UL * 1000UL;
static const uint32_t M = 60UL * 1000UL;
static const Timing T = { 45000UL, 4 * H, 10 * M, 1 * H, 5 * M, 24 * H };

void test_due_and_elapsed_survive_wrap(void) {
    const uint32_t at = 0xFFFFFF00u + 0x200u;       // deadline past the wrap
    TEST_ASSERT_FALSE(due(0xFFFFFF00u, at));
    TEST_ASSERT_TRUE(due(0x100u, at));
    TEST_ASSERT_TRUE(due(0x200u, at));
    TEST_ASSERT_TRUE(elapsed(0x10u, 0xFFFFFFF0u, 0x20u));
    TEST_ASSERT_FALSE(elapsed(0x0Fu, 0xFFFFFFF0u, 0x20u));
}

void test_first_refresh_then_every_four_hours(void) {
    Schedule s = {};
    TEST_ASSERT_FALSE(refreshDue(s, 0, 3, 0, T));   // not started
    start(s, 1000, T);
    TEST_ASSERT_FALSE(refreshDue(s, 1000 + 44999, 3, 0, T));
    TEST_ASSERT_TRUE(refreshDue(s, 1000 + 45000, 3, 0, T));
    refreshDone(s, 50000, 3, T);
    TEST_ASSERT_FALSE(refreshDue(s, 50000 + 4 * H - 1, 3, 0, T));
    TEST_ASSERT_TRUE(refreshDue(s, 50000 + 4 * H, 3, 0, T));
}

void test_zero_speakers_retry_after_ten_minutes(void) {
    Schedule s = {};
    start(s, 0, T);
    refreshDone(s, 100000, 0, T);
    TEST_ASSERT_FALSE(refreshDue(s, 100000 + 10 * M - 1, 0, 0, T));
    TEST_ASSERT_TRUE(refreshDue(s, 100000 + 10 * M, 0, 0, T));
    // Finding speakers puts it back on the 4-hour interval.
    refreshDone(s, 200000, 2, T);
    TEST_ASSERT_FALSE(refreshDue(s, 200000 + 10 * M, 2, 0, T));
}

void test_idle_panel_refreshes_hourly(void) {
    Schedule s = {};
    start(s, 0, T);
    refreshDone(s, 0, 4, T);
    // An hour later, untouched for 5 minutes: due, although 4 h have not passed.
    TEST_ASSERT_TRUE(refreshDue(s, 1 * H, 4, 5 * M, T));
    // Touched a minute ago: wait for the 4-hour deadline.
    TEST_ASSERT_FALSE(refreshDue(s, 1 * H, 4, 1 * M, T));
    // Not yet an hour: no.
    TEST_ASSERT_FALSE(refreshDue(s, 1 * H - 1, 4, 30 * M, T));
}

void test_schedule_crosses_millis_wrap(void) {
    Schedule s = {};
    start(s, 0xFFFFFFFFu - 1000u, T);
    TEST_ASSERT_FALSE(refreshDue(s, 0xFFFFFFFFu, 1, 0, T));
    TEST_ASSERT_TRUE(refreshDue(s, 45000u, 1, 0, T));     // after the wrap
    TEST_ASSERT_FALSE(dailyDue(s, 45000u));
}

void test_daily_check_every_24_hours(void) {
    Schedule s = {};
    TEST_ASSERT_FALSE(dailyDue(s, 24 * H));               // not started
    start(s, 0, T);
    TEST_ASSERT_FALSE(dailyDue(s, 24 * H - 1));
    TEST_ASSERT_TRUE(dailyDue(s, 24 * H));
    dailyDone(s, 24 * H + 500, T);
    TEST_ASSERT_FALSE(dailyDue(s, 48 * H));
    TEST_ASSERT_TRUE(dailyDue(s, 48 * H + 500));
}

void test_plan_never_full_scan_with_known_speakers(void) {
    TEST_ASSERT_EQUAL(PLAN_FULL_SCAN, planRefresh(0));
    TEST_ASSERT_EQUAL(PLAN_FULL_SCAN, planRefresh(-1));
    TEST_ASSERT_EQUAL(PLAN_TOPOLOGY, planRefresh(1));
    TEST_ASSERT_EQUAL(PLAN_TOPOLOGY, planRefresh(32));
}

void test_backoff_doubles_and_caps(void) {
    const uint32_t base = 30000, cap = 10 * M;
    TEST_ASSERT_EQUAL_UINT32(0, backoffMs(0, base, cap));
    TEST_ASSERT_EQUAL_UINT32(30000, backoffMs(1, base, cap));
    TEST_ASSERT_EQUAL_UINT32(60000, backoffMs(2, base, cap));
    TEST_ASSERT_EQUAL_UINT32(120000, backoffMs(3, base, cap));
    TEST_ASSERT_EQUAL_UINT32(240000, backoffMs(4, base, cap));
    TEST_ASSERT_EQUAL_UINT32(480000, backoffMs(5, base, cap));
    TEST_ASSERT_EQUAL_UINT32(cap, backoffMs(6, base, cap));
    TEST_ASSERT_EQUAL_UINT32(cap, backoffMs(255, base, cap));   // no shift overflow
    TEST_ASSERT_EQUAL_UINT32(cap, backoffMs(1, 2 * cap, cap));  // base above cap
}

void test_memory_floor(void) {
    const MemSample floor = { 32 * 1024, 4 * 1024 * 1024, 16000 };
    const MemSample ok    = { 80 * 1024, 20 * 1024 * 1024, 40000 };
    TEST_ASSERT_FALSE(exhausted(ok, floor));
    MemSample s = ok; s.internalFree = 32 * 1024 - 1;
    TEST_ASSERT_TRUE(exhausted(s, floor));
    s = ok; s.psramFree = 1024;
    TEST_ASSERT_TRUE(exhausted(s, floor));
    s = ok; s.dmaFree = 15999;
    TEST_ASSERT_TRUE(exhausted(s, floor));
    TEST_ASSERT_FALSE(exhausted(floor, floor));            // at the floor is not below it
}

void test_untouched_clamps_a_touch_after_now(void) {
    TEST_ASSERT_EQUAL_UINT32(5000, untouchedMs(10000, 5000));
    TEST_ASSERT_EQUAL_UINT32(0, untouchedMs(10000, 10003));          // touch landed after `now`
    TEST_ASSERT_EQUAL_UINT32(0x200u, untouchedMs(0x100u, 0xFFFFFF00u)); // across the wrap
}

void test_memory_check_is_daily_and_separate(void) {
    Schedule s = {};
    start(s, 1000, T);
    TEST_ASSERT_FALSE(memDue(s, 1000 + 24 * H - 1));
    TEST_ASSERT_TRUE(memDue(s, 1000 + 24 * H));
    dailyDone(s, 1000 + 24 * H, T);                 // selection done, memory still owed
    TEST_ASSERT_TRUE(memDue(s, 1000 + 24 * H));
    memDone(s, 1000 + 24 * H, T);
    TEST_ASSERT_FALSE(memDue(s, 1000 + 48 * H - 1));
    TEST_ASSERT_TRUE(memDue(s, 1000 + 48 * H));
}

void test_memory_check_needs_two_exhausted_samples(void) {
    const uint32_t gap = 15000;
    MemCheck c = {};
    // Healthy first sample: done at once.
    TEST_ASSERT_TRUE(memSampleNow(c, 0, gap));
    TEST_ASSERT_EQUAL(MEM_OK, memCheckStep(c, 0, false));
    // Exhausted, then healthy: a transient, no restart.
    TEST_ASSERT_EQUAL(MEM_WAIT, memCheckStep(c, 100, true));
    TEST_ASSERT_FALSE(memSampleNow(c, 100 + gap - 1, gap));
    TEST_ASSERT_TRUE(memSampleNow(c, 100 + gap, gap));
    TEST_ASSERT_EQUAL(MEM_OK, memCheckStep(c, 100 + gap, false));
    TEST_ASSERT_FALSE(c.pending);
    // Exhausted twice, across the millis wrap: restart.
    const uint32_t t0 = 0xFFFFF000u;
    TEST_ASSERT_EQUAL(MEM_WAIT, memCheckStep(c, t0, true));
    TEST_ASSERT_FALSE(memSampleNow(c, t0 + 1000, gap));
    TEST_ASSERT_TRUE(memSampleNow(c, t0 + gap, gap));
    TEST_ASSERT_EQUAL(MEM_EXHAUSTED, memCheckStep(c, t0 + gap, true));
    TEST_ASSERT_FALSE(c.pending);
}

void test_postpone_after_failed_task_create(void) {
    uint32_t at = 0;
    postpone(at, 0xFFFFFFF0u, 60000);
    TEST_ASSERT_FALSE(due(0xFFFFFFF0u + 59999, at));
    TEST_ASSERT_TRUE(due(0xFFFFFFF0u + 60000, at));
}

void test_line_in_field_round_trip(void) {
    TEST_ASSERT_EQUAL_CHAR('1', lineInField(true, true));
    TEST_ASSERT_EQUAL_CHAR('0', lineInField(true, false));
    TEST_ASSERT_EQUAL_CHAR('?', lineInField(false, false));
    TEST_ASSERT_EQUAL_CHAR('?', lineInField(false, true));   // a default is never written as a result
    bool known = true, has = true;
    parseLineInField('?', known, has);
    TEST_ASSERT_FALSE(known); TEST_ASSERT_FALSE(has);
    parseLineInField('1', known, has);
    TEST_ASSERT_TRUE(known); TEST_ASSERT_TRUE(has);
    parseLineInField('0', known, has);
    TEST_ASSERT_TRUE(known); TEST_ASSERT_FALSE(has);
    parseLineInField('x', known, has);                       // unknown symbol: ask again
    TEST_ASSERT_FALSE(known); TEST_ASSERT_FALSE(has);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_due_and_elapsed_survive_wrap);
    RUN_TEST(test_first_refresh_then_every_four_hours);
    RUN_TEST(test_zero_speakers_retry_after_ten_minutes);
    RUN_TEST(test_idle_panel_refreshes_hourly);
    RUN_TEST(test_schedule_crosses_millis_wrap);
    RUN_TEST(test_daily_check_every_24_hours);
    RUN_TEST(test_plan_never_full_scan_with_known_speakers);
    RUN_TEST(test_backoff_doubles_and_caps);
    RUN_TEST(test_memory_floor);
    RUN_TEST(test_untouched_clamps_a_touch_after_now);
    RUN_TEST(test_memory_check_is_daily_and_separate);
    RUN_TEST(test_memory_check_needs_two_exhausted_samples);
    RUN_TEST(test_postpone_after_failed_task_create);
    RUN_TEST(test_line_in_field_round_trip);
    return UNITY_END();
}
