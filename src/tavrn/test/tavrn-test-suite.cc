/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN: Topology Aware Vicinity-Reactive Network
 * Comprehensive test suite for the TAVRN module.
 *
 * Modeled after ns-3 AODV test suite (src/aodv/test/aodv-test-suite.cc).
 * Tests all major sub-components: packet headers, GTT, routing table,
 * request queue, neighbors, and ID cache.
 */

#include "ns3/tavrn-gtt.h"
#include "ns3/tavrn-id-cache.h"
#include "ns3/tavrn-neighbor.h"
#include "ns3/tavrn-packet.h"
#include "ns3/tavrn-rqueue.h"
#include "ns3/tavrn-rtable.h"

#include "ns3/ipv4-route.h"
#include "ns3/simulator.h"
#include "ns3/test.h"

namespace ns3
{
namespace tavrn
{

// ============================================================================
// 1. TypeHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TypeHeader — message type identification
 */
struct TypeHeaderTest : public TestCase
{
    TypeHeaderTest()
        : TestCase("TAVRN TypeHeader")
    {
    }

    void DoRun() override
    {
        // Test all valid message types
        MessageType types[] = {
            TAVRN_E_RREQ,
            TAVRN_E_RREP,
            TAVRN_E_RERR,
            TAVRN_HELLO,
            TAVRN_SYNC_OFFER,
            TAVRN_SYNC_PULL,
            TAVRN_SYNC_DATA,
            TAVRN_TC_UPDATE,
            TAVRN_E_RREP_ACK,
        };

        for (auto t : types)
        {
            TypeHeader h(t);
            NS_TEST_EXPECT_MSG_EQ(h.IsValid(), true, "Header with type " << (int)t << " is valid");
            NS_TEST_EXPECT_MSG_EQ(h.Get(), t, "Header type matches");

            // Round-trip serialization
            Ptr<Packet> p = Create<Packet>();
            p->AddHeader(h);
            TypeHeader h2(TAVRN_E_RREQ); // different initial type
            uint32_t bytes = p->RemoveHeader(h2);
            NS_TEST_EXPECT_MSG_EQ(bytes, 1, "Type header is 1 byte long");
            NS_TEST_EXPECT_MSG_EQ(h, h2, "Round trip serialization works for type " << (int)t);
        }

        // Verify serialized size
        TypeHeader h(TAVRN_E_RREQ);
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 1, "TypeHeader serialized size is 1 byte");
    }
};

// ============================================================================
// 2. ERreqHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for ERreqHeader — Enhanced Route Request
 */
struct ERreqHeaderTest : public TestCase
{
    ERreqHeaderTest()
        : TestCase("TAVRN E_RREQ")
    {
    }

    void DoRun() override
    {
        ERreqHeader h(/*flags*/ 0,
                      /*hopCount*/ 6,
                      /*requestID*/ 1,
                      /*dst*/ Ipv4Address("1.2.3.4"),
                      /*dstSeqNo*/ 40,
                      /*origin*/ Ipv4Address("4.3.2.1"),
                      /*originSeqNo*/ 10);
        NS_TEST_EXPECT_MSG_EQ(h.GetGratuitousRrep(), false, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDestinationOnly(), false, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetUnknownSeqno(), false, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetHopCount(), 6, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetId(), 1, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDst(), Ipv4Address("1.2.3.4"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDstSeqno(), 40, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetOrigin(), Ipv4Address("4.3.2.1"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetOriginSeqno(), 10, "trivial");

        // Test flag setters
        h.SetGratuitousRrep(true);
        NS_TEST_EXPECT_MSG_EQ(h.GetGratuitousRrep(), true, "trivial");
        h.SetDestinationOnly(true);
        NS_TEST_EXPECT_MSG_EQ(h.GetDestinationOnly(), true, "trivial");
        h.SetUnknownSeqno(true);
        NS_TEST_EXPECT_MSG_EQ(h.GetUnknownSeqno(), true, "trivial");

        // Test field setters
        h.SetDst(Ipv4Address("1.1.1.1"));
        NS_TEST_EXPECT_MSG_EQ(h.GetDst(), Ipv4Address("1.1.1.1"), "trivial");
        h.SetDstSeqno(5);
        NS_TEST_EXPECT_MSG_EQ(h.GetDstSeqno(), 5, "trivial");
        h.SetHopCount(7);
        NS_TEST_EXPECT_MSG_EQ(h.GetHopCount(), 7, "trivial");
        h.SetId(55);
        NS_TEST_EXPECT_MSG_EQ(h.GetId(), 55, "trivial");
        h.SetOrigin(Ipv4Address("4.4.4.4"));
        NS_TEST_EXPECT_MSG_EQ(h.GetOrigin(), Ipv4Address("4.4.4.4"), "trivial");
        h.SetOriginSeqno(23);
        NS_TEST_EXPECT_MSG_EQ(h.GetOriginSeqno(), 23, "trivial");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        ERreqHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, 23, "E_RREQ is 23 bytes long (AODV wire compat)");
        NS_TEST_EXPECT_MSG_EQ(h, h2, "Round trip serialization works");
        NS_TEST_EXPECT_MSG_EQ(bytes,
                              h.GetSerializedSize(),
                              "(De)Serialized size match");
    }
};

// ============================================================================
// 3. ERrepHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for ERrepHeader — Enhanced Route Reply
 */
struct ERrepHeaderTest : public TestCase
{
    ERrepHeaderTest()
        : TestCase("TAVRN E_RREP")
    {
    }

    void DoRun() override
    {
        ERrepHeader h(/*hopCount*/ 12,
                      /*dst*/ Ipv4Address("1.2.3.4"),
                      /*dstSeqNo*/ 2,
                      /*origin*/ Ipv4Address("4.3.2.1"),
                      /*lifetime*/ Seconds(3));
        NS_TEST_EXPECT_MSG_EQ(h.GetHopCount(), 12, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDst(), Ipv4Address("1.2.3.4"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDstSeqno(), 2, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetOrigin(), Ipv4Address("4.3.2.1"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetLifeTime(), Seconds(3), "trivial");

        // Test field setters
        h.SetDst(Ipv4Address("1.1.1.1"));
        NS_TEST_EXPECT_MSG_EQ(h.GetDst(), Ipv4Address("1.1.1.1"), "trivial");
        h.SetDstSeqno(123);
        NS_TEST_EXPECT_MSG_EQ(h.GetDstSeqno(), 123, "trivial");
        h.SetOrigin(Ipv4Address("4.4.4.4"));
        NS_TEST_EXPECT_MSG_EQ(h.GetOrigin(), Ipv4Address("4.4.4.4"), "trivial");
        h.SetLifeTime(MilliSeconds(1200));
        NS_TEST_EXPECT_MSG_EQ(h.GetLifeTime(), MilliSeconds(1200), "trivial");
        h.SetHopCount(15);
        NS_TEST_EXPECT_MSG_EQ(h.GetHopCount(), 15, "trivial");

        // Test ACK flag
        h.SetAckRequired(true);
        NS_TEST_EXPECT_MSG_EQ(h.GetAckRequired(), true, "trivial");
        h.SetAckRequired(false);
        NS_TEST_EXPECT_MSG_EQ(h.GetAckRequired(), false, "trivial");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        ERrepHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, 19, "E_RREP is 19 bytes long (AODV wire compat)");
        NS_TEST_EXPECT_MSG_EQ(h, h2, "Round trip serialization works");
        NS_TEST_EXPECT_MSG_EQ(bytes,
                              h.GetSerializedSize(),
                              "(De)Serialized size match");
    }
};

// ============================================================================
// 4. ERerrHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for ERerrHeader — Enhanced Route Error
 */
struct ERerrHeaderTest : public TestCase
{
    ERerrHeaderTest()
        : TestCase("TAVRN E_RERR")
    {
    }

    void DoRun() override
    {
        ERerrHeader h;

        // Test NoDelete flag
        h.SetNoDelete(true);
        NS_TEST_EXPECT_MSG_EQ(h.GetNoDelete(), true, "trivial");

        // Test AddUnDestination
        Ipv4Address dst("1.2.3.4");
        NS_TEST_EXPECT_MSG_EQ(h.AddUnDestination(dst, 12), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDestCount(), 1, "trivial");

        // Adding same destination again updates seqNo but still returns true
        NS_TEST_EXPECT_MSG_EQ(h.AddUnDestination(dst, 13), true, "trivial");

        // Add second destination
        Ipv4Address dst2("4.3.2.1");
        NS_TEST_EXPECT_MSG_EQ(h.AddUnDestination(dst2, 12), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDestCount(), 2, "trivial");

        // Test RemoveUnDestination
        std::pair<Ipv4Address, uint32_t> un;
        NS_TEST_EXPECT_MSG_EQ(h.RemoveUnDestination(un), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetDestCount(), 1, "One entry removed");

        // Verify variable-length serialization: 3 + 8*N bytes
        ERerrHeader h3;
        NS_TEST_EXPECT_MSG_EQ(h3.GetSerializedSize(), 3, "Empty RERR is 3 bytes");
        h3.AddUnDestination(Ipv4Address("10.0.0.1"), 1);
        NS_TEST_EXPECT_MSG_EQ(h3.GetSerializedSize(), 11, "RERR with 1 dest is 3 + 8 = 11 bytes");
        h3.AddUnDestination(Ipv4Address("10.0.0.2"), 2);
        NS_TEST_EXPECT_MSG_EQ(h3.GetSerializedSize(),
                              19,
                              "RERR with 2 dests is 3 + 16 = 19 bytes");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        ERerrHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, h.GetSerializedSize(), "(De)Serialized size match");
        NS_TEST_EXPECT_MSG_EQ(h, h2, "Round trip serialization works");
    }
};

// ============================================================================
// 5. HelloHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for HelloHeader — Node join announcement
 */
struct HelloHeaderTest : public TestCase
{
    HelloHeaderTest()
        : TestCase("TAVRN HELLO")
    {
    }

    void DoRun() override
    {
        HelloHeader h(Ipv4Address("10.0.0.1"), 42, true);
        NS_TEST_EXPECT_MSG_EQ(h.GetNodeAddr(), Ipv4Address("10.0.0.1"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetSeqNo(), 42, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetIsNew(), true, "trivial");

        // Test setters
        h.SetNodeAddr(Ipv4Address("10.0.0.2"));
        NS_TEST_EXPECT_MSG_EQ(h.GetNodeAddr(), Ipv4Address("10.0.0.2"), "trivial");
        h.SetSeqNo(100);
        NS_TEST_EXPECT_MSG_EQ(h.GetSeqNo(), 100, "trivial");
        h.SetIsNew(false);
        NS_TEST_EXPECT_MSG_EQ(h.GetIsNew(), false, "trivial");

        // Verify serialized size: 4B nodeAddr + 4B seqNo + 1B flags = 9
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 9, "HELLO is 9 bytes");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        HelloHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, 9, "HELLO deserialized to 9 bytes");
        NS_TEST_EXPECT_MSG_EQ(h2.GetNodeAddr(), Ipv4Address("10.0.0.2"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h2.GetSeqNo(), 100, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h2.GetIsNew(), false, "trivial");
    }
};

// ============================================================================
// 6. SyncOfferHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for SyncOfferHeader — Mentor offers topology sync
 */
struct SyncOfferHeaderTest : public TestCase
{
    SyncOfferHeaderTest()
        : TestCase("TAVRN SYNC_OFFER")
    {
    }

    void DoRun() override
    {
        SyncOfferHeader h(Ipv4Address("10.0.0.1"), 500, Ipv4Address("10.0.0.99"));
        NS_TEST_EXPECT_MSG_EQ(h.GetMentorAddr(), Ipv4Address("10.0.0.1"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetGttSize(), 500, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetMenteeAddr(), Ipv4Address("10.0.0.99"), "trivial");

        // Verify serialized size: 4B mentorAddr + 4B gttSize + 4B menteeAddr = 12
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 12, "SYNC_OFFER is 12 bytes");

        // Test setters
        h.SetMentorAddr(Ipv4Address("10.0.0.2"));
        NS_TEST_EXPECT_MSG_EQ(h.GetMentorAddr(), Ipv4Address("10.0.0.2"), "trivial");
        h.SetGttSize(1000);
        NS_TEST_EXPECT_MSG_EQ(h.GetGttSize(), 1000, "trivial");
        h.SetMenteeAddr(Ipv4Address("10.0.0.50"));
        NS_TEST_EXPECT_MSG_EQ(h.GetMenteeAddr(), Ipv4Address("10.0.0.50"), "trivial");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        SyncOfferHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, 12, "SYNC_OFFER deserialized to 12 bytes");
        NS_TEST_EXPECT_MSG_EQ(h2.GetMentorAddr(), Ipv4Address("10.0.0.2"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h2.GetGttSize(), 1000, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h2.GetMenteeAddr(), Ipv4Address("10.0.0.50"), "trivial");
    }
};

// ============================================================================
// 7. SyncPullHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for SyncPullHeader — Mentee requests topology page
 */
struct SyncPullHeaderTest : public TestCase
{
    SyncPullHeaderTest()
        : TestCase("TAVRN SYNC_PULL")
    {
    }

    void DoRun() override
    {
        SyncPullHeader h(15, 20);
        NS_TEST_EXPECT_MSG_EQ(h.GetIndex(), 15, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetCount(), 20, "trivial");

        // Verify serialized size: 4B index + 4B count = 8
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 8, "SYNC_PULL is 8 bytes");

        // Test setters
        h.SetIndex(30);
        NS_TEST_EXPECT_MSG_EQ(h.GetIndex(), 30, "trivial");
        h.SetCount(10);
        NS_TEST_EXPECT_MSG_EQ(h.GetCount(), 10, "trivial");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        SyncPullHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, 8, "SYNC_PULL deserialized to 8 bytes");
        NS_TEST_EXPECT_MSG_EQ(h2.GetIndex(), 30, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h2.GetCount(), 10, "trivial");
    }
};

// ============================================================================
// 8. SyncDataHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for SyncDataHeader — Mentor sends topology page
 */
struct SyncDataHeaderTest : public TestCase
{
    SyncDataHeaderTest()
        : TestCase("TAVRN SYNC_DATA")
    {
    }

    void DoRun() override
    {
        SyncDataHeader h;
        h.SetStartIndex(5);
        h.SetTotalEntries(100);
        NS_TEST_EXPECT_MSG_EQ(h.GetStartIndex(), 5, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetTotalEntries(), 100, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetEntryCount(), 0, "trivial");

        // Empty header: 4B startIndex + 4B totalEntries + 1B count = 9
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 9, "Empty SYNC_DATA is 9 bytes");

        // Add entries (now includes seqNo and hopCount)
        SyncDataEntry e1;
        e1.nodeAddr = Ipv4Address("10.0.0.1");
        e1.lastSeen = 50;
        e1.ttlRemaining = 100;
        e1.seqNo = 10;
        e1.hopCount = 2;
        h.AddEntry(e1);

        SyncDataEntry e2;
        e2.nodeAddr = Ipv4Address("10.0.0.2");
        e2.lastSeen = 60;
        e2.ttlRemaining = 90;
        e2.seqNo = 20;
        e2.hopCount = 3;
        h.AddEntry(e2);

        SyncDataEntry e3;
        e3.nodeAddr = Ipv4Address("10.0.0.3");
        e3.lastSeen = 70;
        e3.ttlRemaining = 80;
        e3.seqNo = 30;
        e3.hopCount = 1;
        h.AddEntry(e3);

        NS_TEST_EXPECT_MSG_EQ(h.GetEntryCount(), 3, "trivial");
        // 9 + 3 * 15 = 54 bytes (5 extra bytes per entry for seqNo + hopCount)
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 54, "SYNC_DATA with 3 entries is 54 bytes");

        // Verify entries
        const auto& entries = h.GetEntries();
        NS_TEST_EXPECT_MSG_EQ(entries.size(), 3, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries[0].nodeAddr, Ipv4Address("10.0.0.1"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries[1].lastSeen, 60, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries[2].ttlRemaining, 80, "trivial");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        SyncDataHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, h.GetSerializedSize(), "(De)Serialized size match");
        NS_TEST_EXPECT_MSG_EQ(h2.GetStartIndex(), 5, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h2.GetTotalEntries(), 100, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h2.GetEntryCount(), 3, "trivial");

        const auto& entries2 = h2.GetEntries();
        NS_TEST_EXPECT_MSG_EQ(entries2[0].nodeAddr, Ipv4Address("10.0.0.1"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[1].nodeAddr, Ipv4Address("10.0.0.2"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[2].nodeAddr, Ipv4Address("10.0.0.3"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[0].lastSeen, 50, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[1].lastSeen, 60, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[2].lastSeen, 70, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[0].ttlRemaining, 100, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[1].ttlRemaining, 90, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entries2[2].ttlRemaining, 80, "trivial");
        // Verify seqNo and hopCount round-trip
        NS_TEST_EXPECT_MSG_EQ(entries2[0].seqNo, 10, "seqNo round-trip");
        NS_TEST_EXPECT_MSG_EQ(entries2[1].seqNo, 20, "seqNo round-trip");
        NS_TEST_EXPECT_MSG_EQ(entries2[2].seqNo, 30, "seqNo round-trip");
        NS_TEST_EXPECT_MSG_EQ(entries2[0].hopCount, 2, "hopCount round-trip");
        NS_TEST_EXPECT_MSG_EQ(entries2[1].hopCount, 3, "hopCount round-trip");
        NS_TEST_EXPECT_MSG_EQ(entries2[2].hopCount, 1, "hopCount round-trip");
    }
};

// ============================================================================
// 9. TcUpdateHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TcUpdateHeader — Topology change broadcast
 */
struct TcUpdateHeaderTest : public TestCase
{
    TcUpdateHeaderTest()
        : TestCase("TAVRN TC_UPDATE")
    {
    }

    void DoRun() override
    {
        TcUpdateHeader h(Ipv4Address("10.0.0.1"),
                         42,
                         Ipv4Address("10.0.0.5"),
                         TcUpdateHeader::NODE_JOIN,
                         12345);
        NS_TEST_EXPECT_MSG_EQ(h.GetOriginAddr(), Ipv4Address("10.0.0.1"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetSeqNo(), 42, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetSubjectAddr(), Ipv4Address("10.0.0.5"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetEventType(), TcUpdateHeader::NODE_JOIN, "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetTimestamp(), 12345, "trivial");

        // Test UUID: (originAddr << 32) | seqNo
        uint64_t expectedUuid =
            (static_cast<uint64_t>(Ipv4Address("10.0.0.1").Get()) << 32) | 42;
        NS_TEST_EXPECT_MSG_EQ(h.GetUuid(), expectedUuid, "UUID computed correctly");

        // Test NODE_LEAVE event type
        h.SetEventType(TcUpdateHeader::NODE_LEAVE);
        NS_TEST_EXPECT_MSG_EQ(h.GetEventType(), TcUpdateHeader::NODE_LEAVE, "trivial");

        // Test setters
        h.SetOriginAddr(Ipv4Address("10.0.0.2"));
        NS_TEST_EXPECT_MSG_EQ(h.GetOriginAddr(), Ipv4Address("10.0.0.2"), "trivial");
        h.SetSeqNo(99);
        NS_TEST_EXPECT_MSG_EQ(h.GetSeqNo(), 99, "trivial");
        h.SetSubjectAddr(Ipv4Address("10.0.0.10"));
        NS_TEST_EXPECT_MSG_EQ(h.GetSubjectAddr(), Ipv4Address("10.0.0.10"), "trivial");
        h.SetTimestamp(99999);
        NS_TEST_EXPECT_MSG_EQ(h.GetTimestamp(), 99999, "trivial");

        // Verify serialized size: 4+4+4+1+4 = 17 bytes
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 17, "TC_UPDATE is 17 bytes");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        TcUpdateHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, 17, "TC_UPDATE deserialized to 17 bytes");
        NS_TEST_EXPECT_MSG_EQ(h, h2, "Round trip serialization works");
    }
};

// ============================================================================
// 9b. ERrepAckHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for ERrepAckHeader (RREP_ACK) round-trip serialization
 */
struct ERrepAckHeaderTest : public TestCase
{
    ERrepAckHeaderTest()
        : TestCase("TAVRN E_RREP_ACK")
    {
    }

    void DoRun() override
    {
        ERrepAckHeader h;
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 1, "E_RREP_ACK is 1 byte");

        // Round-trip serialization
        Ptr<Packet> p = Create<Packet>();
        p->AddHeader(h);
        ERrepAckHeader h2;
        uint32_t bytes = p->RemoveHeader(h2);
        NS_TEST_EXPECT_MSG_EQ(bytes, 1, "Deserialized 1 byte");
        NS_TEST_EXPECT_MSG_EQ(h, h2, "Round-trip serialization works");
    }
};

// ============================================================================
// 10. TopologyMetadataHeaderTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TopologyMetadataHeader — piggybacked metadata
 */
struct TopologyMetadataHeaderTest : public TestCase
{
    TopologyMetadataHeaderTest()
        : TestCase("TAVRN TopologyMetadataHeader")
    {
    }

    void DoRun() override
    {
        // Test empty header
        TopologyMetadataHeader h;
        NS_TEST_EXPECT_MSG_EQ(h.GetEntryCount(), 0, "Empty header has 0 entries");
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 1, "Empty header is 1 byte (count only)");

        // Round-trip empty header
        {
            Ptr<Packet> p = Create<Packet>();
            p->AddHeader(h);
            TopologyMetadataHeader h2;
            uint32_t bytes = p->RemoveHeader(h2);
            NS_TEST_EXPECT_MSG_EQ(bytes, 1, "Empty metadata deserialized to 1 byte");
            NS_TEST_EXPECT_MSG_EQ(h2.GetEntryCount(), 0, "Empty after round trip");
        }

        // Test with 1 entry
        GttMetadataEntry e1;
        e1.nodeAddr = Ipv4Address("10.0.0.1");
        e1.ttlRemaining = 60;
        e1.flags = 0;
        h.AddEntry(e1);
        NS_TEST_EXPECT_MSG_EQ(h.GetEntryCount(), 1, "1 entry");
        // 1B count + 1 * 7B = 8
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 8, "1 entry = 8 bytes");

        // Test FLAG_FRESHNESS_REQUEST flag
        GttMetadataEntry e2;
        e2.nodeAddr = Ipv4Address("10.0.0.2");
        e2.ttlRemaining = 30;
        e2.flags = GttMetadataEntry::FLAG_FRESHNESS_REQUEST;
        h.AddEntry(e2);
        NS_TEST_EXPECT_MSG_EQ(h.GetEntryCount(), 2, "2 entries");

        // Add more entries to get to 5
        for (int i = 3; i <= 5; ++i)
        {
            GttMetadataEntry e;
            std::ostringstream oss;
            oss << "10.0.0." << i;
            e.nodeAddr = Ipv4Address(oss.str().c_str());
            e.ttlRemaining = static_cast<uint16_t>(100 - i * 10);
            e.flags = 0;
            h.AddEntry(e);
        }
        NS_TEST_EXPECT_MSG_EQ(h.GetEntryCount(), 5, "5 entries");
        // 1B count + 5 * 7B = 36
        NS_TEST_EXPECT_MSG_EQ(h.GetSerializedSize(), 36, "5 entries = 36 bytes");

        // Round-trip with 5 entries
        {
            Ptr<Packet> p = Create<Packet>();
            p->AddHeader(h);
            TopologyMetadataHeader h2;
            uint32_t bytes = p->RemoveHeader(h2);
            NS_TEST_EXPECT_MSG_EQ(bytes, 36, "5-entry metadata deserialized to 36 bytes");
            NS_TEST_EXPECT_MSG_EQ(h2.GetEntryCount(), 5, "5 entries after round trip");

            const auto& entries = h2.GetEntries();
            NS_TEST_EXPECT_MSG_EQ(entries[0].nodeAddr, Ipv4Address("10.0.0.1"), "trivial");
            NS_TEST_EXPECT_MSG_EQ(entries[0].ttlRemaining, 60, "trivial");
            NS_TEST_EXPECT_MSG_EQ(entries[0].flags, 0, "trivial");
            NS_TEST_EXPECT_MSG_EQ(entries[1].nodeAddr, Ipv4Address("10.0.0.2"), "trivial");
            NS_TEST_EXPECT_MSG_EQ(entries[1].ttlRemaining, 30, "trivial");
            NS_TEST_EXPECT_MSG_EQ(entries[1].flags,
                                  GttMetadataEntry::FLAG_FRESHNESS_REQUEST,
                                  "Freshness flag preserved");
        }

        // Test Clear
        h.Clear();
        NS_TEST_EXPECT_MSG_EQ(h.GetEntryCount(), 0, "Cleared");
    }
};

// ============================================================================
// 11. GttBasicTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for GTT basic operations
 */
struct GttBasicTest : public TestCase
{
    GttBasicTest()
        : TestCase("TAVRN GTT basic")
    {
    }

    void DoRun() override
    {
        GlobalTopologyTable gtt(Seconds(120), 0.5);

        // Empty GTT
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeCount(), 0, "Empty GTT should have 0 nodes");
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeExists(Ipv4Address("10.0.0.1")), false, "No nodes yet");

        // AddOrUpdateEntry
        NS_TEST_EXPECT_MSG_EQ(gtt.AddOrUpdateEntry(Ipv4Address("10.0.0.1"), 1, 1),
                              true,
                              "Add succeeds");
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeCount(), 1, "1 node after add");
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeExists(Ipv4Address("10.0.0.1")), true, "Node exists");
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeExists(Ipv4Address("10.0.0.2")), false, "Other node doesn't exist");

        // LookupEntry
        GttEntry entry;
        NS_TEST_EXPECT_MSG_EQ(gtt.LookupEntry(Ipv4Address("10.0.0.1"), entry),
                              true,
                              "Lookup succeeds");
        NS_TEST_EXPECT_MSG_EQ(entry.nodeAddr, Ipv4Address("10.0.0.1"), "Address matches");
        NS_TEST_EXPECT_MSG_EQ(entry.seqNo, 1, "SeqNo matches");
        NS_TEST_EXPECT_MSG_EQ(entry.hopCount, 1, "HopCount matches");
        NS_TEST_EXPECT_MSG_EQ(entry.departed, false, "Not departed");

        // Lookup non-existent
        GttEntry entry2;
        NS_TEST_EXPECT_MSG_EQ(gtt.LookupEntry(Ipv4Address("10.0.0.99"), entry2),
                              false,
                              "Lookup fails for non-existent");

        // Add more entries
        gtt.AddOrUpdateEntry(Ipv4Address("10.0.0.2"), 5, 2);
        gtt.AddOrUpdateEntry(Ipv4Address("10.0.0.3"), 3, 3);
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeCount(), 3, "3 nodes");

        // EnumerateNodes
        auto nodes = gtt.EnumerateNodes();
        NS_TEST_EXPECT_MSG_EQ(nodes.size(), 3, "3 nodes enumerated");

        // Update existing entry with higher seqNo
        NS_TEST_EXPECT_MSG_EQ(gtt.AddOrUpdateEntry(Ipv4Address("10.0.0.1"), 10, 2),
                              true,
                              "Update succeeds with higher seqNo");
        GttEntry updated;
        gtt.LookupEntry(Ipv4Address("10.0.0.1"), updated);
        NS_TEST_EXPECT_MSG_EQ(updated.seqNo, 10, "SeqNo updated");
        NS_TEST_EXPECT_MSG_EQ(updated.hopCount, 2, "HopCount updated");

        // Reject update with lower seqNo
        NS_TEST_EXPECT_MSG_EQ(gtt.AddOrUpdateEntry(Ipv4Address("10.0.0.1"), 5, 1),
                              false,
                              "Reject lower seqNo");
        GttEntry unchanged;
        gtt.LookupEntry(Ipv4Address("10.0.0.1"), unchanged);
        NS_TEST_EXPECT_MSG_EQ(unchanged.seqNo, 10, "SeqNo unchanged after reject");

        // RemoveEntry
        NS_TEST_EXPECT_MSG_EQ(gtt.RemoveEntry(Ipv4Address("10.0.0.2")), true, "Remove succeeds");
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeCount(), 2, "2 nodes after remove");
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeExists(Ipv4Address("10.0.0.2")), false, "Removed node gone");
        NS_TEST_EXPECT_MSG_EQ(gtt.RemoveEntry(Ipv4Address("10.0.0.99")),
                              false,
                              "Remove non-existent fails");

        // MergeEntry (newer replaces older)
        GttEntry mergeEntry;
        mergeEntry.nodeAddr = Ipv4Address("10.0.0.1");
        mergeEntry.lastSeen = Seconds(50);
        mergeEntry.ttlExpiry = Seconds(170);
        mergeEntry.softExpiry = Seconds(110);
        mergeEntry.seqNo = 20;
        mergeEntry.hopCount = 5;
        mergeEntry.departed = false;
        gtt.MergeEntry(mergeEntry);
        GttEntry merged;
        gtt.LookupEntry(Ipv4Address("10.0.0.1"), merged);
        NS_TEST_EXPECT_MSG_EQ(merged.seqNo, 20, "MergeEntry updated seqNo");

        // MergeEntry with lower seqNo should be rejected
        GttEntry staleEntry;
        staleEntry.nodeAddr = Ipv4Address("10.0.0.1");
        staleEntry.lastSeen = Seconds(1);
        staleEntry.ttlExpiry = Seconds(121);
        staleEntry.softExpiry = Seconds(61);
        staleEntry.seqNo = 5;
        staleEntry.hopCount = 1;
        staleEntry.departed = false;
        gtt.MergeEntry(staleEntry);
        GttEntry stillMerged;
        gtt.LookupEntry(Ipv4Address("10.0.0.1"), stillMerged);
        NS_TEST_EXPECT_MSG_EQ(stillMerged.seqNo, 20, "Stale merge rejected");

        // MergeEntry for new node
        GttEntry newEntry;
        newEntry.nodeAddr = Ipv4Address("10.0.0.50");
        newEntry.lastSeen = Seconds(10);
        newEntry.ttlExpiry = Seconds(130);
        newEntry.softExpiry = Seconds(70);
        newEntry.seqNo = 1;
        newEntry.hopCount = 3;
        newEntry.departed = false;
        gtt.MergeEntry(newEntry);
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeExists(Ipv4Address("10.0.0.50")),
                              true,
                              "Merge adds new node");

        // RefreshEntry (preserves seqNo through the update argument, resets TTL)
        gtt.RefreshEntry(Ipv4Address("10.0.0.3"), 3);
        GttEntry refreshed;
        gtt.LookupEntry(Ipv4Address("10.0.0.3"), refreshed);
        NS_TEST_EXPECT_MSG_EQ(refreshed.seqNo, 3, "SeqNo preserved in refresh");

        // MarkDeparted
        gtt.MarkDeparted(Ipv4Address("10.0.0.50"));
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeExists(Ipv4Address("10.0.0.50")),
                              false,
                              "Departed node not visible via NodeExists");
        GttEntry departedEntry;
        NS_TEST_EXPECT_MSG_EQ(gtt.LookupEntry(Ipv4Address("10.0.0.50"), departedEntry),
                              true,
                              "Departed node still in table via LookupEntry");
        NS_TEST_EXPECT_MSG_EQ(departedEntry.departed, true, "Departed flag set");

        // GetTotalEntries includes departed
        // After: add 3, remove 1, merge 1 new = 3 total entries
        NS_TEST_EXPECT_MSG_EQ(gtt.GetTotalEntries(), 3, "Total includes departed");
        // NodeCount excludes departed (10.0.0.1, 10.0.0.3 active; 10.0.0.50 departed)
        NS_TEST_EXPECT_MSG_EQ(gtt.NodeCount(), 2, "NodeCount excludes departed");

        Simulator::Destroy();
    }
};

// ============================================================================
// 12. GttExpiryTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for GTT soft/hard expiry and purge
 */
struct GttExpiryTest : public TestCase
{
    GttExpiryTest()
        : TestCase("TAVRN GTT expiry"),
          m_gtt(nullptr)
    {
    }

    void DoRun() override;
    void CheckSoftExpiry();
    void CheckHardExpiry();
    void CheckPurge();

    GlobalTopologyTable* m_gtt;
};

void
GttExpiryTest::CheckSoftExpiry()
{
    // At t=7s with TTL=10s and soft threshold 0.5:
    // softExpiry = t0 + 10*0.5 = 5s, ttlExpiry = t0 + 10 = 10s
    // Node added at t=0, so softExpiry = 5s, ttlExpiry = 10s
    // At t=7s: now(7) >= soft(5) AND now(7) < ttl(10) -> soft expired
    auto softExpired = m_gtt->GetSoftExpiredEntries();
    NS_TEST_EXPECT_MSG_EQ(softExpired.size(), 1, "One entry at soft expiry at t=7s");
    if (!softExpired.empty())
    {
        NS_TEST_EXPECT_MSG_EQ(softExpired[0].nodeAddr,
                              Ipv4Address("10.0.0.1"),
                              "Correct node soft-expired");
    }

    // Hard-expired should be empty (not past ttlExpiry yet)
    auto hardExpired = m_gtt->GetHardExpiredEntries();
    NS_TEST_EXPECT_MSG_EQ(hardExpired.size(), 0, "No hard-expired entries at t=7s");
}

void
GttExpiryTest::CheckHardExpiry()
{
    // At t=12s: now(12) >= ttl(10) -> hard expired
    auto hardExpired = m_gtt->GetHardExpiredEntries();
    NS_TEST_EXPECT_MSG_EQ(hardExpired.size(), 1, "One entry at hard expiry at t=12s");
    if (!hardExpired.empty())
    {
        NS_TEST_EXPECT_MSG_EQ(hardExpired[0].nodeAddr,
                              Ipv4Address("10.0.0.1"),
                              "Correct node hard-expired");
    }

    // Soft-expired should NOT include this (past ttlExpiry now)
    auto softExpired = m_gtt->GetSoftExpiredEntries();
    NS_TEST_EXPECT_MSG_EQ(softExpired.size(), 0, "Hard-expired node not in soft-expired list");
}

void
GttExpiryTest::CheckPurge()
{
    // At t=35s: node was marked departed at some point;
    // Purge removes entries departed for > 2*TTL = 20s
    m_gtt->MarkDeparted(Ipv4Address("10.0.0.1"));
    // Right after marking, it should still exist
    NS_TEST_EXPECT_MSG_EQ(m_gtt->GetTotalEntries(), 1, "Departed entry still in table");
}

void
GttExpiryTest::DoRun()
{
    GlobalTopologyTable gtt(Seconds(10), 0.5);
    m_gtt = &gtt;

    // Add entry at t=0
    gtt.AddOrUpdateEntry(Ipv4Address("10.0.0.1"), 1, 1);
    NS_TEST_EXPECT_MSG_EQ(gtt.NodeCount(), 1, "One entry added");

    // Initially no expired entries
    auto softExpired = gtt.GetSoftExpiredEntries();
    NS_TEST_EXPECT_MSG_EQ(softExpired.size(), 0, "No soft-expired at t=0");
    auto hardExpired = gtt.GetHardExpiredEntries();
    NS_TEST_EXPECT_MSG_EQ(hardExpired.size(), 0, "No hard-expired at t=0");

    // Check at t=7s (after soft expiry at 5s, before hard expiry at 10s)
    Simulator::Schedule(Seconds(7), &GttExpiryTest::CheckSoftExpiry, this);

    // Check at t=12s (after hard expiry at 10s)
    Simulator::Schedule(Seconds(12), &GttExpiryTest::CheckHardExpiry, this);

    // Mark departed and test purge at t=35s
    Simulator::Schedule(Seconds(35), &GttExpiryTest::CheckPurge, this);

    Simulator::Run();
    Simulator::Destroy();
}

// ============================================================================
// 13. GttTtlRemainingTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for GTT TtlRemaining and RefreshEntry TTL reset
 */
struct GttTtlRemainingTest : public TestCase
{
    GttTtlRemainingTest()
        : TestCase("TAVRN GTT TtlRemaining"),
          m_gtt(nullptr)
    {
    }

    void DoRun() override;
    void CheckTtl();
    void CheckRefreshed();

    GlobalTopologyTable* m_gtt;
};

void
GttTtlRemainingTest::CheckTtl()
{
    // At t=5s, entry was added at t=0 with TTL=10s
    // Remaining should be ~5s
    Time remaining = m_gtt->TtlRemaining(Ipv4Address("10.0.0.1"));
    NS_TEST_EXPECT_MSG_EQ(remaining, Seconds(5), "TTL remaining at t=5s is 5s");

    // Unknown node returns 0
    Time unknownRemaining = m_gtt->TtlRemaining(Ipv4Address("10.0.0.99"));
    NS_TEST_EXPECT_MSG_EQ(unknownRemaining, Seconds(0), "Unknown node returns 0");

    // Refresh the entry at t=5s
    m_gtt->RefreshEntry(Ipv4Address("10.0.0.1"), 2);
}

void
GttTtlRemainingTest::CheckRefreshed()
{
    // At t=8s, entry was refreshed at t=5s with TTL=10s
    // Remaining should be ~7s (5+10-8 = 7)
    Time remaining = m_gtt->TtlRemaining(Ipv4Address("10.0.0.1"));
    NS_TEST_EXPECT_MSG_EQ(remaining, Seconds(7), "TTL remaining at t=8s after refresh is 7s");
}

void
GttTtlRemainingTest::DoRun()
{
    GlobalTopologyTable gtt(Seconds(10), 0.5);
    m_gtt = &gtt;

    // Add at t=0 with TTL=10s
    gtt.AddOrUpdateEntry(Ipv4Address("10.0.0.1"), 1, 1);

    // At t=0, TTL remaining should be 10s
    Time remaining = gtt.TtlRemaining(Ipv4Address("10.0.0.1"));
    NS_TEST_EXPECT_MSG_EQ(remaining, Seconds(10), "TTL remaining at t=0 is 10s");

    Simulator::Schedule(Seconds(5), &GttTtlRemainingTest::CheckTtl, this);
    Simulator::Schedule(Seconds(8), &GttTtlRemainingTest::CheckRefreshed, this);

    Simulator::Run();
    Simulator::Destroy();
}

// ============================================================================
// 14. RoutingTableEntryTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TAVRN routing table entry
 */
struct TavrnRtableEntryTest : public TestCase
{
    TavrnRtableEntryTest()
        : TestCase("TAVRN RtableEntry")
    {
    }

    void DoRun() override
    {
        Ptr<NetDevice> dev;
        Ipv4InterfaceAddress iface;
        RoutingTableEntry rt(/*output device*/ dev,
                             /*dst*/ Ipv4Address("1.2.3.4"),
                             /*validSeqNo*/ true,
                             /*seqNo*/ 10,
                             /*interface*/ iface,
                             /*hop*/ 5,
                             /*next hop*/ Ipv4Address("3.3.3.3"),
                             /*lifetime*/ Seconds(10));
        NS_TEST_EXPECT_MSG_EQ(rt.GetOutputDevice(), dev, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetDestination(), Ipv4Address("1.2.3.4"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetValidSeqNo(), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetSeqNo(), 10, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetInterface(), iface, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetHop(), 5, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetNextHop(), Ipv4Address("3.3.3.3"), "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetLifeTime(), Seconds(10), "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetFlag(), VALID, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetRreqCnt(), 0, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.IsPrecursorListEmpty(), true, "trivial");

        // Test setters
        Ptr<NetDevice> dev2;
        Ipv4InterfaceAddress iface2;
        rt.SetOutputDevice(dev2);
        NS_TEST_EXPECT_MSG_EQ(rt.GetOutputDevice(), dev2, "trivial");
        rt.SetInterface(iface2);
        NS_TEST_EXPECT_MSG_EQ(rt.GetInterface(), iface2, "trivial");
        rt.SetValidSeqNo(false);
        NS_TEST_EXPECT_MSG_EQ(rt.GetValidSeqNo(), false, "trivial");
        rt.SetFlag(INVALID);
        NS_TEST_EXPECT_MSG_EQ(rt.GetFlag(), INVALID, "trivial");
        rt.SetFlag(IN_SEARCH);
        NS_TEST_EXPECT_MSG_EQ(rt.GetFlag(), IN_SEARCH, "trivial");
        rt.SetHop(12);
        NS_TEST_EXPECT_MSG_EQ(rt.GetHop(), 12, "trivial");
        rt.SetLifeTime(Seconds(1));
        NS_TEST_EXPECT_MSG_EQ(rt.GetLifeTime(), Seconds(1), "trivial");
        rt.SetNextHop(Ipv4Address("1.1.1.1"));
        NS_TEST_EXPECT_MSG_EQ(rt.GetNextHop(), Ipv4Address("1.1.1.1"), "trivial");
        rt.SetUnidirectional(true);
        NS_TEST_EXPECT_MSG_EQ(rt.IsUnidirectional(), true, "trivial");
        rt.SetBlacklistTimeout(Seconds(7));
        NS_TEST_EXPECT_MSG_EQ(rt.GetBlacklistTimeout(), Seconds(7), "trivial");
        rt.SetRreqCnt(2);
        NS_TEST_EXPECT_MSG_EQ(rt.GetRreqCnt(), 2, "trivial");
        rt.IncrementRreqCnt();
        NS_TEST_EXPECT_MSG_EQ(rt.GetRreqCnt(), 3, "trivial");
        rt.Invalidate(Seconds(13));
        NS_TEST_EXPECT_MSG_EQ(rt.GetFlag(), INVALID, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetLifeTime(), Seconds(13), "trivial");
        rt.SetLifeTime(MilliSeconds(100));
        NS_TEST_EXPECT_MSG_EQ(rt.GetLifeTime(), MilliSeconds(100), "trivial");
        Ptr<Ipv4Route> route = rt.GetRoute();
        NS_TEST_EXPECT_MSG_EQ(route->GetDestination(), Ipv4Address("1.2.3.4"), "trivial");

        // Precursor list operations
        NS_TEST_EXPECT_MSG_EQ(rt.InsertPrecursor(Ipv4Address("10.0.0.1")), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.IsPrecursorListEmpty(), false, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.InsertPrecursor(Ipv4Address("10.0.0.2")), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.InsertPrecursor(Ipv4Address("10.0.0.2")), false, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.LookupPrecursor(Ipv4Address("10.0.0.3")), false, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.LookupPrecursor(Ipv4Address("10.0.0.1")), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.DeletePrecursor(Ipv4Address("10.0.0.2")), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.LookupPrecursor(Ipv4Address("10.0.0.2")), false, "trivial");
        std::vector<Ipv4Address> prec;
        rt.GetPrecursors(prec);
        NS_TEST_EXPECT_MSG_EQ(prec.size(), 1, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.InsertPrecursor(Ipv4Address("10.0.0.4")), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.DeletePrecursor(Ipv4Address("10.0.0.5")), false, "trivial");
        rt.GetPrecursors(prec);
        NS_TEST_EXPECT_MSG_EQ(prec.size(), 2, "trivial");
        rt.DeleteAllPrecursors();
        NS_TEST_EXPECT_MSG_EQ(rt.IsPrecursorListEmpty(), true, "trivial");
        rt.GetPrecursors(prec);
        NS_TEST_EXPECT_MSG_EQ(prec.size(), 2, "trivial");

        Simulator::Destroy();
    }
};

// ============================================================================
// 15. RoutingTableTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TAVRN routing table
 */
struct TavrnRtableTest : public TestCase
{
    TavrnRtableTest()
        : TestCase("TAVRN Rtable")
    {
    }

    void DoRun() override
    {
        RoutingTable rtable(Seconds(2));
        NS_TEST_EXPECT_MSG_EQ(rtable.GetBadLinkLifetime(), Seconds(2), "trivial");
        rtable.SetBadLinkLifetime(Seconds(1));
        NS_TEST_EXPECT_MSG_EQ(rtable.GetBadLinkLifetime(), Seconds(1), "trivial");

        Ptr<NetDevice> dev;
        Ipv4InterfaceAddress iface;
        RoutingTableEntry rt(/*output device*/ dev,
                             /*dst*/ Ipv4Address("1.2.3.4"),
                             /*validSeqNo*/ true,
                             /*seqNo*/ 10,
                             /*interface*/ iface,
                             /*hop*/ 5,
                             /*next hop*/ Ipv4Address("1.1.1.1"),
                             /*lifetime*/ Seconds(10));
        NS_TEST_EXPECT_MSG_EQ(rtable.AddRoute(rt), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.AddRoute(rt), false, "trivial");

        RoutingTableEntry rt2(/*output device*/ dev,
                              /*dst*/ Ipv4Address("4.3.2.1"),
                              /*validSeqNo*/ false,
                              /*seqNo*/ 0,
                              /*interface*/ iface,
                              /*hop*/ 15,
                              /*next hop*/ Ipv4Address("1.1.1.1"),
                              /*lifetime*/ Seconds(1));
        NS_TEST_EXPECT_MSG_EQ(rtable.AddRoute(rt2), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.LookupRoute(rt2.GetDestination(), rt), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt2.GetDestination(), rt.GetDestination(), "trivial");

        rt.SetHop(20);
        rt.InsertPrecursor(Ipv4Address("10.0.0.3"));
        NS_TEST_EXPECT_MSG_EQ(rtable.Update(rt), true, "trivial");

        RoutingTableEntry rt3;
        NS_TEST_EXPECT_MSG_EQ(rtable.LookupRoute(Ipv4Address("10.0.0.1"), rt), false, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.Update(rt3), false, "trivial");

        NS_TEST_EXPECT_MSG_EQ(rtable.SetEntryState(Ipv4Address("10.0.0.1"), INVALID),
                              false,
                              "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.SetEntryState(Ipv4Address("1.2.3.4"), IN_SEARCH),
                              true,
                              "trivial");

        NS_TEST_EXPECT_MSG_EQ(rtable.DeleteRoute(Ipv4Address("5.5.5.5")), false, "trivial");

        RoutingTableEntry rt4(/*output device*/ dev,
                              /*dst*/ Ipv4Address("5.5.5.5"),
                              /*validSeqNo*/ false,
                              /*seqNo*/ 0,
                              /*interface*/ iface,
                              /*hop*/ 15,
                              /*next hop*/ Ipv4Address("1.1.1.1"),
                              /*lifetime*/ Seconds(-10));
        NS_TEST_EXPECT_MSG_EQ(rtable.AddRoute(rt4), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.SetEntryState(Ipv4Address("5.5.5.5"), INVALID),
                              true,
                              "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.LookupRoute(Ipv4Address("5.5.5.5"), rt), false, "trivial");

        NS_TEST_EXPECT_MSG_EQ(
            rtable.MarkLinkAsUnidirectional(Ipv4Address("1.2.3.4"), Seconds(2)),
            true,
            "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.LookupRoute(Ipv4Address("1.2.3.4"), rt), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.IsUnidirectional(), true, "trivial");

        rt.SetLifeTime(Seconds(-5));
        NS_TEST_EXPECT_MSG_EQ(rtable.Update(rt), true, "trivial");

        std::map<Ipv4Address, uint32_t> unreachable;
        rtable.GetListOfDestinationWithNextHop(Ipv4Address("1.1.1.1"), unreachable);
        NS_TEST_EXPECT_MSG_EQ(unreachable.size(), 2, "trivial");

        unreachable.insert(std::make_pair(Ipv4Address("4.3.2.1"), 3));
        rtable.InvalidateRoutesWithDst(unreachable);
        NS_TEST_EXPECT_MSG_EQ(rtable.LookupRoute(Ipv4Address("4.3.2.1"), rt), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rt.GetFlag(), INVALID, "trivial");

        NS_TEST_EXPECT_MSG_EQ(rtable.DeleteRoute(Ipv4Address("1.2.3.4")), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(rtable.DeleteRoute(Ipv4Address("1.2.3.4")), false, "trivial");

        Simulator::Destroy();
    }
};

// ============================================================================
// 16. QueueEntryTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TAVRN queue entry
 */
struct TavrnQueueEntryTest : public TestCase
{
    TavrnQueueEntryTest()
        : TestCase("TAVRN QueueEntry")
    {
    }

    void Unicast(Ptr<Ipv4Route> route, Ptr<const Packet> packet, const Ipv4Header& header)
    {
    }

    void Error(Ptr<const Packet> p, const Ipv4Header& h, Socket::SocketErrno e)
    {
    }

    void Unicast2(Ptr<Ipv4Route> route, Ptr<const Packet> packet, const Ipv4Header& header)
    {
    }

    void Error2(Ptr<const Packet> p, const Ipv4Header& h, Socket::SocketErrno e)
    {
    }

    void DoRun() override
    {
        Ptr<const Packet> packet = Create<Packet>();
        Ipv4Header h;
        h.SetDestination(Ipv4Address("1.2.3.4"));
        h.SetSource(Ipv4Address("4.3.2.1"));
        Ipv4RoutingProtocol::UnicastForwardCallback ucb =
            MakeCallback(&TavrnQueueEntryTest::Unicast, this);
        Ipv4RoutingProtocol::ErrorCallback ecb =
            MakeCallback(&TavrnQueueEntryTest::Error, this);
        QueueEntry entry(packet, h, ucb, ecb, Seconds(1));
        NS_TEST_EXPECT_MSG_EQ(h.GetDestination(),
                              entry.GetIpv4Header().GetDestination(),
                              "trivial");
        NS_TEST_EXPECT_MSG_EQ(h.GetSource(), entry.GetIpv4Header().GetSource(), "trivial");
        NS_TEST_EXPECT_MSG_EQ(ucb.IsEqual(entry.GetUnicastForwardCallback()), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(ecb.IsEqual(entry.GetErrorCallback()), true, "trivial");
        NS_TEST_EXPECT_MSG_EQ(entry.GetExpireTime(), Seconds(1), "trivial");
        NS_TEST_EXPECT_MSG_EQ(entry.GetPacket(), packet, "trivial");
        entry.SetExpireTime(Seconds(3));
        NS_TEST_EXPECT_MSG_EQ(entry.GetExpireTime(), Seconds(3), "trivial");
        Ipv4Header h2;
        h2.SetDestination(Ipv4Address("1.1.1.1"));
        entry.SetIpv4Header(h2);
        NS_TEST_EXPECT_MSG_EQ(entry.GetIpv4Header().GetDestination(),
                              Ipv4Address("1.1.1.1"),
                              "trivial");
        Ipv4RoutingProtocol::UnicastForwardCallback ucb2 =
            MakeCallback(&TavrnQueueEntryTest::Unicast2, this);
        Ipv4RoutingProtocol::ErrorCallback ecb2 =
            MakeCallback(&TavrnQueueEntryTest::Error2, this);
        entry.SetErrorCallback(ecb2);
        NS_TEST_EXPECT_MSG_EQ(ecb2.IsEqual(entry.GetErrorCallback()), true, "trivial");
        entry.SetUnicastForwardCallback(ucb2);
        NS_TEST_EXPECT_MSG_EQ(ucb2.IsEqual(entry.GetUnicastForwardCallback()), true, "trivial");
    }
};

// ============================================================================
// 17. RequestQueueTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TAVRN request queue
 */
struct TavrnRqueueTest : public TestCase
{
    TavrnRqueueTest()
        : TestCase("TAVRN Rqueue"),
          q(64, Seconds(30))
    {
    }

    void DoRun() override;

    void Unicast(Ptr<Ipv4Route> route, Ptr<const Packet> packet, const Ipv4Header& header)
    {
    }

    void Error(Ptr<const Packet> p, const Ipv4Header& h, Socket::SocketErrno e)
    {
    }

    void CheckSizeLimit();
    void CheckTimeout();

    RequestQueue q;
};

void
TavrnRqueueTest::DoRun()
{
    NS_TEST_EXPECT_MSG_EQ(q.GetMaxQueueLen(), 64, "trivial");
    q.SetMaxQueueLen(32);
    NS_TEST_EXPECT_MSG_EQ(q.GetMaxQueueLen(), 32, "trivial");
    NS_TEST_EXPECT_MSG_EQ(q.GetQueueTimeout(), Seconds(30), "trivial");
    q.SetQueueTimeout(Seconds(10));
    NS_TEST_EXPECT_MSG_EQ(q.GetQueueTimeout(), Seconds(10), "trivial");

    Ptr<const Packet> packet = Create<Packet>();
    Ipv4Header h;
    h.SetDestination(Ipv4Address("1.2.3.4"));
    h.SetSource(Ipv4Address("4.3.2.1"));
    Ipv4RoutingProtocol::UnicastForwardCallback ucb =
        MakeCallback(&TavrnRqueueTest::Unicast, this);
    Ipv4RoutingProtocol::ErrorCallback ecb = MakeCallback(&TavrnRqueueTest::Error, this);
    QueueEntry e1(packet, h, ucb, ecb, Seconds(1));
    q.Enqueue(e1);
    q.Enqueue(e1);
    q.Enqueue(e1);
    NS_TEST_EXPECT_MSG_EQ(q.Find(Ipv4Address("1.2.3.4")), true, "trivial");
    NS_TEST_EXPECT_MSG_EQ(q.Find(Ipv4Address("1.1.1.1")), false, "trivial");
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 1, "trivial");
    q.DropPacketWithDst(Ipv4Address("1.2.3.4"));
    NS_TEST_EXPECT_MSG_EQ(q.Find(Ipv4Address("1.2.3.4")), false, "trivial");
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 0, "trivial");

    h.SetDestination(Ipv4Address("2.2.2.2"));
    QueueEntry e2(packet, h, ucb, ecb, Seconds(1));
    q.Enqueue(e1);
    q.Enqueue(e2);
    Ptr<Packet> packet2 = Create<Packet>();
    QueueEntry e3(packet2, h, ucb, ecb, Seconds(1));
    NS_TEST_EXPECT_MSG_EQ(q.Dequeue(Ipv4Address("3.3.3.3"), e3), false, "trivial");
    NS_TEST_EXPECT_MSG_EQ(q.Dequeue(Ipv4Address("2.2.2.2"), e3), true, "trivial");
    NS_TEST_EXPECT_MSG_EQ(q.Find(Ipv4Address("2.2.2.2")), false, "trivial");
    q.Enqueue(e2);
    q.Enqueue(e3);
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 2, "trivial");
    Ptr<Packet> packet4 = Create<Packet>();
    h.SetDestination(Ipv4Address("1.2.3.4"));
    QueueEntry e4(packet4, h, ucb, ecb, Seconds(20));
    q.Enqueue(e4);
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 3, "trivial");
    q.DropPacketWithDst(Ipv4Address("1.2.3.4"));
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 1, "trivial");

    CheckSizeLimit();

    Simulator::Schedule(q.GetQueueTimeout() + Seconds(1),
                        &TavrnRqueueTest::CheckTimeout,
                        this);

    Simulator::Run();
    Simulator::Destroy();
}

void
TavrnRqueueTest::CheckSizeLimit()
{
    Ptr<Packet> packet = Create<Packet>();
    Ipv4Header header;
    Ipv4RoutingProtocol::UnicastForwardCallback ucb =
        MakeCallback(&TavrnRqueueTest::Unicast, this);
    Ipv4RoutingProtocol::ErrorCallback ecb = MakeCallback(&TavrnRqueueTest::Error, this);
    QueueEntry e1(packet, header, ucb, ecb, Seconds(1));

    for (uint32_t i = 0; i < q.GetMaxQueueLen(); ++i)
    {
        q.Enqueue(e1);
    }
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 2, "trivial");

    for (uint32_t i = 0; i < q.GetMaxQueueLen(); ++i)
    {
        q.Enqueue(e1);
    }
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 2, "trivial");
}

void
TavrnRqueueTest::CheckTimeout()
{
    NS_TEST_EXPECT_MSG_EQ(q.GetSize(), 0, "Must be empty now");
}

// ============================================================================
// 18. NeighborTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TAVRN neighbor management
 */
struct TavrnNeighborTest : public TestCase
{
    TavrnNeighborTest()
        : TestCase("TAVRN Neighbor"),
          neighbor(nullptr)
    {
    }

    void DoRun() override;
    void Handler(Ipv4Address addr);
    void CheckTimeout1();
    void CheckTimeout2();
    void CheckTimeout3();

    Neighbors* neighbor;
};

void
TavrnNeighborTest::Handler(Ipv4Address addr)
{
}

void
TavrnNeighborTest::CheckTimeout1()
{
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.2.3.4")), true, "Neighbor exists");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.1.1.1")), true, "Neighbor exists");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("2.2.2.2")), true, "Neighbor exists");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("3.3.3.3")), true, "Neighbor exists");
}

void
TavrnNeighborTest::CheckTimeout2()
{
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.2.3.4")),
                          false,
                          "Neighbor doesn't exist");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.1.1.1")),
                          false,
                          "Neighbor doesn't exist");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("2.2.2.2")),
                          false,
                          "Neighbor doesn't exist");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("3.3.3.3")), true, "Neighbor exists");
}

void
TavrnNeighborTest::CheckTimeout3()
{
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.2.3.4")),
                          false,
                          "Neighbor doesn't exist");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.1.1.1")),
                          false,
                          "Neighbor doesn't exist");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("2.2.2.2")),
                          false,
                          "Neighbor doesn't exist");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("3.3.3.3")),
                          false,
                          "Neighbor doesn't exist");
}

void
TavrnNeighborTest::DoRun()
{
    Neighbors nb(Seconds(1));
    neighbor = &nb;
    neighbor->SetCallback(MakeCallback(&TavrnNeighborTest::Handler, this));
    neighbor->Update(Ipv4Address("1.2.3.4"), Seconds(1));
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.2.3.4")), true, "Neighbor exists");
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("4.3.2.1")),
                          false,
                          "Neighbor doesn't exist");
    neighbor->Update(Ipv4Address("1.2.3.4"), Seconds(10));
    NS_TEST_EXPECT_MSG_EQ(neighbor->IsNeighbor(Ipv4Address("1.2.3.4")), true, "Neighbor exists");
    NS_TEST_EXPECT_MSG_EQ(neighbor->GetExpireTime(Ipv4Address("1.2.3.4")),
                          Seconds(10),
                          "Known expire time");
    NS_TEST_EXPECT_MSG_EQ(neighbor->GetExpireTime(Ipv4Address("4.3.2.1")),
                          Seconds(0),
                          "Known expire time");
    neighbor->Update(Ipv4Address("1.1.1.1"), Seconds(5));
    neighbor->Update(Ipv4Address("2.2.2.2"), Seconds(10));
    neighbor->Update(Ipv4Address("3.3.3.3"), Seconds(20));

    Simulator::Schedule(Seconds(2), &TavrnNeighborTest::CheckTimeout1, this);
    Simulator::Schedule(Seconds(15), &TavrnNeighborTest::CheckTimeout2, this);
    Simulator::Schedule(Seconds(30), &TavrnNeighborTest::CheckTimeout3, this);
    Simulator::Run();
    Simulator::Destroy();
}

// ============================================================================
// 19. IdCacheTest (RREQ dedup)
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TAVRN ID cache (RREQ duplicate detection)
 */
struct TavrnIdCacheTest : public TestCase
{
    TavrnIdCacheTest()
        : TestCase("TAVRN IdCache"),
          m_cache(nullptr)
    {
    }

    void DoRun() override;
    void CheckExpiry();

    IdCache* m_cache;
};

void
TavrnIdCacheTest::CheckExpiry()
{
    // After lifetime expires, entry should be purged
    NS_TEST_EXPECT_MSG_EQ(m_cache->IsDuplicate(Ipv4Address("10.0.0.1"), 1),
                          false,
                          "Entry expired, should not be duplicate");
}

void
TavrnIdCacheTest::DoRun()
{
    IdCache cache(Seconds(5));
    m_cache = &cache;

    // First time: not duplicate (inserts entry)
    NS_TEST_EXPECT_MSG_EQ(cache.IsDuplicate(Ipv4Address("10.0.0.1"), 1),
                          false,
                          "First time is not duplicate");

    // Second time: is duplicate
    NS_TEST_EXPECT_MSG_EQ(cache.IsDuplicate(Ipv4Address("10.0.0.1"), 1),
                          true,
                          "Second time is duplicate");

    // Different ID: not duplicate
    NS_TEST_EXPECT_MSG_EQ(cache.IsDuplicate(Ipv4Address("10.0.0.1"), 2),
                          false,
                          "Different ID is not duplicate");

    // Different address: not duplicate
    NS_TEST_EXPECT_MSG_EQ(cache.IsDuplicate(Ipv4Address("10.0.0.2"), 1),
                          false,
                          "Different address is not duplicate");

    // Check size
    NS_TEST_EXPECT_MSG_EQ(cache.GetSize(), 3, "Cache has 3 entries");

    // Schedule check after expiry
    Simulator::Schedule(Seconds(6), &TavrnIdCacheTest::CheckExpiry, this);

    Simulator::Run();
    Simulator::Destroy();
}

// ============================================================================
// 20. TcUuidCacheTest
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief Test case for TAVRN TC-UPDATE UUID deduplication cache
 */
struct TavrnTcUuidCacheTest : public TestCase
{
    TavrnTcUuidCacheTest()
        : TestCase("TAVRN TcUuidCache"),
          m_cache(nullptr)
    {
    }

    void DoRun() override;
    void CheckExpiry();

    IdCache* m_cache;
};

void
TavrnTcUuidCacheTest::CheckExpiry()
{
    // After 30s lifetime, UUID should have expired
    NS_TEST_EXPECT_MSG_EQ(m_cache->IsUuidSeen(0xDEADBEEF00000001ULL),
                          false,
                          "UUID expired after 31s");
}

void
TavrnTcUuidCacheTest::DoRun()
{
    IdCache cache(Seconds(30));
    m_cache = &cache;

    // UUID not yet seen
    NS_TEST_EXPECT_MSG_EQ(cache.IsUuidSeen(0xDEADBEEF00000001ULL),
                          false,
                          "UUID not yet seen");

    // Insert UUID
    cache.InsertUuid(0xDEADBEEF00000001ULL);

    // UUID now seen
    NS_TEST_EXPECT_MSG_EQ(cache.IsUuidSeen(0xDEADBEEF00000001ULL),
                          true,
                          "UUID is seen");

    // Different UUID not seen
    NS_TEST_EXPECT_MSG_EQ(cache.IsUuidSeen(0xDEADBEEF00000002ULL),
                          false,
                          "Different UUID not seen");

    // Insert another
    cache.InsertUuid(0xDEADBEEF00000002ULL);
    NS_TEST_EXPECT_MSG_EQ(cache.IsUuidSeen(0xDEADBEEF00000002ULL),
                          true,
                          "Second UUID is seen");

    // Schedule expiry check at t=31s (lifetime is 30s)
    Simulator::Schedule(Seconds(31), &TavrnTcUuidCacheTest::CheckExpiry, this);

    Simulator::Run();
    Simulator::Destroy();
}

// ============================================================================
// Test Suite Registration
// ============================================================================

/**
 * @ingroup tavrn-test
 * @brief The TAVRN test suite
 */
class TavrnTestSuite : public TestSuite
{
  public:
    TavrnTestSuite()
        : TestSuite("tavrn", Type::UNIT)
    {
        // Packet header tests (1-10)
        AddTestCase(new TypeHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new ERreqHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new ERrepHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new ERerrHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new HelloHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new SyncOfferHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new SyncPullHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new SyncDataHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new TcUpdateHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new ERrepAckHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new TopologyMetadataHeaderTest, TestCase::Duration::QUICK);
        // GTT tests (11-13)
        AddTestCase(new GttBasicTest, TestCase::Duration::QUICK);
        AddTestCase(new GttExpiryTest, TestCase::Duration::QUICK);
        AddTestCase(new GttTtlRemainingTest, TestCase::Duration::QUICK);
        // Routing table tests (14-15)
        AddTestCase(new TavrnRtableEntryTest, TestCase::Duration::QUICK);
        AddTestCase(new TavrnRtableTest, TestCase::Duration::QUICK);
        // Queue tests (16-17)
        AddTestCase(new TavrnQueueEntryTest, TestCase::Duration::QUICK);
        AddTestCase(new TavrnRqueueTest, TestCase::Duration::QUICK);
        // Neighbor test (18)
        AddTestCase(new TavrnNeighborTest, TestCase::Duration::QUICK);
        // ID cache tests (19-20)
        AddTestCase(new TavrnIdCacheTest, TestCase::Duration::QUICK);
        AddTestCase(new TavrnTcUuidCacheTest, TestCase::Duration::QUICK);
    }
};

static TavrnTestSuite g_tavrnTestSuite; ///< the test suite

} // namespace tavrn
} // namespace ns3
