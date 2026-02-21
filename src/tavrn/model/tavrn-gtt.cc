/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN: Topology Aware Vicinity-Reactive Network
 * Global Topology Table (GTT) implementation.
 *
 * This file implements the GTT data structure described in the TAVRN v2
 * specification.  The GTT is the core mechanism that gives TAVRN its
 * topology awareness while maintaining reactive routing efficiency.
 *
 * Key design decisions:
 *   - The GTT is a passive data structure: it never schedules timers or
 *     sends packets.  The owning RoutingProtocol polls for soft/hard
 *     expired entries and acts on them.
 *   - Timestamps are stored as absolute simulation times (not deltas)
 *     following the ns-3 AODV convention.
 *   - TracedCallbacks enable external observers (metrics, tests) to
 *     monitor topology changes without coupling.
 */

#include "tavrn-gtt.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <iomanip>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TavrnGtt");

namespace tavrn
{

// ===========================================================================
//  Construction
// ===========================================================================

GlobalTopologyTable::GlobalTopologyTable(Time defaultTtl, double softExpiryThreshold)
    : m_defaultTtl(defaultTtl),
      m_softExpiryThreshold(softExpiryThreshold)
{
    NS_LOG_FUNCTION(this << defaultTtl.As(Time::S) << softExpiryThreshold);
    NS_ASSERT_MSG(softExpiryThreshold >= 0.0 && softExpiryThreshold <= 1.0,
                  "Soft-expiry threshold must be in [0.0, 1.0], got " << softExpiryThreshold);
}

// ===========================================================================
//  Core operations
// ===========================================================================

bool
GlobalTopologyTable::AddOrUpdateEntry(Ipv4Address addr, uint32_t seqNo, uint16_t hopCount)
{
    NS_LOG_FUNCTION(this << addr << seqNo << hopCount);

    Time now = Simulator::Now();

    auto it = m_entries.find(addr);
    if (it != m_entries.end())
    {
        // Entry exists — only update if new seqNo >= existing seqNo
        if (seqNo < it->second.seqNo)
        {
            NS_LOG_LOGIC("Rejecting update for " << addr << ": existing seqNo "
                                                  << it->second.seqNo << " > incoming " << seqNo);
            return false;
        }

        // Use per-node TTL if set, otherwise table default
        Time effectiveTtl = (it->second.perNodeTtl.IsStrictlyPositive())
                                ? it->second.perNodeTtl
                                : m_defaultTtl;
        Time softOffset = Seconds(effectiveTtl.GetSeconds() * m_softExpiryThreshold);

        NS_LOG_LOGIC("Updating GTT entry for " << addr << " seqNo " << seqNo
                     << " effectiveTtl=" << effectiveTtl.As(Time::S));
        it->second.lastSeen = now;
        it->second.ttlExpiry = now + effectiveTtl;
        it->second.softExpiry = now + softOffset;
        it->second.seqNo = seqNo;
        it->second.hopCount = hopCount;

        // Resurrect if previously departed — fresh evidence means the node is back
        if (it->second.departed)
        {
            NS_LOG_LOGIC("Node " << addr << " resurrected (was departed)");
            it->second.departed = false;
            m_nodeJoinTrace(addr);
            m_sizeChangeTrace(NodeCount());
        }

        return true;
    }

    // New entry — uses table default TTL (perNodeTtl starts at 0 = "use default")
    Time softOffset = Seconds(m_defaultTtl.GetSeconds() * m_softExpiryThreshold);
    Time newSoftExpiry = now + softOffset;
    Time newTtlExpiry = now + m_defaultTtl;

    NS_LOG_LOGIC("Adding new GTT entry for " << addr << " seqNo " << seqNo);
    GttEntry entry(addr, now, newTtlExpiry, newSoftExpiry, seqNo, hopCount);
    m_entries.insert(std::make_pair(addr, entry));

    m_nodeJoinTrace(addr);
    m_sizeChangeTrace(NodeCount());

    return true;
}

bool
GlobalTopologyTable::RemoveEntry(Ipv4Address addr)
{
    NS_LOG_FUNCTION(this << addr);

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        NS_LOG_LOGIC("RemoveEntry: " << addr << " not found");
        return false;
    }

    bool wasDeparted = it->second.departed;
    m_entries.erase(it);

    if (!wasDeparted)
    {
        // Only fire leave trace if the node was still considered active
        m_nodeLeaveTrace(addr);
    }
    m_sizeChangeTrace(NodeCount());

    NS_LOG_LOGIC("Removed GTT entry for " << addr);
    return true;
}

bool
GlobalTopologyTable::LookupEntry(Ipv4Address addr, GttEntry& entry) const
{
    NS_LOG_FUNCTION(this << addr);

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        NS_LOG_LOGIC("LookupEntry: " << addr << " not found");
        return false;
    }

    entry = it->second;
    return true;
}

// ===========================================================================
//  Spec API — GTT Interface (For Applications)
// ===========================================================================

bool
GlobalTopologyTable::NodeExists(Ipv4Address addr) const
{
    NS_LOG_FUNCTION(this << addr);

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        return false;
    }
    return !it->second.departed;
}

Time
GlobalTopologyTable::LastSeen(Ipv4Address addr) const
{
    NS_LOG_FUNCTION(this << addr);

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        return Time(0);
    }
    return it->second.lastSeen;
}

Time
GlobalTopologyTable::TtlRemaining(Ipv4Address addr) const
{
    NS_LOG_FUNCTION(this << addr);

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        return Time(0);
    }
    return it->second.ttlExpiry - Simulator::Now();
}

std::vector<Ipv4Address>
GlobalTopologyTable::EnumerateNodes() const
{
    NS_LOG_FUNCTION(this);

    std::vector<Ipv4Address> result;
    result.reserve(m_entries.size());
    for (const auto& pair : m_entries)
    {
        if (!pair.second.departed)
        {
            result.push_back(pair.first);
        }
    }
    return result;
}

uint32_t
GlobalTopologyTable::NodeCount() const
{
    NS_LOG_FUNCTION(this);

    uint32_t count = 0;
    for (const auto& pair : m_entries)
    {
        if (!pair.second.departed)
        {
            ++count;
        }
    }
    return count;
}

// ===========================================================================
//  Expiry management — dual-TTL refresh mechanism
// ===========================================================================

std::vector<GttEntry>
GlobalTopologyTable::GetSoftExpiredEntries() const
{
    NS_LOG_FUNCTION(this);

    Time now = Simulator::Now();
    std::vector<GttEntry> result;

    for (const auto& pair : m_entries)
    {
        const GttEntry& e = pair.second;
        if (!e.departed && now >= e.softExpiry && now < e.ttlExpiry)
        {
            result.push_back(e);
        }
    }

    NS_LOG_LOGIC("GetSoftExpiredEntries: " << result.size() << " entries at t="
                                           << now.As(Time::S));
    return result;
}

std::vector<GttEntry>
GlobalTopologyTable::GetHardExpiredEntries() const
{
    NS_LOG_FUNCTION(this);

    Time now = Simulator::Now();
    std::vector<GttEntry> result;

    for (const auto& pair : m_entries)
    {
        const GttEntry& e = pair.second;
        if (!e.departed && now >= e.ttlExpiry)
        {
            result.push_back(e);
        }
    }

    NS_LOG_LOGIC("GetHardExpiredEntries: " << result.size() << " entries at t="
                                           << now.As(Time::S));
    return result;
}

void
GlobalTopologyTable::RefreshEntry(Ipv4Address addr, uint32_t seqNo)
{
    NS_LOG_FUNCTION(this << addr << seqNo);

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        NS_LOG_LOGIC("RefreshEntry: " << addr << " not found, ignoring");
        return;
    }

    // Use per-node TTL if set, otherwise table default
    Time effectiveTtl = (it->second.perNodeTtl.IsStrictlyPositive())
                            ? it->second.perNodeTtl
                            : m_defaultTtl;

    Time now = Simulator::Now();
    Time softOffset = Seconds(effectiveTtl.GetSeconds() * m_softExpiryThreshold);
    it->second.lastSeen = now;
    it->second.ttlExpiry = now + effectiveTtl;
    it->second.softExpiry = now + softOffset;
    it->second.seqNo = seqNo;

    // If this entry was previously marked departed but we now have
    // liveness evidence (e.g., from passive learning — we received a packet from
    // this node), resurrect it. Without this, false departures become
    // permanent because RefreshEntry couldn't clear the departed flag.
    if (it->second.departed)
    {
        NS_LOG_LOGIC("RefreshEntry: resurrecting " << addr << " (was departed)");
        it->second.departed = false;
        m_nodeJoinTrace(addr);
        m_sizeChangeTrace(NodeCount());
    }

    NS_LOG_LOGIC("Refreshed GTT entry for " << addr << " effectiveTtl="
                 << effectiveTtl.As(Time::S) << " new ttlExpiry="
                 << it->second.ttlExpiry.As(Time::S));
}

bool
GlobalTopologyTable::MarkDeparted(Ipv4Address addr)
{
    NS_LOG_FUNCTION(this << addr);

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        NS_LOG_LOGIC("MarkDeparted: " << addr << " not found");
        return false;
    }

    if (it->second.departed)
    {
        NS_LOG_LOGIC("MarkDeparted: " << addr << " already departed");
        return false;
    }

    it->second.departed = true;
    // Record the departure time in ttlExpiry so Purge() can calculate
    // how long the entry has been departed.
    it->second.ttlExpiry = Simulator::Now();

    NS_LOG_LOGIC("Node " << addr << " marked as departed");

    m_nodeLeaveTrace(addr);
    m_sizeChangeTrace(NodeCount());
    return true;
}

// ===========================================================================
//  Bulk operations — mentorship SYNC_DATA import/export
// ===========================================================================

std::vector<GttEntry>
GlobalTopologyTable::GetEntriesPage(uint32_t startIndex, uint32_t count) const
{
    NS_LOG_FUNCTION(this << startIndex << count);

    std::vector<GttEntry> result;
    if (startIndex >= m_entries.size())
    {
        return result;
    }

    result.reserve(std::min(static_cast<size_t>(count),
                            m_entries.size() - static_cast<size_t>(startIndex)));

    auto it = m_entries.begin();
    std::advance(it, startIndex);

    for (uint32_t i = 0; i < count && it != m_entries.end(); ++i, ++it)
    {
        result.push_back(it->second);
    }

    return result;
}

uint32_t
GlobalTopologyTable::GetTotalEntries() const
{
    NS_LOG_FUNCTION(this);
    return static_cast<uint32_t>(m_entries.size());
}

void
GlobalTopologyTable::MergeEntry(const GttEntry& entry)
{
    NS_LOG_FUNCTION(this << entry.nodeAddr << entry.seqNo);

    auto it = m_entries.find(entry.nodeAddr);
    if (it != m_entries.end())
    {
        // Only accept if the incoming entry has a >= sequence number
        if (entry.seqNo < it->second.seqNo)
        {
            NS_LOG_LOGIC("MergeEntry: skipping " << entry.nodeAddr
                                                  << " — local seqNo " << it->second.seqNo
                                                  << " > incoming " << entry.seqNo);
            return;
        }

        NS_LOG_LOGIC("MergeEntry: updating " << entry.nodeAddr << " from mentor data");
        bool wasDeparted = it->second.departed;
        it->second = entry;

        // If the node came back from departed state, fire traces
        if (wasDeparted && !entry.departed)
        {
            m_nodeJoinTrace(entry.nodeAddr);
            m_sizeChangeTrace(NodeCount());
        }
        else if (!wasDeparted && entry.departed)
        {
            m_nodeLeaveTrace(entry.nodeAddr);
            m_sizeChangeTrace(NodeCount());
        }
    }
    else
    {
        NS_LOG_LOGIC("MergeEntry: inserting new entry for " << entry.nodeAddr);
        m_entries.insert(std::make_pair(entry.nodeAddr, entry));

        if (!entry.departed)
        {
            m_nodeJoinTrace(entry.nodeAddr);
        }
        m_sizeChangeTrace(NodeCount());
    }
}

// ===========================================================================
//  Per-node adaptive TTL (Tier 2)
// ===========================================================================

void
GlobalTopologyTable::SetPerNodeTtl(Ipv4Address addr, Time perNodeTtl)
{
    NS_LOG_FUNCTION(this << addr << perNodeTtl.As(Time::S));

    auto it = m_entries.find(addr);
    if (it == m_entries.end())
    {
        NS_LOG_LOGIC("SetPerNodeTtl: " << addr << " not found, ignoring");
        return;
    }

    // Clamp to table default (per-node TTL is always <= global TTL)
    Time clamped = (perNodeTtl.IsStrictlyPositive() && perNodeTtl < m_defaultTtl)
                       ? perNodeTtl
                       : Time(0); // Time(0) = use default

    it->second.perNodeTtl = clamped;

    // Immediately recalculate expiry based on new effective TTL
    Time effectiveTtl = clamped.IsStrictlyPositive() ? clamped : m_defaultTtl;
    Time softOffset = Seconds(effectiveTtl.GetSeconds() * m_softExpiryThreshold);
    Time now = Simulator::Now();
    it->second.ttlExpiry = it->second.lastSeen + effectiveTtl;
    it->second.softExpiry = it->second.lastSeen + softOffset;

    NS_LOG_LOGIC("SetPerNodeTtl for " << addr << ": perNodeTtl="
                 << (clamped.IsStrictlyPositive() ? clamped.As(Time::S) : m_defaultTtl.As(Time::S))
                 << " ttlExpiry=" << it->second.ttlExpiry.As(Time::S));
}

void
GlobalTopologyTable::GrowPerNodeTtl(Ipv4Address addr, double alpha)
{
    NS_LOG_FUNCTION(this << addr << alpha);

    auto it = m_entries.find(addr);
    if (it == m_entries.end() || it->second.departed)
    {
        return;
    }

    if (!it->second.perNodeTtl.IsStrictlyPositive())
    {
        // Already at table default — nothing to grow
        return;
    }

    // EMA: new = alpha * old + (1 - alpha) * target
    double oldSec = it->second.perNodeTtl.GetSeconds();
    double targetSec = m_defaultTtl.GetSeconds();
    double newSec = alpha * oldSec + (1.0 - alpha) * targetSec;

    // Snap to default if within 5%
    if (newSec >= targetSec * 0.95)
    {
        it->second.perNodeTtl = Time(0); // revert to table default
        NS_LOG_LOGIC("GrowPerNodeTtl: " << addr << " snapped to default ("
                     << targetSec << "s)");
    }
    else
    {
        it->second.perNodeTtl = Seconds(newSec);
        NS_LOG_LOGIC("GrowPerNodeTtl: " << addr << " grew to " << newSec << "s");
    }
}

void
GlobalTopologyTable::ClampAllPerNodeTtls(Time ceiling)
{
    NS_LOG_FUNCTION(this << ceiling.As(Time::S));

    Time now = Simulator::Now();

    for (auto& pair : m_entries)
    {
        GttEntry& e = pair.second;
        if (e.departed)
        {
            continue;
        }

        // If entry uses the table default (perNodeTtl == 0) and default > ceiling,
        // set perNodeTtl to ceiling explicitly
        Time effectiveTtl = e.perNodeTtl.IsStrictlyPositive() ? e.perNodeTtl : m_defaultTtl;
        if (effectiveTtl > ceiling)
        {
            e.perNodeTtl = ceiling;
            // Recalculate expiry from lastSeen
            Time softOffset = Seconds(ceiling.GetSeconds() * m_softExpiryThreshold);
            e.ttlExpiry = e.lastSeen + ceiling;
            e.softExpiry = e.lastSeen + softOffset;

            NS_LOG_LOGIC("ClampAllPerNodeTtls: " << pair.first << " clamped to "
                         << ceiling.As(Time::S));
        }
    }
}

// ===========================================================================
//  Configuration
// ===========================================================================

void
GlobalTopologyTable::SetDefaultTtl(Time ttl)
{
    NS_LOG_FUNCTION(this << ttl.As(Time::S));
    m_defaultTtl = ttl;
}

Time
GlobalTopologyTable::GetDefaultTtl() const
{
    return m_defaultTtl;
}

void
GlobalTopologyTable::SetSoftExpiryThreshold(double ratio)
{
    NS_LOG_FUNCTION(this << ratio);
    NS_ASSERT_MSG(ratio >= 0.0 && ratio <= 1.0,
                  "Soft-expiry threshold must be in [0.0, 1.0], got " << ratio);
    m_softExpiryThreshold = ratio;
}

double
GlobalTopologyTable::GetSoftExpiryThreshold() const
{
    return m_softExpiryThreshold;
}

// ===========================================================================
//  Debug / diagnostics
// ===========================================================================

void
GlobalTopologyTable::Print(Ptr<OutputStreamWrapper> stream) const
{
    NS_LOG_FUNCTION(this);

    std::ostream* os = stream->GetStream();

    // Save and configure ostream formatting
    std::ios oldState(nullptr);
    oldState.copyfmt(*os);

    *os << std::resetiosflags(std::ios::adjustfield) << std::setiosflags(std::ios::left);
    *os << "\nTAVRN Global Topology Table (" << NodeCount() << " active / "
        << m_entries.size() << " total entries)\n";
    *os << "  Default TTL: " << m_defaultTtl.As(Time::S)
        << "  Soft-expiry threshold: " << m_softExpiryThreshold << "\n";
    *os << std::setw(18) << "Address"
        << std::setw(16) << "LastSeen"
        << std::setw(16) << "TTL-Remaining"
        << std::setw(10) << "SeqNo"
        << std::setw(8) << "Hops"
        << std::setw(10) << "Status"
        << std::endl;

    Time now = Simulator::Now();

    for (const auto& pair : m_entries)
    {
        const GttEntry& e = pair.second;

        std::ostringstream addrStr;
        addrStr << e.nodeAddr;

        std::ostringstream lastSeenStr;
        lastSeenStr << std::setprecision(2) << e.lastSeen.As(Time::S);

        std::ostringstream ttlStr;
        Time remaining = e.ttlExpiry - now;
        ttlStr << std::setprecision(2) << remaining.As(Time::S);

        std::string status;
        if (e.departed)
        {
            status = "DEPARTED";
        }
        else if (now >= e.ttlExpiry)
        {
            status = "HARD_EXP";
        }
        else if (now >= e.softExpiry)
        {
            status = "SOFT_EXP";
        }
        else
        {
            status = "ACTIVE";
        }

        *os << std::setw(18) << addrStr.str()
            << std::setw(16) << lastSeenStr.str()
            << std::setw(16) << ttlStr.str()
            << std::setw(10) << e.seqNo
            << std::setw(8) << e.hopCount
            << std::setw(10) << status
            << std::endl;
    }

    *os << std::endl;

    // Restore ostream state
    (*os).copyfmt(oldState);
}

void
GlobalTopologyTable::Purge()
{
    NS_LOG_FUNCTION(this);

    if (m_entries.empty())
    {
        return;
    }

    Time now = Simulator::Now();
    // Cleanup threshold: departed entries are removed after 2 * defaultTtl.
    // This gives the RoutingProtocol ample time to propagate TC-UPDATE messages
    // before the local record is garbage-collected.
    Time cleanupThreshold = m_defaultTtl + m_defaultTtl;

    uint32_t removedCount = 0;

    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (it->second.departed)
        {
            // ttlExpiry was set to departure time in MarkDeparted()
            Time departedDuration = now - it->second.ttlExpiry;
            if (departedDuration >= cleanupThreshold)
            {
                NS_LOG_LOGIC("Purging departed entry for " << it->first
                                                           << " (departed for "
                                                           << departedDuration.As(Time::S) << ")");
                auto tmp = it;
                ++it;
                m_entries.erase(tmp);
                ++removedCount;
                continue;
            }
        }
        ++it;
    }

    if (removedCount > 0)
    {
        NS_LOG_LOGIC("Purged " << removedCount << " departed entries");
        m_sizeChangeTrace(NodeCount());
    }
}

} // namespace tavrn
} // namespace ns3
