/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN: Topology Aware Vicinity-Reactive Network
 * Helper class implementation.
 */

#include "tavrn-helper.h"

#include "ns3/ipv4-list-routing.h"
#include "ns3/names.h"
#include "ns3/node-list.h"
#include "ns3/ptr.h"
#include "ns3/tavrn-routing-protocol.h"

namespace ns3
{

TavrnHelper::TavrnHelper()
    : Ipv4RoutingHelper()
{
    m_agentFactory.SetTypeId("ns3::tavrn::RoutingProtocol");
}

TavrnHelper*
TavrnHelper::Copy() const
{
    return new TavrnHelper(*this);
}

Ptr<Ipv4RoutingProtocol>
TavrnHelper::Create(Ptr<Node> node) const
{
    Ptr<tavrn::RoutingProtocol> agent = m_agentFactory.Create<tavrn::RoutingProtocol>();
    node->AggregateObject(agent);
    return agent;
}

void
TavrnHelper::Set(std::string name, const AttributeValue& value)
{
    m_agentFactory.Set(name, value);
}

int64_t
TavrnHelper::AssignStreams(NodeContainer c, int64_t stream)
{
    int64_t currentStream = stream;
    Ptr<Node> node;
    for (auto i = c.Begin(); i != c.End(); ++i)
    {
        node = (*i);
        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
        NS_ASSERT_MSG(ipv4, "Ipv4 not installed on node");
        Ptr<Ipv4RoutingProtocol> proto = ipv4->GetRoutingProtocol();
        NS_ASSERT_MSG(proto, "Ipv4 routing not installed on node");
        Ptr<tavrn::RoutingProtocol> tavrn = DynamicCast<tavrn::RoutingProtocol>(proto);
        if (tavrn)
        {
            currentStream += tavrn->AssignStreams(currentStream);
            continue;
        }
        // Tavrn may also be in a list
        Ptr<Ipv4ListRouting> list = DynamicCast<Ipv4ListRouting>(proto);
        if (list)
        {
            int16_t priority;
            Ptr<Ipv4RoutingProtocol> listProto;
            Ptr<tavrn::RoutingProtocol> listTavrn;
            for (uint32_t j = 0; j < list->GetNRoutingProtocols(); j++)
            {
                listProto = list->GetRoutingProtocol(j, priority);
                listTavrn = DynamicCast<tavrn::RoutingProtocol>(listProto);
                if (listTavrn)
                {
                    currentStream += listTavrn->AssignStreams(currentStream);
                    break;
                }
            }
        }
    }
    return (currentStream - stream);
}

} // namespace ns3
