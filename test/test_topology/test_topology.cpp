/**
 * Host tests for include/sonos_topology.h
 *
 *     pio test -e native
 *
 * Every case here is a bug that actually shipped, or the boundary next to one.
 * The v1.15.1 group regression and the satellite miscount were both found by
 * reading code and reproduced in a throwaway Python model; these are that model,
 * made permanent and pointed at the real implementation.
 */

#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "sonos_topology.h"

using namespace sonos_topology;

static const char* LIVING = "RINCON_C43875F1193A01400";
static const char* KITCHEN = "RINCON_B8E93700001400";
static const char* LOUNGE = "RINCON_B8E93700011400";

// ---------------------------------------------------------------------------
// uuidEquals — the v1.15.1 regression
// ---------------------------------------------------------------------------

void test_uuid_same_case(void) {
    TEST_ASSERT_TRUE(uuidEquals(LIVING, LIVING));
}

// The exact shape of the shipped bug: the topology parser lowercased its whole
// response, so the id it stored never matched the uppercase rinconID from the
// device description, and every membership test silently answered "no".
void test_uuid_differing_case(void) {
    std::string lower(LIVING);
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 32;
    TEST_ASSERT_FALSE(strcmp(lower.c_str(), LIVING) == 0);   // == would fail
    TEST_ASSERT_TRUE(uuidEquals(lower.c_str(), LIVING));     // uuidEquals holds
}

void test_uuid_different_ids(void) {
    TEST_ASSERT_FALSE(uuidEquals(KITCHEN, LOUNGE));
}

// A prefix must not compare equal to the longer id it is a prefix of.
void test_uuid_prefix_is_not_equal(void) {
    TEST_ASSERT_FALSE(uuidEquals("RINCON_B8E9370000", KITCHEN));
}

void test_uuid_empty_and_null(void) {
    TEST_ASSERT_TRUE(uuidEquals("", ""));
    TEST_ASSERT_FALSE(uuidEquals(KITCHEN, ""));
    TEST_ASSERT_FALSE(uuidEquals(nullptr, KITCHEN));
    TEST_ASSERT_FALSE(uuidEquals(KITCHEN, nullptr));
}

// ---------------------------------------------------------------------------
// decodeInPlace
// ---------------------------------------------------------------------------

static std::string decoded(const std::string& in) {
    std::string buf(in);
    size_t n = decodeInPlace(&buf[0], buf.size());
    buf.resize(n);
    return buf;
}

void test_decode_entities_and_lowercase(void) {
    TEST_ASSERT_EQUAL_STRING("<zonegroup id=\"a\">",
                             decoded("&lt;ZoneGroup ID=&quot;A&quot;&gt;").c_str());
}

// &amp; must resolve last. "&amp;lt;" is a literal "&lt;" in the document and
// must NOT become "<": that would fabricate a tag the payload never contained.
void test_decode_ampersand_does_not_double_decode(void) {
    TEST_ASSERT_EQUAL_STRING("&lt;", decoded("&amp;lt;").c_str());
}

void test_decode_leaves_plain_text(void) {
    TEST_ASSERT_EQUAL_STRING("kitchen & lounge", decoded("Kitchen &amp; Lounge").c_str());
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static std::string member(const char* uuid, const char* zone, int satellites = 0) {
    std::string s = std::string("<ZoneGroupMember UUID=\"") + uuid +
                    "\" ZoneName=\"" + zone +
                    "\" Location=\"http://192.168.1.9:1400/xml/device_description.xml\""
                    " Configuration=\"1\" SoftwareVersion=\"83.1-61123\" WifiEnabled=\"1\"";
    if (satellites == 0) return s + "/>";
    s += ">";
    for (int i = 0; i < satellites; i++) {
        s += std::string("<Satellite UUID=\"") + uuid + ":SAT" + std::to_string(i) +
             "\" ZoneName=\"" + zone + " (sat)\" Configuration=\"1\"/>";
    }
    return s + "</ZoneGroupMember>";
}

static std::string group(const char* coord, const std::string& membersXml) {
    return std::string("<ZoneGroup Coordinator=\"") + coord + "\" ID=\"" + coord + ":1\">" +
           membersXml + "</ZoneGroup>";
}

static std::string wrap(const std::string& groupsXml) {
    return "<ZoneGroupState><ZoneGroups>" + groupsXml + "</ZoneGroups></ZoneGroupState>";
}

struct Parsed {
    std::string buf;
    Group groups[16];
    Slice members[64];
    int count = 0;
};

static void parse(Parsed& p, const std::string& xml) {
    p.buf = xml;
    size_t n = decodeInPlace(&p.buf[0], p.buf.size());
    p.buf.resize(n);
    p.count = parseZoneGroups(p.buf.c_str(), p.buf.size(),
                              p.groups, 16, p.members, 64);
}

static bool memberIs(const Parsed& p, int idx, const char* uuid) {
    return uuidEquals(p.members[idx].ptr, p.members[idx].len, uuid, cstrLen(uuid));
}

static bool coordIs(const Parsed& p, int g, const char* uuid) {
    return uuidEquals(p.groups[g].coordinator.ptr, p.groups[g].coordinator.len,
                      uuid, cstrLen(uuid));
}

// ---------------------------------------------------------------------------
// parseZoneGroups
// ---------------------------------------------------------------------------

void test_single_standalone_speaker(void) {
    Parsed p;
    parse(p, wrap(group(LIVING, member(LIVING, "Living Room"))));
    TEST_ASSERT_EQUAL_INT(1, p.count);
    TEST_ASSERT_EQUAL_INT(1, p.groups[0].memberCount);
    TEST_ASSERT_TRUE(coordIs(p, 0, LIVING));
    TEST_ASSERT_TRUE(memberIs(p, 0, LIVING));
}

// The reported topology: everything in one group. Before the fix the Groups
// screen showed this as "12 speakers, 1 group" with the row reading Standalone.
// A real GetZoneGroupState body, captured verbatim from a Sonos Five on a live
// network (firmware 96.1-79270) rather than assembled by the fixtures above.
//
// The point is the attribute soup: Location, Icon, Configuration,
// SoftwareVersion, SWGen, MinCompatibleVersion, LegacyCompatibleVersion,
// BootSeq, TVConfigurationError, HdmiCecAvailable, VoiceConfigState and the
// rest, in the order and spelling Sonos actually emits. The hand-built member()
// helper carries five attributes; the real thing carries twenty-odd, and the
// parser has to find coordinator=" and uuid=" inside all of it.
void test_real_capture_single_speaker(void) {
    static const char kReal[] =
        "<ZoneGroups><ZoneGroup Coordinator=\"RINCON_C43875F1193A01400\" ID=\"RINCON_C43875F1193A"
        "01400:1328590192\"><ZoneGroupMember UUID=\"RINCON_C43875F1193A01400\" Location=\"http://"
        "192.168.2.36:1400/xml/device_description.xml\" ZoneName=\"Living Room\" Icon=\"\" Config"
        "uration=\"1\" SoftwareVersion=\"96.1-79270\" SWGen=\"2\" MinCompatibleVersion=\"95.0-000"
        "00\" LegacyCompatibleVersion=\"58.0-00000\" BootSeq=\"140\" TVConfigurationError=\"0\" H"
        "dmiCecAvailable=\"0\" WirelessMode=\"1\" ConnectionType=\"5\" ChannelFreq=\"5785\" Behin"
        "dWifiExtender=\"0\" WifiEnabled=\"1\" EthLink=\"0\" Orientation=\"0\" RoomCalibrationSta"
        "te=\"1\" SecureRegState=\"3\" VoiceConfigState=\"0\" MicEnabled=\"0\" HeadphoneSwapActiv"
        "e=\"0\" AirPlayEnabled=\"1\" IdleState=\"0\" MoreInfo=\"\" SSLPort=\"1443\" HHSSLPort=\""
        "1843\"/></ZoneGroup></ZoneGroups>";

    Parsed p;
    parse(p, std::string("<ZoneGroupState>") + kReal + "</ZoneGroupState>");

    TEST_ASSERT_EQUAL_INT(1, p.count);
    TEST_ASSERT_EQUAL_INT(1, p.groups[0].memberCount);
    TEST_ASSERT_TRUE(coordIs(p, 0, LIVING));
    TEST_ASSERT_TRUE(memberIs(p, 0, LIVING));
}

void test_twelve_speakers_in_one_group(void) {
    std::string members;
    char uuid[64];
    for (int i = 0; i < 12; i++) {
        snprintf(uuid, sizeof(uuid), "RINCON_B8E937%02d01400", i);
        members += member(uuid, "Room");
    }
    Parsed p;
    parse(p, wrap(group("RINCON_B8E9370001400", members)));

    TEST_ASSERT_EQUAL_INT(1, p.count);
    TEST_ASSERT_EQUAL_INT(12, p.groups[0].memberCount);
    TEST_ASSERT_TRUE(memberIs(p, 0, "RINCON_B8E9370001400"));
    TEST_ASSERT_TRUE(memberIs(p, 11, "RINCON_B8E9371101400"));
}

void test_mixed_grouped_and_standalone(void) {
    std::string xml = group(KITCHEN, member(KITCHEN, "Kitchen") + member(LOUNGE, "Lounge")) +
                      group(LIVING, member(LIVING, "Living Room"));
    Parsed p;
    parse(p, wrap(xml));

    TEST_ASSERT_EQUAL_INT(2, p.count);
    TEST_ASSERT_EQUAL_INT(2, p.groups[0].memberCount);
    TEST_ASSERT_TRUE(coordIs(p, 0, KITCHEN));
    TEST_ASSERT_EQUAL_INT(1, p.groups[1].memberCount);
    TEST_ASSERT_TRUE(coordIs(p, 1, LIVING));

    // Members of group 1 start after group 0's, not at zero.
    TEST_ASSERT_EQUAL_INT(2, p.groups[1].firstMember);
    TEST_ASSERT_TRUE(memberIs(p, p.groups[1].firstMember, LIVING));
}

// A standalone home theatre: one ZoneGroupMember with nested <Satellite> children
// for its sub and surrounds. Counting every uuid=" made this a 4-speaker group,
// which put a standalone speaker onto the group volume path and printed
// "4 speakers in group" on screen.
void test_satellites_are_not_members(void) {
    Parsed p;
    parse(p, wrap(group(LIVING, member(LIVING, "Beam", /*satellites=*/3))));
    TEST_ASSERT_EQUAL_INT(1, p.count);
    TEST_ASSERT_EQUAL_INT(1, p.groups[0].memberCount);
    TEST_ASSERT_TRUE(memberIs(p, 0, LIVING));
}

// The coordinator is identified by id, never by position, so a group whose
// coordinator is not its first member still resolves correctly.
void test_coordinator_need_not_be_first_member(void) {
    Parsed p;
    parse(p, wrap(group(LOUNGE, member(KITCHEN, "Kitchen") + member(LOUNGE, "Lounge"))));
    TEST_ASSERT_EQUAL_INT(2, p.groups[0].memberCount);
    TEST_ASSERT_TRUE(coordIs(p, 0, LOUNGE));
    TEST_ASSERT_TRUE(memberIs(p, 0, KITCHEN));
    TEST_ASSERT_TRUE(memberIs(p, 1, LOUNGE));
}

// <ZoneGroups> (plural) must not be mistaken for a <ZoneGroup> - the open tag is
// matched with its trailing space and the close tag with its '>'.
void test_zonegroups_wrapper_is_not_a_group(void) {
    Parsed p;
    parse(p, wrap(""));
    TEST_ASSERT_EQUAL_INT(0, p.count);
}

// A response cut short mid-group must yield the groups that did close, not read
// past the buffer.
void test_truncated_response_is_safe(void) {
    std::string xml = wrap(group(KITCHEN, member(KITCHEN, "Kitchen")) +
                           group(LIVING, member(LIVING, "Living Room")));
    xml = xml.substr(0, xml.size() - 40);          // chop the tail
    Parsed p;
    parse(p, xml);
    TEST_ASSERT_EQUAL_INT(1, p.count);
    TEST_ASSERT_TRUE(coordIs(p, 0, KITCHEN));
}

void test_empty_input_is_safe(void) {
    Group g[2];
    Slice m[2];
    TEST_ASSERT_EQUAL_INT(0, parseZoneGroups("", 0, g, 2, m, 2));
    TEST_ASSERT_EQUAL_INT(0, parseZoneGroups(nullptr, 0, g, 2, m, 2));
}

// Caller buffers are honoured: a topology larger than the arrays must stop, not
// overrun. MAX_SONOS_DEVICES is 32, so a 40-speaker household is reachable.
void test_respects_buffer_limits(void) {
    std::string members;
    char uuid[64];
    for (int i = 0; i < 40; i++) {
        snprintf(uuid, sizeof(uuid), "RINCON_AAAA%02d01400", i);
        members += member(uuid, "Room");
    }
    std::string buf = wrap(group("RINCON_AAAA0001400", members));
    size_t n = decodeInPlace(&buf[0], buf.size());

    Group g[2];
    Slice m[8];
    int count = parseZoneGroups(buf.c_str(), n, g, 2, m, 8);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_TRUE(g[0].memberCount <= 8);
}


// ---------------------------------------------------------------------------
// parseMembers — hidden rooms (#211) and the speaker refresh
// ---------------------------------------------------------------------------
static std::string sv(const Slice& s) { return s.ptr ? std::string(s.ptr, s.len) : std::string(); }

// A Beam with a bonded Sub (a Satellite child) beside a stereo pair whose second
// speaker is Invisible, and a standalone room. Entity-decoded, case kept.
static const char* HIDDEN_FIXTURE =
    "<ZoneGroups>"
    "<ZoneGroup Coordinator=\"RINCON_BEAM01400\" ID=\"RINCON_BEAM01400:1\">"
    "<ZoneGroupMember UUID=\"RINCON_BEAM01400\" Location=\"http://192.168.1.20:1400/xml/device_description.xml\" ZoneName=\"Living Room\">"
    "<Satellite UUID=\"RINCON_SUB01400\" Location=\"http://192.168.1.21:1400/xml/device_description.xml\" ZoneName=\"Living Room\"/>"
    "</ZoneGroupMember>"
    "</ZoneGroup>"
    "<ZoneGroup Coordinator=\"RINCON_L01400\" ID=\"RINCON_L01400:2\">"
    "<ZoneGroupMember UUID=\"RINCON_L01400\" Location=\"http://192.168.1.30:1400/xml/device_description.xml\" ZoneName=\"Office\"/>"
    "<ZoneGroupMember UUID=\"RINCON_R01400\" Location=\"http://192.168.1.31:1400/xml/device_description.xml\" ZoneName=\"Office\" Invisible=\"1\"/>"
    "</ZoneGroup>"
    "<ZoneGroup Coordinator=\"RINCON_K01400\" ID=\"RINCON_K01400:3\">"
    "<ZoneGroupMember UUID=\"RINCON_K01400\" Location=\"http://192.168.1.40:1400/xml/device_description.xml\" ZoneName=\"Kitchen\" Invisible=\"0\"/>"
    "</ZoneGroup>"
    "</ZoneGroups>";

void test_members_flag_satellites_and_invisible(void) {
    Member m[16];
    int n = parseMembers(HIDDEN_FIXTURE, strlen(HIDDEN_FIXTURE), m, 16);
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_FALSE(m[0].hidden);                       // Beam
    TEST_ASSERT_TRUE(m[1].hidden);                        // its Sub (Satellite)
    TEST_ASSERT_FALSE(m[2].hidden);                       // Office left
    TEST_ASSERT_TRUE(m[3].hidden);                        // Office right (Invisible="1")
    TEST_ASSERT_FALSE(m[4].hidden);                       // Invisible="0" is visible
    TEST_ASSERT_TRUE(isHidden(m, n, "rincon_sub01400", 15));   // case-blind lookup
    TEST_ASSERT_FALSE(isHidden(m, n, "RINCON_BEAM01400", 16));
}

void test_members_read_name_and_address(void) {
    Member m[16];
    int n = parseMembers(HIDDEN_FIXTURE, strlen(HIDDEN_FIXTURE), m, 16);
    TEST_ASSERT_TRUE(n >= 3);
    TEST_ASSERT_EQUAL_STRING("Living Room", sv(m[0].name).c_str());
    TEST_ASSERT_EQUAL_STRING("RINCON_L01400", sv(m[2].uuid).c_str());
    char host[24];
    TEST_ASSERT_TRUE(locationHost(m[2].location, host, sizeof(host)));
    TEST_ASSERT_EQUAL_STRING("192.168.1.30", host);
}

// The same document after the group parser's lowercase pass: names are lost to
// case, but the hidden flags and uuids must still resolve.
void test_members_work_on_lowercased_buffer(void) {
    std::string lower(HIDDEN_FIXTURE);
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 32;
    Member m[16];
    int n = parseMembers(lower.c_str(), lower.size(), m, 16);
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_TRUE(isHidden(m, n, "RINCON_SUB01400", 15));
    TEST_ASSERT_TRUE(isHidden(m, n, "RINCON_R01400", 13));
    TEST_ASSERT_FALSE(isHidden(m, n, "RINCON_K01400", 13));
}

void test_members_decode_keeps_case(void) {
    std::string enc = "&lt;ZoneGroupMember UUID=&quot;RINCON_A1400&quot; ZoneName=&quot;Caf&amp;apos;s Room&quot;/&gt;";
    std::vector<char> buf(enc.begin(), enc.end());
    size_t len = decodeEntitiesInPlace(buf.data(), buf.size(), false);
    Member m[4];
    int n = parseMembers(buf.data(), len, m, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("RINCON_A1400", sv(m[0].uuid).c_str());
    TEST_ASSERT_EQUAL_STRING("Caf&apos;s Room", sv(m[0].name).c_str());   // entity left for decodeHTML
}

void test_members_ignore_other_tags_and_respect_limit(void) {
    const char* x = "<ZoneGroup Coordinator=\"a\"><ZoneGroupMemberXYZ UUID=\"x\"/>"
                    "<ZoneGroupMember UUID=\"b\"/><ZoneGroupMember UUID=\"c\"/></ZoneGroup>";
    Member m[1];
    TEST_ASSERT_EQUAL_INT(1, parseMembers(x, strlen(x), m, 1));
    TEST_ASSERT_EQUAL_STRING("b", sv(m[0].uuid).c_str());
    TEST_ASSERT_EQUAL_INT(0, parseMembers(nullptr, 0, m, 1));
    TEST_ASSERT_EQUAL_INT(0, parseMembers("<ZoneGroupMember UUID=\"unterminated", 35, m, 1));
}

// The background refresh parses members from the case-kept buffer, then
// lowercases the SAME buffer and hands it to the group parser instead of making a
// second GetZoneGroupState call. Both readings must agree.
void test_one_payload_serves_members_and_groups(void) {
    std::string enc(HIDDEN_FIXTURE);
    std::vector<char> buf(enc.begin(), enc.end());
    size_t len = decodeEntitiesInPlace(buf.data(), buf.size(), false);
    Member m[16];
    int n = parseMembers(buf.data(), len, m, 16);
    TEST_ASSERT_EQUAL_INT(5, n);
    std::string firstName = sv(m[0].name);               // copied out before lowercasing
    for (size_t i = 0; i < len; i++) if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] += 32;
    Group g[8];
    Slice s[16];
    int groups = parseZoneGroups(buf.data(), len, g, 8, s, 16);
    TEST_ASSERT_EQUAL_INT(3, groups);
    TEST_ASSERT_EQUAL_INT(1, g[0].memberCount);          // Beam; its Sub is a Satellite, not a member
    TEST_ASSERT_EQUAL_INT(2, g[1].memberCount);          // stereo pair: two members, one visible room
    TEST_ASSERT_EQUAL_STRING("Living Room", firstName.c_str());
}

void test_location_host_rejects_garbage(void) {
    char host[24];
    Slice a = {"nonsense", 8};
    TEST_ASSERT_FALSE(locationHost(a, host, sizeof(host)));
    Slice b = {nullptr, 0};
    TEST_ASSERT_FALSE(locationHost(b, host, sizeof(host)));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_uuid_same_case);
    RUN_TEST(test_uuid_differing_case);
    RUN_TEST(test_uuid_different_ids);
    RUN_TEST(test_uuid_prefix_is_not_equal);
    RUN_TEST(test_uuid_empty_and_null);

    RUN_TEST(test_decode_entities_and_lowercase);
    RUN_TEST(test_decode_ampersand_does_not_double_decode);
    RUN_TEST(test_decode_leaves_plain_text);

    RUN_TEST(test_single_standalone_speaker);
    RUN_TEST(test_real_capture_single_speaker);
    RUN_TEST(test_twelve_speakers_in_one_group);
    RUN_TEST(test_mixed_grouped_and_standalone);
    RUN_TEST(test_satellites_are_not_members);
    RUN_TEST(test_coordinator_need_not_be_first_member);
    RUN_TEST(test_zonegroups_wrapper_is_not_a_group);
    RUN_TEST(test_truncated_response_is_safe);
    RUN_TEST(test_empty_input_is_safe);
    RUN_TEST(test_respects_buffer_limits);
    RUN_TEST(test_members_flag_satellites_and_invisible);
    RUN_TEST(test_members_read_name_and_address);
    RUN_TEST(test_members_work_on_lowercased_buffer);
    RUN_TEST(test_members_decode_keeps_case);
    RUN_TEST(test_members_ignore_other_tags_and_respect_limit);
    RUN_TEST(test_one_payload_serves_members_and_groups);
    RUN_TEST(test_location_host_rejects_garbage);

    return UNITY_END();
}
