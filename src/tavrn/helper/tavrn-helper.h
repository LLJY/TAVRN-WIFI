/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN: Topology Aware Vicinity-Reactive Network
 * Helper class that adds TAVRN routing to nodes.
 *
 * Design reference: AODV helper (ns-3) for structural patterns.
 */

#ifndef TAVRN_HELPER_H
#define TAVRN_HELPER_H

#include "ns3/ipv4-routing-helper.h"
#include "ns3/node-container.h"
#include "ns3/node.h"
#include "ns3/object-factory.h"

namespace ns3
{
/**
 * @ingroup tavrn
 * @brief Helper class that adds TAVRN routing to nodes.
 */
class TavrnHelper : public Ipv4RoutingHelper
{
  public:
    TavrnHelper();

    /**
     * @returns pointer to clone of this TavrnHelper
     *
     * @internal
     * This method is mainly for internal use by the other helpers;
     * clients are expected to free the dynamic memory allocated by this method
     */
    TavrnHelper* Copy() const override;

    /**
     * @param node the node on which the routing protocol will run
     * @returns a newly-created routing protocol
     *
     * This method will be called by ns3::InternetStackHelper::Install
     */
    Ptr<Ipv4RoutingProtocol> Create(Ptr<Node> node) const override;

    /**
     * @param name the name of the attribute to set
     * @param value the value of the attribute to set.
     *
     * This method controls the attributes of ns3::tavrn::RoutingProtocol
     */
    void Set(std::string name, const AttributeValue& value);

    /**
     * Assign a fixed random variable stream number to the random variables
     * used by this model.  Return the number of streams (possibly zero) that
     * have been assigned.  The Install() method of the InternetStackHelper
     * should have previously been called by the user.
     *
     * @param stream first stream index to use
     * @param c NodeContainer of the set of nodes for which TAVRN
     *          should be modified to use a fixed stream
     * @return the number of stream indices assigned by this helper
     */
    int64_t AssignStreams(NodeContainer c, int64_t stream);

  private:
    /** the factory to create TAVRN routing object */
    ObjectFactory m_agentFactory;
};

} // namespace ns3

#endif /* TAVRN_HELPER_H */
