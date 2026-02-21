/* SPDX-License-Identifier: GPL-2.0-only */

#include "tavrn-neighbor.h"

#include "ns3/log.h"
#include "ns3/wifi-mac-header.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TavrnNeighbors");

namespace tavrn
{

Neighbors::Neighbors(Time delay)
    : m_ntimer(Timer::CANCEL_ON_DESTROY)
{
    m_ntimer.SetDelay(delay);
    m_ntimer.SetFunction(&Neighbors::Purge, this);
    m_txErrorCallback = MakeCallback(&Neighbors::ProcessTxError, this);
}

bool
Neighbors::IsNeighbor(Ipv4Address addr)
{
    Purge();
    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_neighborAddress == addr)
        {
            return true;
        }
    }
    return false;
}

Time
Neighbors::GetExpireTime(Ipv4Address addr)
{
    Purge();
    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_neighborAddress == addr)
        {
            return (i->m_expireTime - Simulator::Now());
        }
    }
    return Time(0);
}

void
Neighbors::Update(Ipv4Address addr, Time expire)
{
    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_neighborAddress == addr)
        {
            i->m_expireTime = std::max(expire + Simulator::Now(), i->m_expireTime);
            if (i->m_hardwareAddress == Mac48Address())
            {
                i->m_hardwareAddress = LookupMacAddress(i->m_neighborAddress);
            }
            return;
        }
    }

    NS_LOG_LOGIC("Open link to " << addr);
    Neighbor neighbor(addr, LookupMacAddress(addr), expire + Simulator::Now());
    m_nb.push_back(neighbor);
    Purge();
}

/**
 * @brief CloseNeighbor structure
 */
struct CloseNeighbor
{
    /**
     * Check if the entry is expired
     *
     * @param nb Neighbors::Neighbor entry
     * @return true if expired, false otherwise
     */
    bool operator()(const Neighbors::Neighbor& nb) const
    {
        return ((nb.m_expireTime < Simulator::Now()) || nb.close);
    }
};

void
Neighbors::Purge()
{
    if (m_nb.empty())
    {
        return;
    }

    CloseNeighbor pred;
    if (!m_handleLinkFailure.IsNull())
    {
        for (auto j = m_nb.begin(); j != m_nb.end(); ++j)
        {
            if (pred(*j))
            {
                NS_LOG_LOGIC("Close link to " << j->m_neighborAddress);
                m_handleLinkFailure(j->m_neighborAddress);
            }
        }
    }
    m_nb.erase(std::remove_if(m_nb.begin(), m_nb.end(), pred), m_nb.end());
    m_ntimer.Cancel();
    m_ntimer.Schedule();
}

void
Neighbors::ScheduleTimer()
{
    m_ntimer.Cancel();
    m_ntimer.Schedule();
}

void
Neighbors::AddArpCache(Ptr<ArpCache> a)
{
    m_arp.push_back(a);
}

void
Neighbors::DelArpCache(Ptr<ArpCache> a)
{
    m_arp.erase(std::remove(m_arp.begin(), m_arp.end(), a), m_arp.end());
}

Mac48Address
Neighbors::LookupMacAddress(Ipv4Address addr)
{
    Mac48Address hwaddr;
    for (auto i = m_arp.begin(); i != m_arp.end(); ++i)
    {
        ArpCache::Entry* entry = (*i)->Lookup(addr);
        if (entry != nullptr && (entry->IsAlive() || entry->IsPermanent()) && !entry->IsExpired())
        {
            hwaddr = Mac48Address::ConvertFrom(entry->GetMacAddress());
            break;
        }
    }
    return hwaddr;
}

void
Neighbors::ProcessTxError(const WifiMacHeader& hdr)
{
    Mac48Address addr = hdr.GetAddr1();

    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_hardwareAddress == addr)
        {
            i->close = true;
        }
    }
    Purge();
}

Ipv4Address
Neighbors::LookupIpAddress(Mac48Address mac) const
{
    for (auto i = m_nb.begin(); i != m_nb.end(); ++i)
    {
        if (i->m_hardwareAddress == mac)
        {
            return i->m_neighborAddress;
        }
    }
    return Ipv4Address();
}

double
Neighbors::GetRssi(Ipv4Address neighbor) const
{
    auto i = m_rssiCache.find(neighbor);
    if (i != m_rssiCache.end())
    {
        return i->second;
    }
    return 0.0;
}

void
Neighbors::SetRssi(Ipv4Address neighbor, double rssi)
{
    m_rssiCache[neighbor] = rssi;
}

} // namespace tavrn
} // namespace ns3
