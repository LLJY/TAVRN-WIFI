/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN: Topology Aware Vicinity-Reactive Network
 * Minimal example: 5 nodes in ad-hoc WiFi with TAVRN routing and UDP echo.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/tavrn-helper.h"
#include "ns3/wifi-module.h"

#include <iostream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TavrnExample");

int
main(int argc, char* argv[])
{
    uint32_t nNodes = 5;
    double totalTime = 100.0;
    double step = 100.0; // meters between nodes
    bool pcap = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("nNodes", "Number of nodes", nNodes);
    cmd.AddValue("time", "Simulation time (s)", totalTime);
    cmd.AddValue("step", "Distance between nodes (m)", step);
    cmd.AddValue("pcap", "Enable PCAP traces", pcap);
    cmd.Parse(argc, argv);

    // Create nodes
    NodeContainer nodes;
    nodes.Create(nNodes);

    // Set up WiFi in ad-hoc mode
    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::AdhocWifiMac");
    YansWifiPhyHelper wifiPhy;
    YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default();
    wifiPhy.SetChannel(wifiChannel.Create());
    WifiHelper wifi;
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue("OfdmRate6Mbps"),
                                 "RtsCtsThreshold",
                                 UintegerValue(0));
    NetDeviceContainer devices = wifi.Install(wifiPhy, wifiMac, nodes);

    if (pcap)
    {
        wifiPhy.EnablePcapAll("tavrn");
    }

    // Position nodes in a line
    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::GridPositionAllocator",
                                  "MinX",
                                  DoubleValue(0.0),
                                  "MinY",
                                  DoubleValue(0.0),
                                  "DeltaX",
                                  DoubleValue(step),
                                  "DeltaY",
                                  DoubleValue(0.0),
                                  "GridWidth",
                                  UintegerValue(nNodes),
                                  "LayoutType",
                                  StringValue("RowFirst"));
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    // Install TAVRN routing and Internet stack
    TavrnHelper tavrn;
    InternetStackHelper stack;
    stack.SetRoutingHelper(tavrn);
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // Print routing tables at t=8s
    Ptr<OutputStreamWrapper> routingStream =
        Create<OutputStreamWrapper>("tavrn.routes", std::ios::out);
    Ipv4RoutingHelper::PrintRoutingTableAllAt(Seconds(8), routingStream);

    // Install UDP echo server on last node
    UdpEchoServerHelper echoServer(9);
    ApplicationContainer serverApps = echoServer.Install(nodes.Get(nNodes - 1));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(totalTime - 1.0));

    // Install UDP echo client on first node
    UdpEchoClientHelper echoClient(interfaces.GetAddress(nNodes - 1), 9);
    echoClient.SetAttribute("MaxPackets", UintegerValue(100));
    echoClient.SetAttribute("Interval", TimeValue(Seconds(1.0)));
    echoClient.SetAttribute("PacketSize", UintegerValue(512));

    ApplicationContainer clientApps = echoClient.Install(nodes.Get(0));
    clientApps.Start(Seconds(2.0));
    clientApps.Stop(Seconds(totalTime - 1.0));

    std::cout << "Starting TAVRN simulation with " << nNodes << " nodes for " << totalTime
              << " s..." << std::endl;

    Simulator::Stop(Seconds(totalTime));
    Simulator::Run();
    Simulator::Destroy();

    return 0;
}
