/* SPDX-License-Identifier: GPL-2.0-only */

#include "tavrn-id-cache.h"

#include <algorithm>

namespace ns3
{
namespace tavrn
{

bool
IdCache::IsDuplicate(Ipv4Address addr, uint32_t id)
{
    Purge();
    for (auto i = m_idCache.begin(); i != m_idCache.end(); ++i)
    {
        if (i->m_context == addr && i->m_id == id)
        {
            return true;
        }
    }
    UniqueId uniqueId = {addr, id, m_lifetime + Simulator::Now()};
    m_idCache.push_back(uniqueId);
    return false;
}

void
IdCache::Purge()
{
    m_idCache.erase(remove_if(m_idCache.begin(), m_idCache.end(), IsExpired()), m_idCache.end());
}

uint32_t
IdCache::GetSize()
{
    Purge();
    return m_idCache.size();
}

bool
IdCache::IsUuidSeen(uint64_t uuid) const
{
    Time now = Simulator::Now();
    auto i = m_uuidCache.find(uuid);
    if (i != m_uuidCache.end() && i->second >= now)
    {
        return true;
    }
    return false;
}

void
IdCache::InsertUuid(uint64_t uuid)
{
    PurgeUuids();
    m_uuidCache[uuid] = m_lifetime + Simulator::Now();
}

void
IdCache::PurgeUuids()
{
    Time now = Simulator::Now();
    for (auto i = m_uuidCache.begin(); i != m_uuidCache.end();)
    {
        if (i->second < now)
        {
            i = m_uuidCache.erase(i);
        }
        else
        {
            ++i;
        }
    }
}

} // namespace tavrn
} // namespace ns3
