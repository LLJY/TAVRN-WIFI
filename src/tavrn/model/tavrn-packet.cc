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
    // 1B count + N * (4B addr + 2B ttl + 1B flags)
    return 1 + static_cast<uint32_t>(m_entries.size()) * 7;
}

void
TopologyMetadataHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(static_cast<uint8_t>(m_entries.size()));
    for (const auto& entry : m_entries)
    {
        WriteTo(i, entry.nodeAddr);
        i.WriteHtonU16(entry.ttlRemaining);
        i.WriteU8(entry.flags);
    }
}

uint32_t
TopologyMetadataHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    uint8_t count = i.ReadU8();
    m_entries.clear();
    m_entries.reserve(count);
    for (uint8_t k = 0; k < count; ++k)
    {
        GttMetadataEntry entry;
        ReadFrom(i, entry.nodeAddr);
        entry.ttlRemaining = i.ReadNtohU16();
        entry.flags = i.ReadU8();
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
    // Match AODV wire format exactly (23 bytes per RFC 3561):
    // 1B flags + 1B reserved + 1B hopCount +
    // 4B requestID + 4B dst + 4B dstSeqNo + 4B origin + 4B originSeqNo = 23
    return 23;
}

void
ERreqHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(m_flags);
    i.WriteU8(0); // reserved
    i.WriteU8(m_hopCount);
    // removed extra reserved byte to match AODV 23-byte format
    i.WriteHtonU32(m_requestID);
    WriteTo(i, m_dst);
    i.WriteHtonU32(m_dstSeqNo);
    WriteTo(i, m_origin);
    i.WriteHtonU32(m_originSeqNo);
}

uint32_t
ERreqHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_flags = i.ReadU8();
    i.ReadU8(); // reserved
    m_hopCount = i.ReadU8();
    // removed extra reserved byte to match AODV 23-byte format
    m_requestID = i.ReadNtohU32();
    ReadFrom(i, m_dst);
    m_dstSeqNo = i.ReadNtohU32();
    ReadFrom(i, m_origin);
    m_originSeqNo = i.ReadNtohU32();

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
    // Match AODV wire format exactly (19 bytes per RFC 3561):
    // 1B flags + 1B prefixSize + 1B hopCount + 4B dst + 4B dstSeqNo + 4B origin + 4B lifetime = 19
    return 19;
}

void
ERrepHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(m_flags);
    i.WriteU8(m_prefixSize);  // prefixSize instead of 2B reserved
    i.WriteU8(m_hopCount);
    WriteTo(i, m_dst);
    i.WriteHtonU32(m_dstSeqNo);
    WriteTo(i, m_origin);
    i.WriteHtonU32(m_lifeTime);
}

uint32_t
ERrepHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_flags = i.ReadU8();
    m_prefixSize = i.ReadU8();
    m_hopCount = i.ReadU8();
    ReadFrom(i, m_dst);
    m_dstSeqNo = i.ReadNtohU32();
    ReadFrom(i, m_origin);
    m_lifeTime = i.ReadNtohU32();

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
    // 1B flag + 1B reserved + 1B destCount + N * (4B addr + 4B seq)
    return (3 + 8 * GetDestCount());
}

void
ERerrHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU8(m_flag);
    i.WriteU8(m_reserved);
    i.WriteU8(GetDestCount());
    for (auto j = m_unreachableDstSeqNo.begin(); j != m_unreachableDstSeqNo.end(); ++j)
    {
        WriteTo(i, j->first);
        i.WriteHtonU32(j->second);
    }
}

uint32_t
ERerrHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_flag = i.ReadU8();
    m_reserved = i.ReadU8();
    uint8_t dest = i.ReadU8();
    m_unreachableDstSeqNo.clear();
    Ipv4Address address;
    uint32_t seqNo;
    for (uint8_t k = 0; k < dest; ++k)
    {
        ReadFrom(i, address);
        seqNo = i.ReadNtohU32();
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
    // 4B nodeAddr + 4B seqNo + 1B flags = 9
    return 9;
}

void
HelloHeader::Serialize(Buffer::Iterator i) const
{
    WriteTo(i, m_nodeAddr);
    i.WriteHtonU32(m_seqNo);
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
    ReadFrom(i, m_nodeAddr);
    m_seqNo = i.ReadNtohU32();
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
    // 4B mentorAddr + 4B gttSize + 4B menteeAddr = 12
    return 12;
}

void
SyncOfferHeader::Serialize(Buffer::Iterator i) const
{
    WriteTo(i, m_mentorAddr);
    i.WriteHtonU32(m_gttSize);
    WriteTo(i, m_menteeAddr);
}

uint32_t
SyncOfferHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    ReadFrom(i, m_mentorAddr);
    m_gttSize = i.ReadNtohU32();
    ReadFrom(i, m_menteeAddr);

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
    // 4B index + 4B count = 8
    return 8;
}

void
SyncPullHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteHtonU32(m_index);
    i.WriteHtonU32(m_count);
}

uint32_t
SyncPullHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_index = i.ReadNtohU32();
    m_count = i.ReadNtohU32();

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
    // 4B startIndex + 4B totalEntries + 1B count
    // + N * (4B addr + 4B lastSeen + 2B ttl + 4B seqNo + 1B hopCount) = 15B each
    return 9 + static_cast<uint32_t>(m_entries.size()) * 15;
}

void
SyncDataHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteHtonU32(m_startIndex);
    i.WriteHtonU32(m_totalEntries);
    i.WriteU8(static_cast<uint8_t>(m_entries.size()));
    for (const auto& entry : m_entries)
    {
        WriteTo(i, entry.nodeAddr);
        i.WriteHtonU32(entry.lastSeen);
        i.WriteHtonU16(entry.ttlRemaining);
        i.WriteHtonU32(entry.seqNo);
        i.WriteU8(entry.hopCount);
    }
}

uint32_t
SyncDataHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_startIndex = i.ReadNtohU32();
    m_totalEntries = i.ReadNtohU32();
    uint8_t count = i.ReadU8();
    m_entries.clear();
    m_entries.reserve(count);
    for (uint8_t k = 0; k < count; ++k)
    {
        SyncDataEntry entry;
        ReadFrom(i, entry.nodeAddr);
        entry.lastSeen = i.ReadNtohU32();
        entry.ttlRemaining = i.ReadNtohU16();
        entry.seqNo = i.ReadNtohU32();
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
    // 4B originAddr + 4B seqNo + 4B subjectAddr + 1B eventType + 4B timestamp = 17
    return 17;
}

void
TcUpdateHeader::Serialize(Buffer::Iterator i) const
{
    WriteTo(i, m_originAddr);
    i.WriteHtonU32(m_seqNo);
    WriteTo(i, m_subjectAddr);
    i.WriteU8(static_cast<uint8_t>(m_eventType));
    i.WriteHtonU32(m_timestamp);
}

uint32_t
TcUpdateHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    ReadFrom(i, m_originAddr);
    m_seqNo = i.ReadNtohU32();
    ReadFrom(i, m_subjectAddr);
    m_eventType = static_cast<EventType>(i.ReadU8());
    m_timestamp = i.ReadNtohU32();

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
