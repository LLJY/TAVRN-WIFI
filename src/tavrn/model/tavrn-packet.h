/*
 * TAVRN v2: Topology Aware Vicinity-Reactive Network
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef TAVRN_PACKET_H
#define TAVRN_PACKET_H

#include "ns3/header.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

namespace ns3
{
namespace tavrn
{

// ============================================================================
// Entropy-Based Suffix Compression (ESC) helpers
// ============================================================================

/**
 * @ingroup tavrn
 * @brief Entropy-Based Suffix Compression (ESC) for TAVRN wire encoding.
 *
 * Inspired by IPHC (RFC 6282), ESC compresses L3 addresses on the wire
 * using suffix bytes whose count is determined by the entropy of the GTT.
 * The GTT serves as the shared context table (analogous to IPHC's context,
 * but with a defined synchronization mechanism: mentorship + piggybacking).
 *
 * Each address field carries a 2-bit Address Mode (AM):
 *   11 = 1-byte suffix  (k=1, /24 networks)
 *   10 = 2-byte suffix  (k=2, /16 networks)
 *   01 = 8-byte suffix  (k<=8, IPv6 IID)
 *   00 = full address   (16B IPv6 / 4B IPv4)
 *
 * ns-3 implementation: uses k=1 (AM=11) for /24 simulation subnets.
 * suffix = ip.Get() & 0xFF (last octet).  This is the correct ESC
 * behavior for a /24 network, not a simplification.
 */
class CompressedEncoding
{
  public:
    /// Set the network prefix (call once during RoutingProtocol::Start).
    static void SetNetworkPrefix(Ipv4Address prefix);
    /// Get the current network prefix.
    static Ipv4Address GetNetworkPrefix();
    /// Whether the prefix has been set (for test safety).
    static bool IsInitialized();

    /// Compress an IPv4 address to a 1-byte suffix (ESC k=1, AM=11).
    static uint8_t IpToNodeId(Ipv4Address addr);
    /// Expand a 1-byte suffix back to a full IPv4 address (ESC k=1, AM=11).
    static Ipv4Address NodeIdToIp(uint8_t id);

    /**
     * @brief Encode a TTL (seconds) into a 4-bit bucket.
     *
     * bucket = min(15, ttl_seconds / 20).  Max encodable: 300s.
     * Decode: ttl = bucket * 20.
     */
    static uint8_t EncodeTtlBucket(uint16_t ttlSeconds);
    /// Decode a 4-bit TTL bucket back to seconds.
    static uint16_t DecodeTtlBucket(uint8_t bucket);

    /**
     * @brief Encode a sequence number into 2 bytes (uint16_t).
     *
     * Takes the lower 16 bits.  Sequence numbers wrap naturally.
     */
    static uint16_t CompressSeqNo(uint32_t seqNo);
    /// Expand a 2-byte sequence number back to uint32_t (zero-extended).
    static uint32_t ExpandSeqNo(uint16_t compressed);

    /**
     * @brief Encode a lifetime in milliseconds into 2 bytes.
     *
     * Uses a non-linear encoding: values up to 16383ms are stored directly
     * (bit 15 = 0).  Values above that use bit 15 = 1 and store
     * (ms / 100) in bits 0-14, giving a max of ~3276.7 seconds.
     */
    static uint16_t EncodeLifetime(uint32_t lifetimeMs);
    /// Decode a 2-byte lifetime back to milliseconds.
    static uint32_t DecodeLifetime(uint16_t encoded);

  private:
    static uint32_t s_networkPrefix;  ///< Network prefix (host byte order)
    static bool s_initialized;
};

/**
 * @ingroup tavrn
 * @brief TAVRN message types
 *
 * Maps to the spec's message table:
 *   E_RREQ, E_RREP, E_RERR       - Routing + Topology
 *   HELLO, TC_UPDATE               - Topology
 *   SYNC_OFFER, SYNC_PULL, SYNC_DATA - Topology (mentorship)
 */
enum MessageType : uint8_t
{
    TAVRN_E_RREQ = 1,      ///< Enhanced Route Request with piggybacked topology metadata
    TAVRN_E_RREP = 2,      ///< Enhanced Route Reply with piggybacked topology metadata
    TAVRN_E_RERR = 3,      ///< Enhanced Route Error with piggybacked topology metadata
    TAVRN_HELLO = 4,        ///< Node join announcement
    TAVRN_SYNC_OFFER = 5,   ///< Mentor offers topology sync to new node
    TAVRN_SYNC_PULL = 6,    ///< Mentee requests topology page from mentor
    TAVRN_SYNC_DATA = 7,    ///< Mentor sends topology page
    TAVRN_TC_UPDATE = 8,    ///< Broadcast on node join/leave events
    TAVRN_E_RREP_ACK = 9,  ///< RREP acknowledgment for unidirectional link detection
};

/**
 * @ingroup tavrn
 * @brief Type header - identifies the message type
 */
class TypeHeader : public Header
{
  public:
    TypeHeader(MessageType t = TAVRN_E_RREQ);

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    MessageType Get() const { return m_type; }
    bool IsValid() const { return m_valid; }
    bool operator==(const TypeHeader& o) const;

  private:
    MessageType m_type;
    bool m_valid;
};

std::ostream& operator<<(std::ostream& os, const TypeHeader& h);

// ============================================================================
// Topology Metadata Extension (piggybacked on E_RREQ/E_RREP/E_RERR)
// ============================================================================

/**
 * @ingroup tavrn
 * @brief A single GTT entry piggybacked as topology metadata.
 *
 * Represents one node's existence information:
 *   - nodeAddr: the node being described
 *   - ttlRemaining: TTL remaining on the sender's GTT entry for that node
 *   - flags: bit 0 = freshness_request (soft-expiry query)
 */
struct GttMetadataEntry
{
    Ipv4Address nodeAddr;     ///< Node address
    uint16_t ttlRemaining;    ///< TTL remaining (seconds)
    uint8_t flags;            ///< Bit 0: freshness_request

    static constexpr uint8_t FLAG_FRESHNESS_REQUEST = 0x01;
};

/**
 * @ingroup tavrn
 * @brief Topology metadata extension header.
 *
 * Appended to E_RREQ/E_RREP/E_RERR messages. Contains 0..N GTT metadata
 * entries for piggybacked topology information.
 *
 * Compressed wire format:
 *   [1B count][N * (1B nodeId + 1B packed{4-bit TTL bucket | 4-bit flags})]
 *   Entry: 2 bytes each (down from 7 bytes uncompressed).
 *   TTL bucket: min(15, ttl_seconds / 20), max encodable 300s.
 */
class TopologyMetadataHeader : public Header
{
  public:
    TopologyMetadataHeader();

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    void AddEntry(const GttMetadataEntry& entry);
    uint8_t GetEntryCount() const { return static_cast<uint8_t>(m_entries.size()); }
    const std::vector<GttMetadataEntry>& GetEntries() const { return m_entries; }
    void Clear() { m_entries.clear(); }

  private:
    std::vector<GttMetadataEntry> m_entries;
};

// ============================================================================
// Enhanced RREQ (E_RREQ) - AODV RREQ + topology metadata
// ============================================================================

/**
 * @ingroup tavrn
 * @brief Enhanced Route Request header
 *
 *   Compressed 10-byte wire format (802.15.4 optimized):
 *   [1B flags][1B hopCount][2B requestID][1B dstNodeId]
 *   [2B dstSeqNo][1B originNodeId][2B originSeqNo]
 *
 *   Addresses use 1-byte network-local node IDs.
 *   Sequence numbers use lower 16 bits.
 *   Request ID uses lower 16 bits.
 *
 * Topology metadata is carried in a separate TopologyMetadataHeader
 * appended after this header.
 */
class ERreqHeader : public Header
{
  public:
    ERreqHeader(uint8_t flags = 0,
                uint8_t hopCount = 0,
                uint32_t requestID = 0,
                Ipv4Address dst = Ipv4Address(),
                uint32_t dstSeqNo = 0,
                Ipv4Address origin = Ipv4Address(),
                uint32_t originSeqNo = 0);

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    // Field accessors
    void SetHopCount(uint8_t count) { m_hopCount = count; }
    uint8_t GetHopCount() const { return m_hopCount; }
    void SetId(uint32_t id) { m_requestID = id; }
    uint32_t GetId() const { return m_requestID; }
    void SetDst(Ipv4Address a) { m_dst = a; }
    Ipv4Address GetDst() const { return m_dst; }
    void SetDstSeqno(uint32_t s) { m_dstSeqNo = s; }
    uint32_t GetDstSeqno() const { return m_dstSeqNo; }
    void SetOrigin(Ipv4Address a) { m_origin = a; }
    Ipv4Address GetOrigin() const { return m_origin; }
    void SetOriginSeqno(uint32_t s) { m_originSeqNo = s; }
    uint32_t GetOriginSeqno() const { return m_originSeqNo; }

    // Flags
    void SetDestinationOnly(bool f);
    bool GetDestinationOnly() const;
    void SetUnknownSeqno(bool f);
    bool GetUnknownSeqno() const;
    void SetGratuitousRrep(bool f);
    bool GetGratuitousRrep() const;

    bool operator==(const ERreqHeader& o) const;

  private:
    uint8_t m_flags;
    uint8_t m_hopCount;
    uint32_t m_requestID;
    Ipv4Address m_dst;
    uint32_t m_dstSeqNo;
    Ipv4Address m_origin;
    uint32_t m_originSeqNo;
};

std::ostream& operator<<(std::ostream& os, const ERreqHeader& h);

// ============================================================================
// Enhanced RREP (E_RREP)
// ============================================================================

/**
 * @ingroup tavrn
 * @brief Enhanced Route Reply header
 *
 * Compressed 8-byte wire format (802.15.4 optimized):
 *   [1B flags][1B hopCount][1B dstNodeId][2B dstSeqNo]
 *   [1B originNodeId][2B lifetime]
 */
class ERrepHeader : public Header
{
  public:
    ERrepHeader(uint8_t hopCount = 0,
                Ipv4Address dst = Ipv4Address(),
                uint32_t dstSeqNo = 0,
                Ipv4Address origin = Ipv4Address(),
                Time lifetime = MilliSeconds(0));

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    void SetHopCount(uint8_t count) { m_hopCount = count; }
    uint8_t GetHopCount() const { return m_hopCount; }
    void SetDst(Ipv4Address a) { m_dst = a; }
    Ipv4Address GetDst() const { return m_dst; }
    void SetDstSeqno(uint32_t s) { m_dstSeqNo = s; }
    uint32_t GetDstSeqno() const { return m_dstSeqNo; }
    void SetOrigin(Ipv4Address a) { m_origin = a; }
    Ipv4Address GetOrigin() const { return m_origin; }
    void SetLifeTime(Time t);
    Time GetLifeTime() const;

    // Flags
    void SetAckRequired(bool f);
    bool GetAckRequired() const;
    /// Prefix size for AODV wire compatibility
    void SetPrefixSize(uint8_t s) { m_prefixSize = s; }
    uint8_t GetPrefixSize() const { return m_prefixSize; }

    bool operator==(const ERrepHeader& o) const;

  private:
    uint8_t m_flags;
    uint8_t m_prefixSize;  ///< replaces reserved(2) for AODV parity (19 bytes)
    uint8_t m_hopCount;
    Ipv4Address m_dst;
    uint32_t m_dstSeqNo;
    Ipv4Address m_origin;
    uint32_t m_lifeTime;  ///< In milliseconds
};

std::ostream& operator<<(std::ostream& os, const ERrepHeader& h);

// ============================================================================
// Enhanced RERR (E_RERR)
// ============================================================================

/**
 * @ingroup tavrn
 * @brief Enhanced Route Error header
 *
 * Compressed wire format: [1B flags][N * (1B dstNodeId + 2B seqNo)]
 * Size: 1 + 3N bytes (down from 3 + 8N).
 */
class ERerrHeader : public Header
{
  public:
    ERerrHeader();

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator i) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    bool AddUnDestination(Ipv4Address dst, uint32_t seqNo);
    bool RemoveUnDestination(std::pair<Ipv4Address, uint32_t>& un);
    void Clear();
    uint8_t GetDestCount() const { return static_cast<uint8_t>(m_unreachableDstSeqNo.size()); }

    void SetNoDelete(bool f);
    bool GetNoDelete() const;

    bool operator==(const ERerrHeader& o) const;

  private:
    uint8_t m_flag;
    uint8_t m_reserved;
    std::map<Ipv4Address, uint32_t> m_unreachableDstSeqNo;
};

std::ostream& operator<<(std::ostream& os, const ERerrHeader& h);

// ============================================================================
// HELLO header - Node join announcement
// ============================================================================

/**
 * @ingroup tavrn
 * @brief HELLO message header for node join announcements
 *
 * Compressed wire format:
 *   [1B nodeId][2B seqNo][1B flags]  = 4 bytes (down from 9)
 * flags bit 0: isNew (first join announcement)
 */
class HelloHeader : public Header
{
  public:
    HelloHeader(Ipv4Address nodeAddr = Ipv4Address(),
                uint32_t seqNo = 0,
                bool isNew = true);

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    Ipv4Address GetNodeAddr() const { return m_nodeAddr; }
    void SetNodeAddr(Ipv4Address a) { m_nodeAddr = a; }
    uint32_t GetSeqNo() const { return m_seqNo; }
    void SetSeqNo(uint32_t s) { m_seqNo = s; }
    bool GetIsNew() const { return m_isNew; }
    void SetIsNew(bool n) { m_isNew = n; }

  private:
    Ipv4Address m_nodeAddr;
    uint32_t m_seqNo;
    bool m_isNew;
};

std::ostream& operator<<(std::ostream& os, const HelloHeader& h);

// ============================================================================
// SYNC_OFFER header
// ============================================================================

/**
 * @ingroup tavrn
 * @brief SYNC_OFFER: Mentor offers topology sync to new node
 *
 * Compressed wire format:
 *   [1B mentorNodeId][1B gttSize][1B menteeNodeId]  = 3 bytes (down from 12)
 *   gttSize capped at 255 (sufficient for <254 node smart home mesh).
 */
class SyncOfferHeader : public Header
{
  public:
    SyncOfferHeader(Ipv4Address mentorAddr = Ipv4Address(),
                    uint32_t gttSize = 0,
                    Ipv4Address menteeAddr = Ipv4Address());

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    Ipv4Address GetMentorAddr() const { return m_mentorAddr; }
    void SetMentorAddr(Ipv4Address a) { m_mentorAddr = a; }
    uint32_t GetGttSize() const { return m_gttSize; }
    void SetGttSize(uint32_t s) { m_gttSize = s; }
    /// Mentee address so overhearing nodes know which mentee the offer targets.
    Ipv4Address GetMenteeAddr() const { return m_menteeAddr; }
    void SetMenteeAddr(Ipv4Address a) { m_menteeAddr = a; }

  private:
    Ipv4Address m_mentorAddr;
    uint32_t m_gttSize;
    Ipv4Address m_menteeAddr; ///< target mentee address for broadcast dampening
};

std::ostream& operator<<(std::ostream& os, const SyncOfferHeader& h);

// ============================================================================
// SYNC_PULL header
// ============================================================================

/**
 * @ingroup tavrn
 * @brief SYNC_PULL: Mentee requests topology page from mentor
 *
 * Compressed wire format:
 *   [1B index][1B count]  = 2 bytes (down from 8)
 *   Capped at 255 entries (sufficient for <254 node mesh).
 */
class SyncPullHeader : public Header
{
  public:
    SyncPullHeader(uint32_t index = 0, uint32_t count = 15);

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    uint32_t GetIndex() const { return m_index; }
    void SetIndex(uint32_t i) { m_index = i; }
    uint32_t GetCount() const { return m_count; }
    void SetCount(uint32_t c) { m_count = c; }

  private:
    uint32_t m_index;
    uint32_t m_count;
};

std::ostream& operator<<(std::ostream& os, const SyncPullHeader& h);

// ============================================================================
// SYNC_DATA header
// ============================================================================

/**
 * @ingroup tavrn
 * @brief SYNC_DATA: Mentor sends topology page
 *
 * Compressed wire format:
 *   [1B startIndex][1B totalEntries][1B entryCount]
 *   [N * (1B nodeId + 2B lastSeen + 1B ttlBucket + 2B seqNo + 1B hopCount)]
 *   Entry: 7 bytes each (down from 15 bytes uncompressed).
 *   Header overhead: 3 bytes (down from 9).
 */
struct SyncDataEntry
{
    Ipv4Address nodeAddr;
    uint32_t lastSeen;      ///< Timestamp (simulation seconds)
    uint16_t ttlRemaining;
    uint32_t seqNo;         ///< Carry sequence number from mentor's GTT
    uint8_t hopCount;       ///< Carry hop count from mentor's GTT
};

class SyncDataHeader : public Header
{
  public:
    SyncDataHeader();

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    void SetStartIndex(uint32_t i) { m_startIndex = i; }
    uint32_t GetStartIndex() const { return m_startIndex; }
    void SetTotalEntries(uint32_t t) { m_totalEntries = t; }
    uint32_t GetTotalEntries() const { return m_totalEntries; }
    void AddEntry(const SyncDataEntry& entry);
    uint8_t GetEntryCount() const { return static_cast<uint8_t>(m_entries.size()); }
    const std::vector<SyncDataEntry>& GetEntries() const { return m_entries; }

  private:
    uint32_t m_startIndex;
    uint32_t m_totalEntries;
    std::vector<SyncDataEntry> m_entries;
};

std::ostream& operator<<(std::ostream& os, const SyncDataHeader& h);

// ============================================================================
// TC-UPDATE header
// ============================================================================

/**
 * @ingroup tavrn
 * @brief TC-UPDATE: Broadcast on node join/leave events
 *
 * Compressed wire format:
 *   [1B originNodeId][2B seqNo][1B subjectNodeId][1B eventType][2B timestamp]
 *   = 7 bytes (down from 17).
 *
 * UUID = (originAddr, seqNo) - 64-bit unique identifier per spec
 * eventType: 0 = join, 1 = leave
 * Timestamp uses 2B (seconds, max ~18 hours, sufficient for simulation).
 */
class TcUpdateHeader : public Header
{
  public:
    enum EventType : uint8_t
    {
        NODE_JOIN = 0,
        NODE_LEAVE = 1,
    };

    TcUpdateHeader(Ipv4Address originAddr = Ipv4Address(),
                   uint32_t seqNo = 0,
                   Ipv4Address subjectAddr = Ipv4Address(),
                   EventType event = NODE_JOIN,
                   uint32_t timestamp = 0);

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    // UUID components
    Ipv4Address GetOriginAddr() const { return m_originAddr; }
    void SetOriginAddr(Ipv4Address a) { m_originAddr = a; }
    uint32_t GetSeqNo() const { return m_seqNo; }
    void SetSeqNo(uint32_t s) { m_seqNo = s; }

    // Event
    Ipv4Address GetSubjectAddr() const { return m_subjectAddr; }
    void SetSubjectAddr(Ipv4Address a) { m_subjectAddr = a; }
    EventType GetEventType() const { return m_eventType; }
    void SetEventType(EventType e) { m_eventType = e; }
    uint32_t GetTimestamp() const { return m_timestamp; }
    void SetTimestamp(uint32_t t) { m_timestamp = t; }

    /// Unique 64-bit UUID: combine origin + seqno
    uint64_t GetUuid() const;

    bool operator==(const TcUpdateHeader& o) const;

  private:
    Ipv4Address m_originAddr;  ///< Who generated this TC-UPDATE
    uint32_t m_seqNo;          ///< Local sequence number
    Ipv4Address m_subjectAddr; ///< Node that joined/left
    EventType m_eventType;
    uint32_t m_timestamp;      ///< Event time (simulation seconds)
};

std::ostream& operator<<(std::ostream& os, const TcUpdateHeader& h);

// ============================================================================
// E_RREP_ACK header — Unidirectional link detection
// ============================================================================

/**
 * @ingroup tavrn
 * @brief Route Reply Acknowledgment (E_RREP_ACK)
 *
 * Mirrors AODV's RrepAckHeader. 1-byte reserved field.
 * Sent in response to an RREP with the ACK flag set.
 */
class ERrepAckHeader : public Header
{
  public:
    ERrepAckHeader();

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    bool operator==(const ERrepAckHeader& o) const;

  private:
    uint8_t m_reserved;
};

std::ostream& operator<<(std::ostream& os, const ERrepAckHeader& h);

} // namespace tavrn
} // namespace ns3

#endif /* TAVRN_PACKET_H */
