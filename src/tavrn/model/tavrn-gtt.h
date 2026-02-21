/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN: Topology Aware Vicinity-Reactive Network
 * Global Topology Table (GTT) — the core data structure providing
 * topology awareness for service discovery layers.
 *
 * Design reference: AODV RoutingTable (ns-3) for structural patterns.
 * Protocol reference: TAVRN v2 specification.
 */

#ifndef TAVRN_GTT_H
#define TAVRN_GTT_H

#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/simulator.h"
#include "ns3/traced-callback.h"

#include <map>
#include <vector>

namespace ns3
{
namespace tavrn
{

/**
 * @ingroup tavrn
 * @brief A single entry in the Global Topology Table.
 *
 * Each GttEntry represents one known node in the mesh network.
 * Entries track freshness via a dual-TTL system: soft expiry triggers
 * opportunistic refresh requests, while hard expiry triggers explicit
 * verification (E_RREQ).  See TAVRN v2 spec, "Steady State (Dual-TTL Refresh)".
 */
struct GttEntry
{
    Ipv4Address nodeAddr;  ///< IPv4 address of the known node
    Time lastSeen;         ///< Absolute simulation time when last evidence of this node was received
    Time ttlExpiry;        ///< Absolute simulation time when this entry fully (hard) expires
    Time softExpiry;       ///< Absolute simulation time when soft-expiry triggers a freshness request
    uint32_t seqNo;        ///< Latest known sequence number for this node
    uint16_t hopCount;     ///< Estimated hop distance (learned from routing metadata)
    bool departed;         ///< True if the node has been confirmed departed after hard expiry + no response

    /**
     * @brief Per-node effective TTL duration (Tier 2 of adaptive TTL).
     *
     * Each entry tracks its own TTL independently. On remote topology disruption
     * (TC-UPDATE NODE_LEAVE for a non-neighbor), this snaps to TTL_min for
     * surgical fast-checking of just that node without affecting the global TTL.
     * Grows back toward the current global TTL via EMA after re-stabilization.
     * Always <= the current global TTL (ceiling).
     *
     * A value of Time(0) means "use the table's default TTL" (backward compat).
     */
    Time perNodeTtl;

    /**
     * @brief Default constructor — creates an empty/invalid entry.
     */
    GttEntry()
        : nodeAddr(Ipv4Address()),
          lastSeen(Time(0)),
          ttlExpiry(Time(0)),
          softExpiry(Time(0)),
          seqNo(0),
          hopCount(0),
          departed(false),
          perNodeTtl(Time(0))
    {
    }

    /**
     * @brief Construct a fully specified GTT entry.
     *
     * @param addr   IPv4 address of the node
     * @param seen   Absolute simulation time of last evidence
     * @param ttl    Absolute simulation time of hard expiry
     * @param soft   Absolute simulation time of soft expiry
     * @param seq    Sequence number
     * @param hops   Hop distance
     */
    GttEntry(Ipv4Address addr, Time seen, Time ttl, Time soft, uint32_t seq, uint16_t hops)
        : nodeAddr(addr),
          lastSeen(seen),
          ttlExpiry(ttl),
          softExpiry(soft),
          seqNo(seq),
          hopCount(hops),
          departed(false),
          perNodeTtl(Time(0))
    {
    }
};

/**
 * @ingroup tavrn
 * @brief The Global Topology Table — TAVRN's core topology-awareness data structure.
 *
 * The GTT stores one entry per known node in the mesh.  It is NOT an ns3::Object;
 * it is owned (by composition) by the TAVRN RoutingProtocol, following the same
 * pattern as AODV's RoutingTable.
 *
 * Memory complexity is O(N) per node where N is the network size.
 * This is the fundamental trade-off of TAVRN: spend local memory/CPU to
 * conserve radio transmissions.
 *
 * The GTT implements the dual-TTL refresh mechanism described in the TAVRN v2 spec:
 *
 *  - **Soft expiry**: entry.softExpiry has passed but entry.ttlExpiry has not.
 *    The owning RoutingProtocol should piggyback a freshness request on the
 *    next outgoing transmission.
 *
 *  - **Hard expiry**: entry.ttlExpiry has passed.  The owning RoutingProtocol
 *    should send a targeted E_RREQ to verify the node still exists.
 *
 * The GTT itself is passive — it does not schedule timers or send packets.
 * All scheduling and transmission decisions are made by the RoutingProtocol
 * that owns it.  This keeps the GTT testable and deterministic.
 *
 * @see TAVRN v2 spec sections: "GTT Interface (For Applications)",
 *      "Steady State (Dual-TTL Refresh)", "Tunability".
 */
class GlobalTopologyTable
{
  public:
    /**
     * @brief Construct a GlobalTopologyTable with the given default TTL and soft-expiry ratio.
     *
     * @param defaultTtl           Duration from refresh to hard expiry.
     *                             Corresponds to the GTT_TTL tunable (default 120 s).
     * @param softExpiryThreshold  Fraction of defaultTtl at which soft expiry fires.
     *                             Must be in [0.0, 1.0].  Default 0.5 means soft expiry
     *                             occurs halfway through the TTL window.
     */
    GlobalTopologyTable(Time defaultTtl, double softExpiryThreshold);

    // -----------------------------------------------------------------------
    //  Core operations
    // -----------------------------------------------------------------------

    /**
     * @brief Add a new entry or update an existing one.
     *
     * If the entry exists and the new sequence number is >= the existing one,
     * the entry's lastSeen, ttlExpiry, softExpiry, seqNo, and hopCount are
     * updated.  The departed flag is cleared on update.
     *
     * If the entry does not exist, a new entry is created, the m_nodeJoinTrace
     * and m_sizeChangeTrace callbacks are fired.
     *
     * Soft-expiry is set to Now() + (defaultTtl * softExpiryThreshold).
     * Hard-expiry (ttlExpiry) is set to Now() + defaultTtl.
     *
     * @param addr     IPv4 address of the node
     * @param seqNo    Latest sequence number for this node
     * @param hopCount Estimated hop distance
     * @return true if the entry was created or updated; false if the update was
     *         rejected (existing entry has a strictly higher sequence number)
     */
    bool AddOrUpdateEntry(Ipv4Address addr, uint32_t seqNo, uint16_t hopCount);

    /**
     * @brief Remove an entry from the table.
     *
     * Fires m_nodeLeaveTrace and m_sizeChangeTrace if the entry existed.
     *
     * @param addr IPv4 address of the node to remove
     * @return true if the entry existed and was removed
     */
    bool RemoveEntry(Ipv4Address addr);

    /**
     * @brief Look up an entry by address.
     *
     * @param addr  IPv4 address to look up
     * @param entry [out] Populated with the entry data if found
     * @return true if the entry exists (including departed entries)
     */
    bool LookupEntry(Ipv4Address addr, GttEntry& entry) const;

    // -----------------------------------------------------------------------
    //  Spec API — maps to TAVRN v2 "GTT Interface (For Applications)"
    // -----------------------------------------------------------------------

    /**
     * @brief Check whether a node is known and not departed.
     *
     * @param addr IPv4 address of the node
     * @return true if the node exists in the GTT and is not marked departed
     */
    bool NodeExists(Ipv4Address addr) const;

    /**
     * @brief Get the time a node was last seen (heard from).
     *
     * @param addr IPv4 address of the node
     * @return Absolute simulation time of last evidence, or Time(0) if unknown
     */
    Time LastSeen(Ipv4Address addr) const;

    /**
     * @brief Get the remaining time before hard expiry for a node.
     *
     * @param addr IPv4 address of the node
     * @return Remaining time (may be negative if expired), or Time(0) if unknown
     */
    Time TtlRemaining(Ipv4Address addr) const;

    /**
     * @brief Enumerate all non-departed nodes currently in the GTT.
     *
     * @return Vector of IPv4 addresses of all active (non-departed) nodes
     */
    std::vector<Ipv4Address> EnumerateNodes() const;

    /**
     * @brief Get the count of non-departed nodes.
     *
     * @return Number of active (non-departed) entries
     */
    uint32_t NodeCount() const;

    // -----------------------------------------------------------------------
    //  Expiry management — used by RoutingProtocol to drive the dual-TTL FSM
    // -----------------------------------------------------------------------

    /**
     * @brief Get entries that have reached soft expiry but not hard expiry.
     *
     * These entries need a piggybacked freshness request on the next outgoing
     * transmission.  Only non-departed entries are returned.
     *
     * Condition: Now() >= softExpiry AND Now() < ttlExpiry AND !departed
     *
     * @return Vector of soft-expired entries
     */
    std::vector<GttEntry> GetSoftExpiredEntries() const;

    /**
     * @brief Get entries that have reached hard expiry.
     *
     * These entries need explicit verification via targeted E_RREQ.
     * Only non-departed entries are returned.
     *
     * Condition: Now() >= ttlExpiry AND !departed
     *
     * @return Vector of hard-expired entries
     */
    std::vector<GttEntry> GetHardExpiredEntries() const;

    /**
     * @brief Refresh an entry, resetting its TTL to full duration.
     *
     * Called when a freshness response or direct evidence is received.
     * Uses the entry's perNodeTtl if set (> 0), otherwise uses the table's
     * defaultTtl. Resets ttlExpiry and softExpiry accordingly.
     * Updates lastSeen = Now() and the sequence number.
     *
     * @param addr  IPv4 address of the node to refresh
     * @param seqNo Updated sequence number
     */
    void RefreshEntry(Ipv4Address addr, uint32_t seqNo);

    /**
     * @brief Set the per-node TTL override for a specific entry (Tier 2 adaptive TTL).
     *
     * When set to a value > 0, this entry uses perNodeTtl instead of the table's
     * defaultTtl for its next refresh cycle. The per-node TTL is always clamped
     * to be <= the table's current defaultTtl.
     *
     * Also immediately recalculates this entry's softExpiry and ttlExpiry based
     * on the new perNodeTtl, using lastSeen as the reference time.
     *
     * @param addr       IPv4 address of the node
     * @param perNodeTtl Per-node TTL duration; Time(0) reverts to table default
     */
    void SetPerNodeTtl(Ipv4Address addr, Time perNodeTtl);

    /**
     * @brief Grow a per-node TTL toward the table's current defaultTtl via EMA.
     *
     * perNodeTtl_new = alpha * perNodeTtl_old + (1 - alpha) * defaultTtl
     * If the result is within 5% of defaultTtl, snaps to Time(0) (use default).
     *
     * @param addr  IPv4 address of the node
     * @param alpha EMA smoothing factor in (0, 1). Higher = slower growth.
     */
    void GrowPerNodeTtl(Ipv4Address addr, double alpha);

    /**
     * @brief Clamp all per-node TTLs to be <= the given ceiling.
     *
     * Called when the global TTL resets to TTL_min. Any entry with a
     * perNodeTtl > ceiling gets its perNodeTtl set to ceiling and its
     * expiry times recalculated.
     *
     * @param ceiling Maximum allowed per-node TTL
     */
    void ClampAllPerNodeTtls(Time ceiling);

    /**
     * @brief Mark a node as departed.
     *
     * Called after hard expiry verification fails (no response to E_RREQ).
     * Fires m_nodeLeaveTrace.  The entry remains in the table for cleanup
     * by Purge() after 2 * defaultTtl.
     *
     * @param addr IPv4 address of the departed node
     * @return true if the node was newly marked as departed (state changed),
     *         false if it was already departed or not found in the GTT
     */
    bool MarkDeparted(Ipv4Address addr);

    // -----------------------------------------------------------------------
    //  Bulk operations — used during mentorship SYNC_DATA import
    // -----------------------------------------------------------------------

    /**
     * @brief Get a page of entries for paginated sync transfer.
     *
     * Entries are returned in the iteration order of the underlying std::map
     * (i.e., sorted by Ipv4Address).  Both active and departed entries are
     * included to give the mentee a complete picture.
     *
     * @param startIndex 0-based index into the ordered entry set
     * @param count      Maximum number of entries to return
     * @return Vector of entries (may be shorter than count if near the end)
     */
    std::vector<GttEntry> GetEntriesPage(uint32_t startIndex, uint32_t count) const;

    /**
     * @brief Get the total number of entries (including departed).
     *
     * @return Total entry count (active + departed)
     */
    uint32_t GetTotalEntries() const;

    /**
     * @brief Merge a single entry received during SYNC_DATA import.
     *
     * If the entry already exists locally with a higher sequence number, the
     * merge is skipped.  Otherwise the entry is inserted or updated.  The
     * imported timestamps (lastSeen, ttlExpiry, softExpiry) are preserved
     * from the mentor's data rather than being recalculated, since the mentor
     * has fresher temporal context.
     *
     * @param entry The entry to merge from the mentor's GTT
     */
    void MergeEntry(const GttEntry& entry);

    // -----------------------------------------------------------------------
    //  Configuration — runtime tuning of GTT parameters
    // -----------------------------------------------------------------------

    /**
     * @brief Set the default TTL for new/refreshed entries.
     *
     * Corresponds to the GTT_TTL tunable in the TAVRN v2 spec.
     *
     * @param ttl New default TTL duration
     */
    void SetDefaultTtl(Time ttl);

    /**
     * @brief Get the current default TTL.
     *
     * @return Default TTL duration
     */
    Time GetDefaultTtl() const;

    /**
     * @brief Set the soft-expiry threshold ratio.
     *
     * @param ratio Value in [0.0, 1.0].  Soft expiry fires at
     *              Now() + (defaultTtl * ratio) after a refresh.
     */
    void SetSoftExpiryThreshold(double ratio);

    /**
     * @brief Get the current soft-expiry threshold ratio.
     *
     * @return Soft-expiry threshold in [0.0, 1.0]
     */
    double GetSoftExpiryThreshold() const;

    // -----------------------------------------------------------------------
    //  Debug / diagnostics
    // -----------------------------------------------------------------------

    /**
     * @brief Print the GTT contents to an output stream.
     *
     * Produces a human-readable table suitable for ns-3 trace file analysis.
     *
     * @param stream Output stream wrapper
     */
    void Print(Ptr<OutputStreamWrapper> stream) const;

    /**
     * @brief Purge fully expired and departed entries.
     *
     * Removes entries that are marked departed AND have been departed for
     * longer than 2 * defaultTtl.  This prevents unbounded memory growth
     * while giving the RoutingProtocol time to propagate TC-UPDATE messages.
     */
    void Purge();

    // -----------------------------------------------------------------------
    //  Traced callbacks for metrics collection
    // -----------------------------------------------------------------------

    /// Fired when a new node is added to the GTT (first time seen).
    TracedCallback<Ipv4Address> m_nodeJoinTrace;

    /// Fired when a node is removed or marked as departed.
    TracedCallback<Ipv4Address> m_nodeLeaveTrace;

    /// Fired when the active (non-departed) entry count changes.
    TracedCallback<uint32_t> m_sizeChangeTrace;

  private:
    /// The topology table: maps node address to entry.
    std::map<Ipv4Address, GttEntry> m_entries;

    /// Default time-to-live for new/refreshed entries (GTT_TTL tunable).
    Time m_defaultTtl;

    /**
     * @brief Fraction of m_defaultTtl at which soft expiry triggers.
     *
     * Value in [0.0, 1.0].  Default is 0.5 (balanced profile).
     * - 0.25 = near-AODV (late soft expiry, maximum silence)
     * - 0.50 = balanced
     * - 0.75 = near-OLSR (early soft expiry, faster convergence)
     */
    double m_softExpiryThreshold;
};

} // namespace tavrn
} // namespace ns3

#endif /* TAVRN_GTT_H */
