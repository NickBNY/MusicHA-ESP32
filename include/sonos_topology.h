#pragma once
/**
 * Sonos ZoneGroupState parsing — pure, framework-free, allocation-free.
 *
 * Split out of sonos_controller.cpp so it can be compiled and tested on the host.
 * Nothing here touches Arduino, FreeRTOS, LVGL or the network: it is string
 * arithmetic over a buffer, which is exactly the part that shipped bugs.
 *
 * Two of the three defects in the v1.15.1/v1.15.2 group work lived in this logic:
 *   - UUIDs stored in the case the payload happened to use, then compared with ==
 *   - member counting that walked every uuid=" and so counted <Satellite> children
 * Both are now covered by test/test_topology.
 *
 * Caller supplies the buffers; this code never allocates.
 */

#include <stddef.h>
#include <stdint.h>

namespace sonos_topology {

// ---------------------------------------------------------------------------
// UUID comparison
// ---------------------------------------------------------------------------
// RINCON ids reach us in inconsistent case: the device description spells them
// uppercase, GetZoneGroupState is lowercased wholesale before parsing (firmware
// varies), and the x-rincon: transport URI carries whatever the speaker sent.
// ALWAYS compare through this, never with ==. Ids are [A-Za-z0-9_:] by
// construction, so plain ASCII folding is correct and locale-free.
inline bool uuidEquals(const char* a, size_t na, const char* b, size_t nb) {
    if (na != nb) return false;
    for (size_t i = 0; i < na; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

inline size_t cstrLen(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

inline bool uuidEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    return uuidEquals(a, cstrLen(a), b, cstrLen(b));
}

// ---------------------------------------------------------------------------
// Parsed shape
// ---------------------------------------------------------------------------
// Slices point INTO the caller's buffer; they are not null-terminated and do not
// outlive it.
struct Slice {
    const char* ptr;
    uint16_t    len;
};

struct Group {
    Slice coordinator;
    uint16_t firstMember;   // index into the members array
    uint16_t memberCount;
};

// ---------------------------------------------------------------------------
// parseZoneGroups
// ---------------------------------------------------------------------------
// `buf` must already be entity-decoded and ASCII-lowercased (see decodeInPlace).
//
// Members are the <ZoneGroupMember> elements only. A home-theatre member carries
// nested <Satellite UUID="..."/> children for its sub and surrounds; scanning for
// uuid=" instead counted those, which made a STANDALONE Beam + Sub + 2 surrounds
// report as a four-speaker group. Each element's attributes are read up to its
// first '>', so satellites are never mistaken for members.
//
// Returns the number of groups written (never more than maxGroups).
inline int parseZoneGroups(const char* buf, size_t len,
                           Group* groups, int maxGroups,
                           Slice* members, int maxMembers) {
    if (!buf || !groups || !members || maxGroups <= 0 || maxMembers <= 0) return 0;

    const char kGroupOpen[]  = "<zonegroup ";
    const char kGroupClose[] = "</zonegroup>";
    const char kMemberOpen[] = "<zonegroupmember ";
    const char kCoordAttr[]  = "coordinator=\"";
    const char kUuidAttr[]   = "uuid=\"";

    auto find = [&](size_t from, const char* needle, size_t nlen) -> size_t {
        if (nlen == 0 || from >= len || len - from < nlen) return (size_t)-1;
        for (size_t i = from; i + nlen <= len; i++) {
            size_t j = 0;
            while (j < nlen && buf[i + j] == needle[j]) j++;
            if (j == nlen) return i;
        }
        return (size_t)-1;
    };

    int groupCount = 0;
    int memberTotal = 0;
    size_t scan = 0;

    while (groupCount < maxGroups) {
        size_t gs = find(scan, kGroupOpen, sizeof(kGroupOpen) - 1);
        if (gs == (size_t)-1) break;
        size_t ge = find(gs, kGroupClose, sizeof(kGroupClose) - 1);
        if (ge == (size_t)-1) break;                 // truncated response
        scan = ge + sizeof(kGroupClose) - 1;

        // Coordinator attribute on the <ZoneGroup> tag.
        size_t cs = find(gs, kCoordAttr, sizeof(kCoordAttr) - 1);
        if (cs == (size_t)-1 || cs > ge) continue;
        cs += sizeof(kCoordAttr) - 1;
        size_t ce = cs;
        while (ce < ge && buf[ce] != '"') ce++;
        if (ce <= cs) continue;

        Group g;
        g.coordinator.ptr = buf + cs;
        g.coordinator.len = (uint16_t)(ce - cs);
        g.firstMember     = (uint16_t)memberTotal;
        g.memberCount     = 0;

        // <ZoneGroupMember> elements within this group only.
        size_t ms = gs;
        while (memberTotal < maxMembers) {
            size_t mstart = find(ms, kMemberOpen, sizeof(kMemberOpen) - 1);
            if (mstart == (size_t)-1 || mstart > ge) break;
            size_t mend = mstart;
            while (mend < ge && buf[mend] != '>') mend++;   // attributes end here
            ms = mend;

            size_t us = find(mstart, kUuidAttr, sizeof(kUuidAttr) - 1);
            if (us == (size_t)-1 || us > mend) continue;
            us += sizeof(kUuidAttr) - 1;
            size_t ue = us;
            while (ue < mend && buf[ue] != '"') ue++;
            if (ue <= us) continue;

            members[memberTotal].ptr = buf + us;
            members[memberTotal].len = (uint16_t)(ue - us);
            memberTotal++;
            g.memberCount++;
        }

        groups[groupCount++] = g;
    }
    return groupCount;
}

// ---------------------------------------------------------------------------
// decodeEntitiesInPlace / decodeInPlace
// ---------------------------------------------------------------------------
// The ZoneGroupState payload arrives entity-encoded inside the SOAP body.
// Decodes &lt; &gt; &quot; &amp; in one left-to-right pass over the buffer and
// returns the new length. With `lower` set it also ASCII-lowercases, which is what
// the group parser wants (RINCON comparison is case-blind). Without it the case is
// kept, which the speaker refresh needs: room names are shown to the user.
//
// One pass, not four String::replace() calls: &amp; MUST resolve last or "&amp;lt;"
// double-decodes into a tag that was never in the document. Scanning once removes
// the ordering hazard entirely rather than relying on the calls staying in order.
inline size_t decodeEntitiesInPlace(char* buf, size_t len, bool lower) {
    if (!buf) return 0;
    size_t w = 0;
    for (size_t r = 0; r < len; ) {
        if (buf[r] == '&') {
            size_t remain = len - r;
            if (remain >= 4 && buf[r+1] == 'l' && buf[r+2] == 't' && buf[r+3] == ';') {
                buf[w++] = '<'; r += 4; continue;
            }
            if (remain >= 4 && buf[r+1] == 'g' && buf[r+2] == 't' && buf[r+3] == ';') {
                buf[w++] = '>'; r += 4; continue;
            }
            if (remain >= 6 && buf[r+1] == 'q' && buf[r+2] == 'u' &&
                buf[r+3] == 'o' && buf[r+4] == 't' && buf[r+5] == ';') {
                buf[w++] = '"'; r += 6; continue;
            }
            if (remain >= 5 && buf[r+1] == 'a' && buf[r+2] == 'm' &&
                buf[r+3] == 'p' && buf[r+4] == ';') {
                buf[w++] = '&'; r += 5; continue;
            }
        }
        char c = buf[r++];
        if (lower && c >= 'A' && c <= 'Z') c += 32;
        buf[w++] = c;
    }
    return w;
}

inline size_t decodeInPlace(char* buf, size_t len) {
    return decodeEntitiesInPlace(buf, len, true);
}

// ---------------------------------------------------------------------------
// parseMembers
// ---------------------------------------------------------------------------
// Every speaker the system reports, with the three attributes the panel needs to
// keep its own list in step with the Sonos system:
//   uuid      the RINCON id
//   name      ZoneName, the room name
//   location  the device description URL, which carries the speaker's address
//
// `hidden` is true for anything the Sonos app does not list as a room of its own:
//   - a <Satellite> child, which is how a home-theatre sub and surrounds appear
//   - a <ZoneGroupMember> flagged Invisible="1", which is how the second half of a
//     stereo pair and a bonded Sub / Sub Mini appear (issue #211)
// Both used to come back from SSDP as ordinary speakers, so a Sub showed up in the
// picker beside the room it belongs to.
//
// Attribute NAMES are matched case-blind, so this works on a buffer that was
// lowercased for RINCON comparison and on one that kept its case. Values are
// returned as found. Slices point into `buf` and are not null-terminated.
struct Member {
    Slice uuid;
    Slice name;
    Slice location;
    bool  hidden;
};

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Reads name="value" inside [from, to). Returns false when absent.
inline bool attrValue(const char* buf, size_t from, size_t to,
                      const char* name, Slice* out) {
    const size_t nlen = cstrLen(name);
    if (nlen == 0) return false;
    for (size_t i = from; i + nlen + 2 <= to; i++) {
        // An attribute starts after whitespace, so "zonename" is not found inside
        // "xzonename" and "uuid" is not found inside a value.
        if (i > from && buf[i - 1] != ' ' && buf[i - 1] != '\t' && buf[i - 1] != '\n') continue;
        size_t j = 0;
        while (j < nlen && lowerAscii(buf[i + j]) == name[j]) j++;
        if (j != nlen || buf[i + nlen] != '=' || buf[i + nlen + 1] != '"') continue;
        const size_t vs = i + nlen + 2;
        size_t ve = vs;
        while (ve < to && buf[ve] != '"') ve++;
        if (ve >= to) return false;           // unterminated
        out->ptr = buf + vs;
        out->len = (uint16_t)(ve - vs);
        return true;
    }
    return false;
}

inline bool sliceIs(const Slice& s, const char* lit) {
    const size_t n = cstrLen(lit);
    if (s.len != n) return false;
    for (size_t i = 0; i < n; i++) if (lowerAscii(s.ptr[i]) != lowerAscii(lit[i])) return false;
    return true;
}

// Returns the number of members written (never more than maxOut). `name` and
// `location` have len 0 / ptr null when the tag did not carry them.
inline int parseMembers(const char* buf, size_t len, Member* out, int maxOut) {
    if (!buf || !out || maxOut <= 0) return 0;
    int n = 0;
    size_t i = 0;
    while (i < len && n < maxOut) {
        // Next '<' that opens a member or a satellite.
        if (buf[i] != '<') { i++; continue; }
        const bool member = i + 17 <= len &&
            lowerAscii(buf[i+1]) == 'z' && lowerAscii(buf[i+2]) == 'o' && lowerAscii(buf[i+3]) == 'n' &&
            lowerAscii(buf[i+4]) == 'e' && lowerAscii(buf[i+5]) == 'g' && lowerAscii(buf[i+6]) == 'r' &&
            lowerAscii(buf[i+7]) == 'o' && lowerAscii(buf[i+8]) == 'u' && lowerAscii(buf[i+9]) == 'p' &&
            lowerAscii(buf[i+10]) == 'm' && lowerAscii(buf[i+11]) == 'e' && lowerAscii(buf[i+12]) == 'm' &&
            lowerAscii(buf[i+13]) == 'b' && lowerAscii(buf[i+14]) == 'e' && lowerAscii(buf[i+15]) == 'r' &&
            (buf[i+16] == ' ' || buf[i+16] == '\n' || buf[i+16] == '\t');
        const bool satellite = !member && i + 11 <= len &&
            lowerAscii(buf[i+1]) == 's' && lowerAscii(buf[i+2]) == 'a' && lowerAscii(buf[i+3]) == 't' &&
            lowerAscii(buf[i+4]) == 'e' && lowerAscii(buf[i+5]) == 'l' && lowerAscii(buf[i+6]) == 'l' &&
            lowerAscii(buf[i+7]) == 'i' && lowerAscii(buf[i+8]) == 't' && lowerAscii(buf[i+9]) == 'e' &&
            (buf[i+10] == ' ' || buf[i+10] == '\n' || buf[i+10] == '\t');
        if (!member && !satellite) { i++; continue; }

        size_t end = i;
        while (end < len && buf[end] != '>') end++;      // attributes end at the first '>'
        Member m;
        m.uuid = {nullptr, 0}; m.name = {nullptr, 0}; m.location = {nullptr, 0};
        m.hidden = satellite;
        if (attrValue(buf, i, end, "uuid", &m.uuid)) {
            attrValue(buf, i, end, "zonename", &m.name);
            attrValue(buf, i, end, "location", &m.location);
            Slice inv = {nullptr, 0};
            if (member && attrValue(buf, i, end, "invisible", &inv) && sliceIs(inv, "1"))
                m.hidden = true;
            out[n++] = m;
        }
        i = end;
    }
    return n;
}

// True when `uuid` names a hidden member in a parseMembers() result.
inline bool isHidden(const Member* members, int count, const char* uuid, size_t ulen) {
    for (int i = 0; i < count; i++)
        if (members[i].hidden && uuidEquals(members[i].uuid.ptr, members[i].uuid.len, uuid, ulen))
            return true;
    return false;
}

// The dotted address inside "http://192.168.1.13:1400/xml/device_description.xml".
// Writes it null-terminated into `out` and returns true when one was found.
inline bool locationHost(const Slice& loc, char* out, size_t outSize) {
    if (!loc.ptr || outSize < 8) return false;
    size_t i = 0;
    while (i + 1 < loc.len && !(loc.ptr[i] == '/' && loc.ptr[i + 1] == '/')) i++;
    if (i + 1 >= loc.len) return false;
    i += 2;
    size_t w = 0;
    while (i < loc.len && loc.ptr[i] != ':' && loc.ptr[i] != '/' && w + 1 < outSize) out[w++] = loc.ptr[i++];
    out[w] = '\0';
    return w >= 7;                             // "1.1.1.1"
}

}  // namespace sonos_topology
