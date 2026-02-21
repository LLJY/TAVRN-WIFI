/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN: Topology Aware Vicinity-Reactive Network
 * Duplicate Packet Detection helper class.
 *
 * Design reference: AODV DPD (ns-3) for structural patterns.
 */

#ifndef TAVRN_DPD_H
#define TAVRN_DPD_H

#include "tavrn-id-cache.h"

#include "ns3/ipv4-header.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"

namespace ns3
{
namespace tavrn
{
/**
 * @ingroup tavrn
 *
 * @brief Helper class used to remember already seen packets and detect duplicates.
 *
 * Currently duplicate detection is based on unique packet ID given by Packet::GetUid ()
 * This approach is known to be weak (ns3::Packet UID is an internal identifier and not intended for
 * logical uniqueness in models) and should be changed.
 */
class DuplicatePacketDetection
{
  public:
    /**
     * Constructor
     * @param lifetime the lifetime for added entries
     */
    DuplicatePacketDetection(Time lifetime)
        : m_idCache(lifetime)
    {
    }

    /**
     * Check if the packet is a duplicate. If not, save information about this packet.
     * @param p the packet to check
     * @param header the IP header to check
     * @returns true if duplicate
     */
    bool IsDuplicate(Ptr<const Packet> p, const Ipv4Header& header);
    /**
     * Set duplicate record lifetime
     * @param lifetime the lifetime for duplicate records
     */
    void SetLifetime(Time lifetime);
    /**
     * Get duplicate record lifetime
     * @returns the duplicate record lifetime
     */
    Time GetLifetime() const;

  private:
    /// Impl
    IdCache m_idCache;
};

} // namespace tavrn
} // namespace ns3

#endif /* TAVRN_DPD_H */
