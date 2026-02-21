/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef TAVRN_ID_CACHE_H
#define TAVRN_ID_CACHE_H

#include "ns3/ipv4-address.h"
#include "ns3/simulator.h"

#include <map>
#include <vector>

namespace ns3
{
namespace tavrn
{

/**
 * @ingroup tavrn
 *
 * @brief Unique packets identification cache used for simple duplicate detection.
 *
 * Provides both RREQ duplicate detection (same as AODV) and TC-UPDATE UUID
 * deduplication (TAVRN-specific).
 */
class IdCache
{
  public:
    /**
     * constructor
     * @param lifetime the lifetime for added entries
     */
    IdCache(Time lifetime)
        : m_lifetime(lifetime)
    {
    }

    /**
     * Check that entry (addr, id) exists in cache. Add entry, if it doesn't exist.
     * @param addr the IP address
     * @param id the cache entry ID
     * @returns true if the pair exists
     */
    bool IsDuplicate(Ipv4Address addr, uint32_t id);
    /// Remove all expired entries
    void Purge();
    /**
     * @returns number of entries in cache
     */
    uint32_t GetSize();

    /**
     * Set lifetime for future added entries.
     * @param lifetime the lifetime for entries
     */
    void SetLifetime(Time lifetime)
    {
        m_lifetime = lifetime;
    }

    /**
     * Return lifetime for existing entries in cache
     * @returns the lifetime
     */
    Time GetLifeTime() const
    {
        return m_lifetime;
    }

    /// @name TC-UPDATE UUID deduplication
    //\{
    /**
     * Check if a 64-bit UUID has been seen (for TC-UPDATE dedup)
     * @param uuid the 64-bit UUID to check
     * @returns true if the UUID has been seen within the expiry window
     */
    bool IsUuidSeen(uint64_t uuid) const;
    /**
     * Mark a UUID as seen
     * @param uuid the 64-bit UUID to record
     */
    void InsertUuid(uint64_t uuid);
    //\}

  private:
    /// Unique packet ID
    struct UniqueId
    {
        /// ID is supposed to be unique in single address context (e.g. sender address)
        Ipv4Address m_context;
        /// The id
        uint32_t m_id;
        /// When record will expire
        Time m_expire;
    };

    /**
     * @brief IsExpired structure
     */
    struct IsExpired
    {
        /**
         * @brief Check if the entry is expired
         *
         * @param u UniqueId entry
         * @return true if expired, false otherwise
         */
        bool operator()(const UniqueId& u) const
        {
            return (u.m_expire < Simulator::Now());
        }
    };

    /// Already seen IDs
    std::vector<UniqueId> m_idCache;
    /// Default lifetime for ID records
    Time m_lifetime;

    /// Already seen TC-UPDATE UUIDs mapped to their expiry time
    std::map<uint64_t, Time> m_uuidCache;

    /// Remove expired UUID entries
    void PurgeUuids();
};

} // namespace tavrn
} // namespace ns3

#endif /* TAVRN_ID_CACHE_H */
