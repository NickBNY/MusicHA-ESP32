/**
 * Host tests for include/auto_update.h
 *
 *     pio test -e native
 *
 * What goes wrong here is quiet and expensive: a version compare that offers a
 * downgrade, an install that starts from a source nobody chose, or a release
 * that is retried on every boot.
 */
#include <unity.h>
#include <string.h>
#include "auto_update.h"
using namespace auto_update;

static_assert(sourceIsUpstream("OpenSurface/SonosESP"), "upstream is upstream");
static_assert(!sourceIsUpstream("someone/SonosESP"), "a fork is not upstream");
static_assert(!sourceIsUpstream("OpenSurface/SonosESP2"), "longer string is not upstream");
static_assert(!sourceIsUpstream("OpenSurface/SonosES"), "prefix is not upstream");
static_assert(sameRepo("", ""), "empty equals empty");

void setUp(void) {}
void tearDown(void) {}

void test_semver_newer(void) {
    TEST_ASSERT_TRUE(semverNewer("2.4.1", "2.4.0"));
    TEST_ASSERT_TRUE(semverNewer("2.5.0", "2.4.9"));
    TEST_ASSERT_TRUE(semverNewer("10.0.0", "9.9.9"));
}

void test_semver_equal_or_older_is_not_newer(void) {
    TEST_ASSERT_FALSE(semverNewer("2.4.0", "2.4.0"));
    TEST_ASSERT_FALSE(semverNewer("2.3.1", "2.4.0"));
    TEST_ASSERT_FALSE(semverNewer("2.4.0", "2.4.1"));
}

// A nightly suffix must not make a build look newer than its own release.
void test_semver_ignores_nightly_suffix(void) {
    TEST_ASSERT_FALSE(semverNewer("2.4.0-nightly.abc1234", "2.4.0"));
    TEST_ASSERT_TRUE(semverNewer("2.4.1-nightly.abc1234", "2.4.0"));
}

void test_semver_empty_is_not_newer(void) {
    TEST_ASSERT_FALSE(semverNewer("", ""));
    TEST_ASSERT_FALSE(semverNewer("", "2.4.0"));
    TEST_ASSERT_TRUE(semverNewer("2.4.0", ""));
    TEST_ASSERT_FALSE(semverNewer(nullptr, "2.4.0"));
}

void test_upstream_detection(void) {
    TEST_ASSERT_TRUE(sourceIsUpstream("OpenSurface/SonosESP"));
    TEST_ASSERT_FALSE(sourceIsUpstream("someone/SonosESP"));
    TEST_ASSERT_FALSE(sourceIsUpstream("OpenSurface/SonosESP-fork"));
    TEST_ASSERT_FALSE(sourceIsUpstream(""));
}

// All inputs favourable, so each test below flips exactly one.
static Gate gate(bool enabled = true, int channel = 0, bool srcOk = true,
                 bool avail = true, bool url = true, bool wifi = true,
                 uint32_t idle = 300000, uint32_t need = 300000,
                 const char* latest = "2.5.0", const char* tried = "") {
    return installGate(enabled, channel, srcOk, avail, url, wifi, idle, need, latest, tried);
}

void test_gate_allowed_when_everything_lines_up(void) {
    TEST_ASSERT_EQUAL(Gate::Allowed, gate());
}

void test_gate_each_reason(void) {
    TEST_ASSERT_EQUAL(Gate::Disabled,         gate(false));
    TEST_ASSERT_EQUAL(Gate::WrongChannel,     gate(true, 1));
    TEST_ASSERT_EQUAL(Gate::SourceNotAllowed, gate(true, 0, false));
    TEST_ASSERT_EQUAL(Gate::NoAsset,          gate(true, 0, true, false));
    TEST_ASSERT_EQUAL(Gate::NoAsset,          gate(true, 0, true, true, false));
    TEST_ASSERT_EQUAL(Gate::Offline,          gate(true, 0, true, true, true, false));
    TEST_ASSERT_EQUAL(Gate::NotIdle,          gate(true, 0, true, true, true, true, 10));
    TEST_ASSERT_EQUAL(Gate::AlreadyTried,     gate(true, 0, true, true, true, true, 300000, 300000, "2.5.0", "2.5.0"));
}

void test_gate_order(void) {
    // Disabled wins over every other reason.
    TEST_ASSERT_EQUAL(Gate::Disabled, gate(false, 1, false, false, false, false, 0, 300000, "2.5.0", "2.5.0"));
    // Channel before source, source before asset, asset before offline,
    // offline before idle, idle before already-tried.
    TEST_ASSERT_EQUAL(Gate::WrongChannel,     gate(true, 1, false, false, false, false, 0, 300000, "2.5.0", "2.5.0"));
    TEST_ASSERT_EQUAL(Gate::SourceNotAllowed, gate(true, 0, false, false, false, false, 0, 300000, "2.5.0", "2.5.0"));
    TEST_ASSERT_EQUAL(Gate::NoAsset,          gate(true, 0, true, false, false, false, 0, 300000, "2.5.0", "2.5.0"));
    TEST_ASSERT_EQUAL(Gate::Offline,          gate(true, 0, true, true, true, false, 0, 300000, "2.5.0", "2.5.0"));
    TEST_ASSERT_EQUAL(Gate::NotIdle,          gate(true, 0, true, true, true, true, 0, 300000, "2.5.0", "2.5.0"));
}

void test_gate_idle_boundary(void) {
    TEST_ASSERT_EQUAL(Gate::NotIdle, gate(true, 0, true, true, true, true, 299999, 300000));
    TEST_ASSERT_EQUAL(Gate::Allowed, gate(true, 0, true, true, true, true, 300000, 300000));
}

void test_gate_tried_only_for_same_version(void) {
    TEST_ASSERT_EQUAL(Gate::AlreadyTried, gate(true, 0, true, true, true, true, 300000, 300000, "2.5.0", "2.5.0"));
    // A newer release after a failed one is a new attempt.
    TEST_ASSERT_EQUAL(Gate::Allowed, gate(true, 0, true, true, true, true, 300000, 300000, "2.5.1", "2.5.0"));
    // An empty latest is never "already tried".
    TEST_ASSERT_EQUAL(Gate::Allowed, gate(true, 0, true, true, true, true, 300000, 300000, "", ""));
}

void test_gate_names(void) {
    TEST_ASSERT_EQUAL_STRING("allowed", gateName(Gate::Allowed));
    TEST_ASSERT_EQUAL_STRING("panel in use", gateName(Gate::NotIdle));
    TEST_ASSERT_EQUAL_STRING("already tried", gateName(Gate::AlreadyTried));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_semver_newer);
    RUN_TEST(test_semver_equal_or_older_is_not_newer);
    RUN_TEST(test_semver_ignores_nightly_suffix);
    RUN_TEST(test_semver_empty_is_not_newer);
    RUN_TEST(test_upstream_detection);
    RUN_TEST(test_gate_allowed_when_everything_lines_up);
    RUN_TEST(test_gate_each_reason);
    RUN_TEST(test_gate_order);
    RUN_TEST(test_gate_idle_boundary);
    RUN_TEST(test_gate_tried_only_for_same_version);
    RUN_TEST(test_gate_names);
    return UNITY_END();
}
