/*
 * TAVRN v2: Topology Aware Vicinity-Reactive Network
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tavrn-packet.h"

#include "ns3/address-utils.h"
#include "ns3/log.h"
#include "ns3/packet.h"

namespace ns3
{
namespace tavrn
{

NS_LOG_COMPONENT_DEFINE("TavrnPacket");

// ============================================================================
// CompressedEncoding — static members and helpers
// ============================================================================

uint32_t CompressedEncoding::s_networkPrefix = 0;
bool CompressedEncoding::s_initialized = false;

void
CompressedEncoding::SetNetworkPrefix(Ipv4Address prefix)
{
    // Store prefix with last octet zeroed: e.g., 10.1.1.0/24 -> 0x0A010100
    s_networkPrefix = prefix.Get() & 0xFFFFFF00;
    s_initialized = true;
    NS_LOG_INFO("CompressedEncoding: network prefix set to "
                << Ipv4Address(s_networkPrefix) << " (0x" << std::hex << s_networkPrefix
                << std::dec << ")");
}

Ipv4Address
CompressedEncoding::GetNetworkPrefix()
{
    return Ipv4Address(s_networkPrefix);
}

bool
CompressedEncoding::IsInitialized()
{
    return s_initialized;
}

uint8_t
CompressedEncoding::IpToNodeId(Ipv4Address addr)
{
    return static_cast<uint8_t>(addr.Get() & 0xFF);
}

Ipv4Address
CompressedEncoding::NodeIdToIp(uint8_t id)
{
    NS_ASSERT_MSG(s_initialized, "CompressedEncoding: network prefix not set");
    return Ipv4Address(s_networkPrefix | id);
}

uint8_t
CompressedEncoding::EncodeTtlBucket(uint16_t ttlSeconds)
{
    if (ttlSeconds == 0)
    {
        return 0;
    }
    // Round up: any nonzero TTL encodes as at least bucket 1 (20s).
    // Prevents living entries from appearing expired (bucket 0 = 0s)
    // and triggering unnecessary freshness responses.
    return static_cast<uint8_t>(std::min(15u, (static_cast<unsigned>(ttlSeconds) + 19) / 20));
}

uint16_t
CompressedEncoding::DecodeTtlBucket(uint8_t bucket)
{
    return static_cast<uint16_t>(bucket) * 20;
}

uint16_t
CompressedEncoding::CompressSeqNo(uint32_t seqNo)
{
    return static_cast<uint16_t>(seqNo & 0xFFFF);
}

uint32_t
CompressedEncoding::ExpandSeqNo(uint16_t compressed)
{
    return static_cast<uint32_t>(compressed);
}

uint16_t
CompressedEncoding::EncodeLifetime(uint32_t lifetimeMs)
{
    if (lifetimeMs <= 16383)
    {
        // Direct encoding: bit 15 = 0, bits 0-14 = ms
        return static_cast<uint16_t>(lifetimeMs);
    }
    // Scaled encoding: bit 15 = 1, bits 0-14 = ms / 100
    uint32_t scaled = lifetimeMs / 100;
    if (scaled > 32767)
    {
        scaled = 32767; // Max: 3276.7 seconds
    }
    return static_cast<uint16_t>(0x8000 | scaled);
}

uint32_t
CompressedEncoding::DecodeLifetime(uint16_t encoded)
{
    if ((encoded & 0x8000) == 0)
    {
        // Direct: bits 0-14 = ms
        return static_cast<uint32_t>(encoded);
    }
    // Scaled: bits 0-14 * 100 = ms
    return static_cast<uint32_t>(encoded & 0x7FFF) * 100;
}

// ============================================================================
// TypeHeader
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(TypeHeader);

TypeHeader::TypeHeader(MessageType t)
    : m_type(t),
      m_valid(true)
{
}

TypeId
TypeHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::TypeHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<TypeHeader>();
    return tid;
}

TypeId
TypeHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
TypeHeader::GetSerializedSize() const
{
    return 1;
}

void
TypeHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(static_cast<uint8_t>(m_type));
}

uint32_t
TypeHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    uint8_t type = i.ReadU8();
    m_valid = true;
    switch (type)
    {
    case TAVRN_E_RREQ:
    case TAVRN_E_RREP:
    case TAVRN_E_RERR:
    case TAVRN_HELLO:
    case TAVRN_SYNC_OFFER:
    case TAVRN_SYNC_PULL:
    case TAVRN_SYNC_DATA:
    case TAVRN_TC_UPDATE:
    case TAVRN_E_RREP_ACK: {
        m_type = static_cast<MessageType>(type);
        break;
    }
    default:
        m_valid = false;
    }
    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
TypeHeader::Print(std::ostream& os) const
{
    switch (m_type)
    {
    case TAVRN_E_RREQ:
        os << "E_RREQ";
        break;
    case TAVRN_E_RREP:
        os << "E_RREP";
        break;
    case TAVRN_E_RERR:
        os << "E_RERR";
        break;
    case TAVRN_HELLO:
        os << "HELLO";
        break;
    case TAVRN_SYNC_OFFER:
        os << "SYNC_OFFER";
        break;
    case TAVRN_SYNC_PULL:
        os << "SYNC_PULL";
        break;
    case TAVRN_SYNC_DATA:
        os << "SYNC_DATA";
        break;
    case TAVRN_TC_UPDATE:
        os << "TC_UPDATE";
        break;
    case TAVRN_E_RREP_ACK:
        os << "E_RREP_ACK";
        break;
    default:
        os << "UNKNOWN_TYPE";
    }
}

bool
TypeHeader::operator==(const TypeHeader& o) const
{
    return (m_type == o.m_type && m_valid == o.m_valid);
}

std::ostream&
operator<<(std::ostream& os, const TypeHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// TopologyMetadataHeader
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(TopologyMetadataHeader);

TopologyMetadataHeader::TopologyMetadataHeader()
{
}

TypeId
TopologyMetadataHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::TopologyMetadataHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<TopologyMetadataHeader>();
    return tid;
}

TypeId
TopologyMetadataHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
TopologyMetadataHeader::GetSerializedSize() const
{
    // Compressed: 1B count + 1B AM+flags + N * (1B nodeId + 1B packed{4-bit TTL bucket | 4-bit flags})
    return 2 + static_cast<uint32_t>(m_entries.size()) * 2;
}

void
TopologyMetadataHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(static_cast<uint8_t>(m_entries.size()));
    // AM + flags byte: [3b AM(111) | 5b reserved]
    i.WriteU8(0xE0); // 111_00000
    for (const auto& entry : m_entries)
    {
        i.WriteU8(CompressedEncoding::IpToNodeId(entry.nodeAddr));
        // Pack: high nibble = TTL bucket (4 bits), low nibble = flags (4 bits)
        uint8_t ttlBucket = CompressedEncoding::EncodeTtlBucket(entry.ttlRemaining);
        uint8_t packed = static_cast<uint8_t>((ttlBucket << 4) | (entry.flags & 0x0F));
        i.WriteU8(packed);
    }
}

uint32_t
TopologyMetadataHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    uint8_t count = i.ReadU8();
    i.ReadU8(); // AM + flags byte (skip — fixed AM=111)
    m_entries.clear();
    m_entries.reserve(count);
    for (uint8_t k = 0; k < count; ++k)
    {
        GttMetadataEntry entry;
        uint8_t nodeId = i.ReadU8();
        entry.nodeAddr = CompressedEncoding::NodeIdToIp(nodeId);
        uint8_t packed = i.ReadU8();
        uint8_t ttlBucket = (packed >> 4) & 0x0F;
        entry.ttlRemaining = CompressedEncoding::DecodeTtlBucket(ttlBucket);
        entry.flags = packed & 0x0F;
        m_entries.push_back(entry);
    }
    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
TopologyMetadataHeader::Print(std::ostream& os) const
{
    os << "TopologyMetadata entries=" << static_cast<uint32_t>(m_entries.size());
    for (const auto& entry : m_entries)
    {
        os << " (" << entry.nodeAddr << " ttl=" << entry.ttlRemaining
           << " flags=0x" << std::hex << static_cast<uint32_t>(entry.flags) << std::dec << ")";
    }
}

void
TopologyMetadataHeader::AddEntry(const GttMetadataEntry& entry)
{
    NS_ASSERT(m_entries.size() < 255);
    m_entries.push_back(entry);
}

// ============================================================================
// ERreqHeader (Enhanced RREQ)
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(ERreqHeader);

ERreqHeader::ERreqHeader(uint8_t flags,
                         uint8_t hopCount,
                         uint32_t requestID,
                         Ipv4Address dst,
                         uint32_t dstSeqNo,
                         Ipv4Address origin,
                         uint32_t originSeqNo)
    : m_flags(flags),
      m_hopCount(hopCount),
      m_requestID(requestID),
      m_dst(dst),
      m_dstSeqNo(dstSeqNo),
      m_origin(origin),
      m_originSeqNo(originSeqNo)
{
}

TypeId
ERreqHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::ERreqHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<ERreqHeader>();
    return tid;
}

TypeId
ERreqHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
ERreqHeader::GetSerializedSize() const
{
    // Compressed: 1B flags + 1B hopCount + 2B requestID +
    // 1B dstNodeId + 2B dstSeqNo + 1B originNodeId + 2B originSeqNo = 10
    return 10;
}

void
ERreqHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(m_flags);
    i.WriteU8(m_hopCount);
    i.WriteHtonU16(static_cast<uint16_t>(m_requestID & 0xFFFF));
    i.WriteU8(CompressedEncoding::IpToNodeId(m_dst));
    i.WriteHtonU16(CompressedEncoding::CompressSeqNo(m_dstSeqNo));
    i.WriteU8(CompressedEncoding::IpToNodeId(m_origin));
    i.WriteHtonU16(CompressedEncoding::CompressSeqNo(m_originSeqNo));
}

uint32_t
ERreqHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_flags = i.ReadU8();
    m_hopCount = i.ReadU8();
    m_requestID = static_cast<uint32_t>(i.ReadNtohU16());
    m_dst = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_dstSeqNo = CompressedEncoding::ExpandSeqNo(i.ReadNtohU16());
    m_origin = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_originSeqNo = CompressedEncoding::ExpandSeqNo(i.ReadNtohU16());

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
ERreqHeader::Print(std::ostream& os) const
{
    os << "E_RREQ ID " << m_requestID;
    os << " destination: ipv4 " << m_dst << " sequence number " << m_dstSeqNo;
    os << " source: ipv4 " << m_origin << " sequence number " << m_originSeqNo;
    os << " flags: GratuitousRrep " << GetGratuitousRrep()
       << " DestinationOnly " << GetDestinationOnly()
       << " UnknownSeqno " << GetUnknownSeqno();
}

void
ERreqHeader::SetGratuitousRrep(bool f)
{
    if (f)
    {
        m_flags |= (1 << 5);
    }
    else
    {
        m_flags &= ~(1 << 5);
    }
}

bool
ERreqHeader::GetGratuitousRrep() const
{
    return (m_flags & (1 << 5));
}

void
ERreqHeader::SetDestinationOnly(bool f)
{
    if (f)
    {
        m_flags |= (1 << 4);
    }
    else
    {
        m_flags &= ~(1 << 4);
    }
}

bool
ERreqHeader::GetDestinationOnly() const
{
    return (m_flags & (1 << 4));
}

void
ERreqHeader::SetUnknownSeqno(bool f)
{
    if (f)
    {
        m_flags |= (1 << 3);
    }
    else
    {
        m_flags &= ~(1 << 3);
    }
}

bool
ERreqHeader::GetUnknownSeqno() const
{
    return (m_flags & (1 << 3));
}

bool
ERreqHeader::operator==(const ERreqHeader& o) const
{
    return (m_flags == o.m_flags && m_hopCount == o.m_hopCount &&
            m_requestID == o.m_requestID && m_dst == o.m_dst &&
            m_dstSeqNo == o.m_dstSeqNo && m_origin == o.m_origin &&
            m_originSeqNo == o.m_originSeqNo);
}

std::ostream&
operator<<(std::ostream& os, const ERreqHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// ERrepHeader (Enhanced RREP)
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(ERrepHeader);

ERrepHeader::ERrepHeader(uint8_t hopCount,
                         Ipv4Address dst,
                         uint32_t dstSeqNo,
                         Ipv4Address origin,
                         Time lifetime)
    : m_flags(0),
      m_prefixSize(0),
      m_hopCount(hopCount),
      m_dst(dst),
      m_dstSeqNo(dstSeqNo),
      m_origin(origin)
{
    m_lifeTime = static_cast<uint32_t>(lifetime.GetMilliSeconds());
}

TypeId
ERrepHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::ERrepHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<ERrepHeader>();
    return tid;
}

TypeId
ERrepHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
ERrepHeader::GetSerializedSize() const
{
    // Compressed: 1B flags + 1B hopCount + 1B dstNodeId + 2B dstSeqNo +
    // 1B originNodeId + 2B lifetime = 8
    return 8;
}

void
ERrepHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(m_flags);
    i.WriteU8(m_hopCount);
    i.WriteU8(CompressedEncoding::IpToNodeId(m_dst));
    i.WriteHtonU16(CompressedEncoding::CompressSeqNo(m_dstSeqNo));
    i.WriteU8(CompressedEncoding::IpToNodeId(m_origin));
    i.WriteHtonU16(CompressedEncoding::EncodeLifetime(m_lifeTime));
}

uint32_t
ERrepHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_flags = i.ReadU8();
    m_hopCount = i.ReadU8();
    m_dst = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_dstSeqNo = CompressedEncoding::ExpandSeqNo(i.ReadNtohU16());
    m_origin = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_lifeTime = CompressedEncoding::DecodeLifetime(i.ReadNtohU16());

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
ERrepHeader::Print(std::ostream& os) const
{
    os << "E_RREP destination: ipv4 " << m_dst << " sequence number " << m_dstSeqNo;
    os << " source: ipv4 " << m_origin;
    os << " lifetime " << m_lifeTime;
    os << " acknowledgment required " << GetAckRequired();
}

void
ERrepHeader::SetLifeTime(Time t)
{
    m_lifeTime = static_cast<uint32_t>(t.GetMilliSeconds());
}

Time
ERrepHeader::GetLifeTime() const
{
    return MilliSeconds(m_lifeTime);
}

void
ERrepHeader::SetAckRequired(bool f)
{
    if (f)
    {
        m_flags |= (1 << 6);
    }
    else
    {
        m_flags &= ~(1 << 6);
    }
}

bool
ERrepHeader::GetAckRequired() const
{
    return (m_flags & (1 << 6));
}

bool
ERrepHeader::operator==(const ERrepHeader& o) const
{
    return (m_flags == o.m_flags && m_prefixSize == o.m_prefixSize &&
            m_hopCount == o.m_hopCount && m_dst == o.m_dst &&
            m_dstSeqNo == o.m_dstSeqNo && m_origin == o.m_origin &&
            m_lifeTime == o.m_lifeTime);
}

std::ostream&
operator<<(std::ostream& os, const ERrepHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// ERerrHeader (Enhanced RERR)
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(ERerrHeader);

ERerrHeader::ERerrHeader()
    : m_flag(0),
      m_reserved(0)
{
}

TypeId
ERerrHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::ERerrHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<ERerrHeader>();
    return tid;
}

TypeId
ERerrHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
ERerrHeader::GetSerializedSize() const
{
    // Compressed: 2B flags + N * (1B dstNodeId + 2B seqNo) = 2 + 3N
    // Byte 0: [N(1) | AM_d(3) | A(1) | destCount_hi(3)]
    // Byte 1: [destCount_lo(5) | reserved(3)]
    return (2 + 3 * GetDestCount());
}

void
ERerrHeader::Serialize(Buffer::Iterator i) const
{
    // 2-byte flags layout (3-bit AM):
    //   byte 0: bit 7 = N, bits 6-4 = AM_d (111), bit 3 = A, bits 2-0 = destCount high 3 bits
    //   byte 1: bits 7-3 = destCount low 5 bits, bits 2-0 = reserved
    uint8_t count = GetDestCount(); // now supports up to 255
    uint8_t byte0 = ((m_flag & 0x01) << 7) // bit 7: N
                   | (0x07 << 4)            // bits 6-4: AM=111 (1-byte suffix)
                   | (0 << 3)              // bit 3: A=0 (no ambiguity)
                   | ((count >> 5) & 0x07); // bits 2-0: destCount high 3
    uint8_t byte1 = (count & 0x1F) << 3;   // bits 7-3: destCount low 5, bits 2-0: reserved
    i.WriteU8(byte0);
    i.WriteU8(byte1);

    uint8_t written = 0;
    for (auto j = m_unreachableDstSeqNo.begin();
         j != m_unreachableDstSeqNo.end() && written < count;
         ++j, ++written)
    {
        i.WriteU8(CompressedEncoding::IpToNodeId(j->first));
        i.WriteHtonU16(CompressedEncoding::CompressSeqNo(j->second));
    }
}

uint32_t
ERerrHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    uint8_t byte0 = i.ReadU8();
    uint8_t byte1 = i.ReadU8();
    m_flag = (byte0 >> 7) & 0x01;      // bit 7: N
    m_reserved = 0;
    // bits 6-4: AM_d (ignored in ns-3, always k=1)
    // bit 3: A (ambiguity flag, ignored for now)
    uint8_t dest = ((byte0 & 0x07) << 5) | ((byte1 >> 3) & 0x1F);  // 8-bit destCount
    m_unreachableDstSeqNo.clear();
    for (uint8_t k = 0; k < dest; ++k)
    {
        uint8_t nodeId = i.ReadU8();
        Ipv4Address address = CompressedEncoding::NodeIdToIp(nodeId);
        uint32_t seqNo = CompressedEncoding::ExpandSeqNo(i.ReadNtohU16());
        m_unreachableDstSeqNo.insert(std::make_pair(address, seqNo));
    }

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
ERerrHeader::Print(std::ostream& os) const
{
    os << "E_RERR unreachable destinations (ipv4 address, seq. number):";
    for (auto j = m_unreachableDstSeqNo.begin(); j != m_unreachableDstSeqNo.end(); ++j)
    {
        os << " (" << j->first << ", " << j->second << ")";
    }
    os << " No delete flag " << GetNoDelete();
}

void
ERerrHeader::SetNoDelete(bool f)
{
    if (f)
    {
        m_flag |= (1 << 0);
    }
    else
    {
        m_flag &= ~(1 << 0);
    }
}

bool
ERerrHeader::GetNoDelete() const
{
    return (m_flag & (1 << 0));
}

bool
ERerrHeader::AddUnDestination(Ipv4Address dst, uint32_t seqNo)
{
    if (m_unreachableDstSeqNo.find(dst) != m_unreachableDstSeqNo.end())
    {
        return true;
    }

    NS_ASSERT(GetDestCount() < 255);
    m_unreachableDstSeqNo.insert(std::make_pair(dst, seqNo));
    return true;
}

bool
ERerrHeader::RemoveUnDestination(std::pair<Ipv4Address, uint32_t>& un)
{
    if (m_unreachableDstSeqNo.empty())
    {
        return false;
    }
    auto i = m_unreachableDstSeqNo.begin();
    un = *i;
    m_unreachableDstSeqNo.erase(i);
    return true;
}

void
ERerrHeader::Clear()
{
    m_unreachableDstSeqNo.clear();
    m_flag = 0;
    m_reserved = 0;
}

bool
ERerrHeader::operator==(const ERerrHeader& o) const
{
    if (m_flag != o.m_flag || m_reserved != o.m_reserved || GetDestCount() != o.GetDestCount())
    {
        return false;
    }

    auto j = m_unreachableDstSeqNo.begin();
    auto k = o.m_unreachableDstSeqNo.begin();
    for (uint8_t i = 0; i < GetDestCount(); ++i)
    {
        if ((j->first != k->first) || (j->second != k->second))
        {
            return false;
        }
        j++;
        k++;
    }
    return true;
}

std::ostream&
operator<<(std::ostream& os, const ERerrHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// HelloHeader
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(HelloHeader);

HelloHeader::HelloHeader(Ipv4Address nodeAddr, uint32_t seqNo, bool isNew)
    : m_nodeAddr(nodeAddr),
      m_seqNo(seqNo),
      m_isNew(isNew)
{
}

TypeId
HelloHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::HelloHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<HelloHeader>();
    return tid;
}

TypeId
HelloHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
HelloHeader::GetSerializedSize() const
{
    // Compressed: 1B nodeId + 2B seqNo + 1B flags = 4
    return 4;
}

void
HelloHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(CompressedEncoding::IpToNodeId(m_nodeAddr));
    i.WriteHtonU16(CompressedEncoding::CompressSeqNo(m_seqNo));
    uint8_t flags = 0;
    if (m_isNew)
    {
        flags |= (1 << 0);
    }
    i.WriteU8(flags);
}

uint32_t
HelloHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_nodeAddr = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_seqNo = CompressedEncoding::ExpandSeqNo(i.ReadNtohU16());
    uint8_t flags = i.ReadU8();
    m_isNew = (flags & (1 << 0));

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
HelloHeader::Print(std::ostream& os) const
{
    os << "HELLO node=" << m_nodeAddr << " seqNo=" << m_seqNo << " isNew=" << m_isNew;
}

std::ostream&
operator<<(std::ostream& os, const HelloHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// SyncOfferHeader
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(SyncOfferHeader);

SyncOfferHeader::SyncOfferHeader(Ipv4Address mentorAddr, uint32_t gttSize, Ipv4Address menteeAddr)
    : m_mentorAddr(mentorAddr),
      m_gttSize(gttSize),
      m_menteeAddr(menteeAddr)
{
}

TypeId
SyncOfferHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::SyncOfferHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<SyncOfferHeader>();
    return tid;
}

TypeId
SyncOfferHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
SyncOfferHeader::GetSerializedSize() const
{
    // Compressed: 1B AM(mentor+mentee) + 1B mentorNodeId + 1B gttSize + 1B menteeNodeId = 4
    // AM byte: [3b AM_mentor | 3b AM_mentee | 2b reserved]
    return 4;
}

void
SyncOfferHeader::Serialize(Buffer::Iterator i) const
{
    // AM byte: both addresses use AM=111 (1-byte suffix)
    i.WriteU8(0xFC); // 111_111_00
    i.WriteU8(CompressedEncoding::IpToNodeId(m_mentorAddr));
    i.WriteU8(static_cast<uint8_t>(std::min(m_gttSize, 255u)));
    i.WriteU8(CompressedEncoding::IpToNodeId(m_menteeAddr));
}

uint32_t
SyncOfferHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    i.ReadU8(); // AM byte (skip — fixed AM=111 for both addresses)
    m_mentorAddr = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_gttSize = static_cast<uint32_t>(i.ReadU8());
    m_menteeAddr = CompressedEncoding::NodeIdToIp(i.ReadU8());

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
SyncOfferHeader::Print(std::ostream& os) const
{
    os << "SYNC_OFFER mentor=" << m_mentorAddr << " gttSize=" << m_gttSize
       << " mentee=" << m_menteeAddr;
}

std::ostream&
operator<<(std::ostream& os, const SyncOfferHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// SyncPullHeader
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(SyncPullHeader);

SyncPullHeader::SyncPullHeader(uint32_t index, uint32_t count)
    : m_index(index),
      m_count(count)
{
}

TypeId
SyncPullHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::SyncPullHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<SyncPullHeader>();
    return tid;
}

TypeId
SyncPullHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
SyncPullHeader::GetSerializedSize() const
{
    // Compressed: 1B index + 1B count = 2
    return 2;
}

void
SyncPullHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(static_cast<uint8_t>(std::min(m_index, 255u)));
    i.WriteU8(static_cast<uint8_t>(std::min(m_count, 255u)));
}

uint32_t
SyncPullHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_index = static_cast<uint32_t>(i.ReadU8());
    m_count = static_cast<uint32_t>(i.ReadU8());

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
SyncPullHeader::Print(std::ostream& os) const
{
    os << "SYNC_PULL index=" << m_index << " count=" << m_count;
}

std::ostream&
operator<<(std::ostream& os, const SyncPullHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// SyncDataHeader
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(SyncDataHeader);

SyncDataHeader::SyncDataHeader()
    : m_startIndex(0),
      m_totalEntries(0)
{
}

TypeId
SyncDataHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::SyncDataHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<SyncDataHeader>();
    return tid;
}

TypeId
SyncDataHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
SyncDataHeader::GetSerializedSize() const
{
    // Compressed: 1B startIndex + 1B totalEntries + 1B count
    // + N * (1B nodeId + 2B lastSeen + 1B ttlBucket + 2B seqNo + 1B hopCount) = 7B each
    return 3 + static_cast<uint32_t>(m_entries.size()) * 7;
}

void
SyncDataHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(static_cast<uint8_t>(std::min(m_startIndex, 255u)));
    i.WriteU8(static_cast<uint8_t>(std::min(m_totalEntries, 255u)));
    i.WriteU8(static_cast<uint8_t>(m_entries.size()));
    for (const auto& entry : m_entries)
    {
        i.WriteU8(CompressedEncoding::IpToNodeId(entry.nodeAddr));
        i.WriteHtonU16(static_cast<uint16_t>(std::min(entry.lastSeen, 65535u)));
        i.WriteU8(CompressedEncoding::EncodeTtlBucket(entry.ttlRemaining));
        i.WriteHtonU16(CompressedEncoding::CompressSeqNo(entry.seqNo));
        i.WriteU8(entry.hopCount);
    }
}

uint32_t
SyncDataHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_startIndex = static_cast<uint32_t>(i.ReadU8());
    m_totalEntries = static_cast<uint32_t>(i.ReadU8());
    uint8_t count = i.ReadU8();
    m_entries.clear();
    m_entries.reserve(count);
    for (uint8_t k = 0; k < count; ++k)
    {
        SyncDataEntry entry;
        entry.nodeAddr = CompressedEncoding::NodeIdToIp(i.ReadU8());
        entry.lastSeen = static_cast<uint32_t>(i.ReadNtohU16());
        entry.ttlRemaining = CompressedEncoding::DecodeTtlBucket(i.ReadU8());
        entry.seqNo = CompressedEncoding::ExpandSeqNo(i.ReadNtohU16());
        entry.hopCount = i.ReadU8();
        m_entries.push_back(entry);
    }

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
SyncDataHeader::Print(std::ostream& os) const
{
    os << "SYNC_DATA startIndex=" << m_startIndex
       << " totalEntries=" << m_totalEntries
       << " count=" << static_cast<uint32_t>(m_entries.size());
    for (const auto& entry : m_entries)
    {
        os << " (" << entry.nodeAddr << " lastSeen=" << entry.lastSeen
           << " ttl=" << entry.ttlRemaining << ")";
    }
}

void
SyncDataHeader::AddEntry(const SyncDataEntry& entry)
{
    NS_ASSERT(m_entries.size() < 255);
    m_entries.push_back(entry);
}

std::ostream&
operator<<(std::ostream& os, const SyncDataHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// TcUpdateHeader
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(TcUpdateHeader);

TcUpdateHeader::TcUpdateHeader(Ipv4Address originAddr,
                               uint32_t seqNo,
                               Ipv4Address subjectAddr,
                               EventType event,
                               uint32_t timestamp)
    : m_originAddr(originAddr),
      m_seqNo(seqNo),
      m_subjectAddr(subjectAddr),
      m_eventType(event),
      m_timestamp(timestamp)
{
}

TypeId
TcUpdateHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::TcUpdateHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<TcUpdateHeader>();
    return tid;
}

TypeId
TcUpdateHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
TcUpdateHeader::GetSerializedSize() const
{
    // Compressed: 1B AM(origin+subject) + 1B originNodeId + 2B seqNo + 1B subjectNodeId + 1B eventType + 2B timestamp = 8
    // AM byte: [3b AM_origin | 3b AM_subject | 2b reserved]
    return 8;
}

void
TcUpdateHeader::Serialize(Buffer::Iterator i) const
{
    // AM byte: both addresses use AM=111 (1-byte suffix)
    i.WriteU8(0xFC); // 111_111_00
    i.WriteU8(CompressedEncoding::IpToNodeId(m_originAddr));
    i.WriteHtonU16(CompressedEncoding::CompressSeqNo(m_seqNo));
    i.WriteU8(CompressedEncoding::IpToNodeId(m_subjectAddr));
    i.WriteU8(static_cast<uint8_t>(m_eventType));
    i.WriteHtonU16(static_cast<uint16_t>(std::min(m_timestamp, 65535u)));
}

uint32_t
TcUpdateHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    i.ReadU8(); // AM byte (skip — fixed AM=111 for both addresses)
    m_originAddr = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_seqNo = CompressedEncoding::ExpandSeqNo(i.ReadNtohU16());
    m_subjectAddr = CompressedEncoding::NodeIdToIp(i.ReadU8());
    m_eventType = static_cast<EventType>(i.ReadU8());
    m_timestamp = static_cast<uint32_t>(i.ReadNtohU16());

    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
TcUpdateHeader::Print(std::ostream& os) const
{
    os << "TC_UPDATE origin=" << m_originAddr << " seqNo=" << m_seqNo
       << " subject=" << m_subjectAddr
       << " event=" << (m_eventType == NODE_JOIN ? "JOIN" : "LEAVE")
       << " timestamp=" << m_timestamp;
}

uint64_t
TcUpdateHeader::GetUuid() const
{
    return (static_cast<uint64_t>(m_originAddr.Get()) << 32) | m_seqNo;
}

bool
TcUpdateHeader::operator==(const TcUpdateHeader& o) const
{
    return (m_originAddr == o.m_originAddr && m_seqNo == o.m_seqNo &&
            m_subjectAddr == o.m_subjectAddr && m_eventType == o.m_eventType &&
            m_timestamp == o.m_timestamp);
}

std::ostream&
operator<<(std::ostream& os, const TcUpdateHeader& h)
{
    h.Print(os);
    return os;
}

// ============================================================================
// ERrepAckHeader — Unidirectional link detection
// ============================================================================

NS_OBJECT_ENSURE_REGISTERED(ERrepAckHeader);

ERrepAckHeader::ERrepAckHeader()
    : m_reserved(0)
{
}

TypeId
ERrepAckHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::tavrn::ERrepAckHeader")
                            .SetParent<Header>()
                            .SetGroupName("Tavrn")
                            .AddConstructor<ERrepAckHeader>();
    return tid;
}

TypeId
ERrepAckHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
ERrepAckHeader::GetSerializedSize() const
{
    return 1;
}

void
ERrepAckHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(m_reserved);
}

uint32_t
ERrepAckHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_reserved = i.ReadU8();
    uint32_t dist = i.GetDistanceFrom(start);
    NS_ASSERT(dist == GetSerializedSize());
    return dist;
}

void
ERrepAckHeader::Print(std::ostream& os) const
{
    os << "E_RREP_ACK";
}

bool
ERrepAckHeader::operator==(const ERrepAckHeader& o) const
{
    return m_reserved == o.m_reserved;
}

std::ostream&
operator<<(std::ostream& os, const ERrepAckHeader& h)
{
    h.Print(os);
    return os;
}

} // namespace tavrn
} // namespace ns3
