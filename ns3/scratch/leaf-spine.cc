// Assignment 3, Option 2: leaf-spine topology.
//
// 2 spine switches, 2 ToR (leaf) switches, 16 servers per ToR (32 servers).
// Server <-> ToR  : 100 Gbps, 500 ns propagation delay each direction
// ToR    <-> Spine: 400 Gbps, 500 ns propagation delay each direction
// All links are bidirectional (point-to-point links in ns-3 are full duplex).
//
// Workload: every server opens one BulkSend flow of 64 MB to each of the other
// 31 servers (32 x 31 = 992 directed flows), all starting at the same instant.
// Each flow's completion time is measured from the application start time until
// its receiver has taken delivery of all 64 MB of application bytes.
//
// Build: copy this file into <ns-3>/scratch/ and run
//   python3.12 ./ns3 run "leaf-spine --verbose=1"
// (ns-3.45's ./ns3 script is incompatible with Python 3.14's argparse.)
// Use an optimized build (./ns3 configure --build-profile=optimized): the full
// run moves 992 x 64 MB = 63.5 GB of payload, tens of millions of packets.

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ppp-header.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace ns3;

namespace
{

constexpr uint32_t kNumSpines = 2;
constexpr uint32_t kNumTors = 2;
constexpr uint32_t kServersPerTor = 16;
constexpr uint32_t kNumServers = kNumTors * kServersPerTor;

const char* kServerLinkRate = "100Gbps";
const char* kFabricLinkRate = "400Gbps";
const char* kLinkDelay = "500ns";

/// 64 MB of application payload per flow, as specified.
constexpr uint64_t kFlowBytes = 64 * 1000 * 1000;
/// All flows start together, after the stack has settled.
const Time kFlowStart = Seconds(1.0);
/// Receiver port for a flow is kBasePort + <source server index>, so every
/// (destination address, port) pair identifies exactly one of the 992 flows.
constexpr uint16_t kBasePort = 10000;

/// Index of the ToR that `server` (index into LeafSpineTopology::servers) hangs off.
uint32_t
TorOf(uint32_t server)
{
    return server / kServersPerTor;
}

/// The port a flow from `src` lands on at its receiver.
uint16_t
PortFor(uint32_t src)
{
    return kBasePort + static_cast<uint16_t>(src);
}

/// One installed point-to-point link.
struct Link
{
    std::string name;           //!< e.g. "tor0-server3" or "tor1-spine0"
    NetDeviceContainer devices; //!< devices[0] is the switch end of the pair
    Ipv4InterfaceContainer interfaces;
};

/// Installs one point-to-point link and gives it its own /30. The switch side
/// of each pair (node `a`) takes .1.
Link
MakeLink(Ptr<Node> a,
         Ptr<Node> b,
         PointToPointHelper& helper,
         std::string name,
         Ipv4AddressHelper& address,
         const std::string& subnet)
{
    Link link;
    link.name = std::move(name);
    link.devices = helper.Install(a, b);
    address.SetBase(subnet.c_str(), "255.255.255.252");
    link.interfaces = address.Assign(link.devices);
    return link;
}

} // namespace

/// One directed flow and the state needed to time it. Everything derivable is
/// derived: the start time is always kFlowStart, the port is PortFor(src), the
/// destination address is the topology's address for `dst`, bytes received are
/// the sink's own counter, and "completed" means `finish` was stamped.
struct FlowRecord
{
    uint32_t id;
    uint32_t src;
    uint32_t dst;
    Ptr<PacketSink> sink;
    Time finish = Time::Max(); //!< set when the last application byte arrives

    bool Completed() const { return finish != Time::Max(); }
    uint64_t BytesRx() const { return sink->GetTotalRx(); }
};

/// Holds the whole fabric: nodes, links, and the server addresses flows use.
struct LeafSpineTopology
{
    NodeContainer spines;
    NodeContainer tors;
    NodeContainer servers;                    //!< index = torIndex * 16 + serverIndex
    std::vector<Link> serverLinks;            //!< 32 links, one per server
    std::vector<Link> fabricLinks;            //!< 4 links, every ToR to every spine
    std::vector<Ipv4Address> serverAddresses; //!< parallel to `servers`
};

/// Builds the topology of the assignment figure.
static LeafSpineTopology
BuildLeafSpine()
{
    LeafSpineTopology topo;
    topo.spines.Create(kNumSpines);
    topo.tors.Create(kNumTors);
    topo.servers.Create(kNumServers);

    // Switches and servers are both modelled as plain ns-3 nodes running the
    // IP stack; the switches forward with global (ECMP) routing.
    InternetStackHelper stack;
    stack.Install(topo.spines);
    stack.Install(topo.tors);
    stack.Install(topo.servers);

    PointToPointHelper serverLink;
    serverLink.SetDeviceAttribute("DataRate", StringValue(kServerLinkRate));
    serverLink.SetChannelAttribute("Delay", StringValue(kLinkDelay));

    PointToPointHelper fabricLink;
    fabricLink.SetDeviceAttribute("DataRate", StringValue(kFabricLinkRate));
    fabricLink.SetChannelAttribute("Delay", StringValue(kLinkDelay));

    // Server links are 10.1.<server>.0/30, fabric links 10.2.<uplink>.0/30.
    Ipv4AddressHelper address;
    topo.serverAddresses.resize(kNumServers);

    for (uint32_t server = 0; server < kNumServers; ++server)
    {
        const uint32_t tor = TorOf(server);
        topo.serverLinks.push_back(MakeLink(topo.tors.Get(tor),
                                           topo.servers.Get(server),
                                           serverLink,
                                           "tor" + std::to_string(tor) + "-server" +
                                               std::to_string(server),
                                           address,
                                           "10.1." + std::to_string(server) + ".0"));
        topo.serverAddresses[server] = topo.serverLinks.back().interfaces.GetAddress(1);
    }

    for (uint32_t tor = 0; tor < kNumTors; ++tor)
    {
        for (uint32_t spine = 0; spine < kNumSpines; ++spine)
        {
            const uint32_t uplink = tor * kNumSpines + spine;
            topo.fabricLinks.push_back(MakeLink(topo.tors.Get(tor),
                                                topo.spines.Get(spine),
                                                fabricLink,
                                                "tor" + std::to_string(tor) + "-spine" +
                                                    std::to_string(spine),
                                                address,
                                                "10.2." + std::to_string(uplink) + ".0"));
        }
    }

    // Inter-rack traffic has two equal-cost paths (one per spine); without
    // ECMP global routing would pin every flow to a single spine.
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    return topo;
}

/// Prints the installed links so the topology can be checked against the figure.
static void
PrintTopology(const LeafSpineTopology& topo)
{
    std::cout << "Nodes: " << kNumSpines << " spines, " << kNumTors << " ToRs, " << kNumServers
              << " servers (" << kServersPerTor << " per ToR)\n";
    std::cout << "Links: " << topo.serverLinks.size() << " server links @ " << kServerLinkRate
              << ", " << topo.fabricLinks.size() << " fabric links @ " << kFabricLinkRate << ", "
              << kLinkDelay << " propagation delay each direction\n";

    for (const Link& link : topo.fabricLinks)
    {
        std::cout << "  " << link.name << ": " << link.interfaces.GetAddress(0) << " <-> "
                  << link.interfaces.GetAddress(1) << " @ " << kFabricLinkRate << "\n";
    }
    for (const Link& link : topo.serverLinks)
    {
        std::cout << "  " << link.name << ": " << link.interfaces.GetAddress(0) << " <-> "
                  << link.interfaces.GetAddress(1) << " @ " << kServerLinkRate << "\n";
    }
}

/// Bytes and distinct TCP flows observed on one direction of a fabric link.
struct FabricCounter
{
    std::string name;
    uint64_t bytes = 0;
    uint64_t packets = 0;
    std::set<std::tuple<uint32_t, uint32_t, uint16_t, uint16_t>> tuples; //!< 4-tuples seen
};

/// Records a packet leaving a ToR-spine device. Peeks the IPv4/TCP headers so
/// the same flow can be looked for on the *other* spine, which is what
/// distinguishes per-flow path selection from per-packet spraying.
static void
FabricTxCallback(FabricCounter* counter, Ptr<const Packet> packet)
{
    counter->bytes += packet->GetSize();
    ++counter->packets;

    // PointToPointNetDevice fires MacTx *after* AddHeader, so the PPP header is
    // already on the packet and has to come off before the IPv4 header.
    Ptr<Packet> copy = packet->Copy();
    PppHeader ppp;
    Ipv4Header ip;
    TcpHeader tcp;
    if (copy->RemoveHeader(ppp) == 0 || copy->RemoveHeader(ip) == 0 || ip.GetProtocol() != 6 ||
        copy->RemoveHeader(tcp) == 0)
    {
        return;
    }
    counter->tuples.emplace(ip.GetSource().Get(),
                            ip.GetDestination().Get(),
                            tcp.GetSourcePort(),
                            tcp.GetDestinationPort());
}

/// Stamps the flow's finish time once its sink has taken delivery of all 64 MB.
/// The sink keeps the byte count itself, so nothing is accumulated here.
static void
RxCallback(FlowRecord* flow, Ptr<const Packet>, const Address&)
{
    if (!flow->Completed() && flow->BytesRx() >= kFlowBytes)
    {
        flow->finish = Simulator::Now();
    }
}

/// Installs the 32 x 31 all-to-all flows. Each receiver runs one PacketSink per
/// source, so a flow is identified by its destination address and port.
static std::vector<std::unique_ptr<FlowRecord>>
SetupFlows(const LeafSpineTopology& topo)
{
    std::vector<std::unique_ptr<FlowRecord>> flows;
    flows.reserve(kNumServers * (kNumServers - 1));

    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory", Address());
    BulkSendHelper sourceHelper("ns3::TcpSocketFactory", Address());
    sourceHelper.SetAttribute("MaxBytes", UintegerValue(kFlowBytes));
    sourceHelper.SetAttribute("SendSize", UintegerValue(1448));

    for (uint32_t src = 0; src < kNumServers; ++src)
    {
        for (uint32_t dst = 0; dst < kNumServers; ++dst)
        {
            if (src == dst)
            {
                continue; // no flow from a server to itself
            }

            auto flow = std::make_unique<FlowRecord>();
            flow->id = static_cast<uint32_t>(flows.size());
            flow->src = src;
            flow->dst = dst;

            const uint16_t port = PortFor(src);
            sinkHelper.SetAttribute("Local",
                                    AddressValue(InetSocketAddress(Ipv4Address::GetAny(), port)));
            ApplicationContainer sinkApp = sinkHelper.Install(topo.servers.Get(dst));
            sinkApp.Start(Seconds(0.0));
            flow->sink = DynamicCast<PacketSink>(sinkApp.Get(0));
            flow->sink->TraceConnectWithoutContext("Rx",
                                                   MakeBoundCallback(&RxCallback, flow.get()));

            sourceHelper.SetAttribute(
                "Remote",
                AddressValue(InetSocketAddress(topo.serverAddresses[dst], port)));
            // Every flow starts at the same instant, as specified.
            sourceHelper.Install(topo.servers.Get(src)).Start(kFlowStart);

            flows.push_back(std::move(flow));
        }
    }

    return flows;
}

/// Writes one row per flow and reports anything still running at the end of the
/// simulation. Returns the number of unfinished flows.
static uint32_t
ReportFlows(const std::vector<std::unique_ptr<FlowRecord>>& flows,
            const LeafSpineTopology& topo,
            const std::string& csvPath)
{
    std::ofstream csv(csvPath);
    csv << "flow_id,src_server,dst_server,src_tor,dst_tor,dst_address,port,"
           "bytes_rx,start_s,finish_s,fct_s,completed\n";
    csv << std::fixed << std::setprecision(9);

    uint32_t unfinished = 0;
    double fctSum = 0.0;
    for (const auto& flow : flows)
    {
        csv << flow->id << ',' << flow->src << ',' << flow->dst << ',' << TorOf(flow->src) << ','
            << TorOf(flow->dst) << ',' << topo.serverAddresses[flow->dst] << ','
            << PortFor(flow->src) << ',' << flow->BytesRx() << ',' << kFlowStart.GetSeconds()
            << ',';
        if (flow->Completed())
        {
            const double fct = (flow->finish - kFlowStart).GetSeconds();
            fctSum += fct;
            csv << flow->finish.GetSeconds() << ',' << fct << ",1\n";
        }
        else
        {
            ++unfinished;
            csv << ",,0\n"; // no finish time: the flow never delivered all 64 MB
        }
    }
    csv.close();

    const uint32_t completed = flows.size() - unfinished;
    std::cout << "Flows: " << flows.size() << " launched, " << completed << " completed, "
              << unfinished << " unfinished\n";
    if (completed > 0)
    {
        std::cout << "Mean FCT of completed flows: " << (fctSum / completed) << " s\n";
    }
    if (unfinished > 0)
    {
        std::cout << "WARNING: " << unfinished
                  << " flow(s) did not deliver all 64 MB before the simulation ended; "
                     "raise --stop. Unfinished flows (src->dst, bytes delivered):\n";
        for (const auto& flow : flows)
        {
            if (!flow->Completed())
            {
                std::cout << "  flow " << flow->id << ": server " << flow->src << " -> server "
                          << flow->dst << ", " << flow->BytesRx() << " / " << kFlowBytes
                          << " bytes\n";
            }
        }
    }
    std::cout << "Per-flow completion times written to " << csvPath << "\n";
    return unfinished;
}

/// Attaches a byte/flow counter to the ToR side of every ToR-spine link.
static std::vector<std::unique_ptr<FabricCounter>>
InstrumentFabric(const LeafSpineTopology& topo)
{
    std::vector<std::unique_ptr<FabricCounter>> counters;
    for (const Link& link : topo.fabricLinks)
    {
        auto counter = std::make_unique<FabricCounter>();
        counter->name = link.name;
        // devices[0] is the ToR end, so this counts ToR -> spine traffic.
        link.devices.Get(0)->TraceConnectWithoutContext(
            "MacTx",
            MakeBoundCallback(&FabricTxCallback, counter.get()));
        counters.push_back(std::move(counter));
    }
    return counters;
}

/// Reports the load split across the two spines and whether any single flow was
/// seen on both of them.
static void
ReportFabric(const std::vector<std::unique_ptr<FabricCounter>>& counters)
{
    std::cout << "\nFabric load (ToR -> spine direction):\n";
    for (const auto& c : counters)
    {
        std::cout << "  " << c->name << ": " << c->bytes << " bytes, " << c->packets
                  << " packets, " << c->tuples.size() << " distinct TCP flows\n";
    }

    // For each ToR, compare its two spine uplinks.
    for (uint32_t tor = 0; tor < kNumTors; ++tor)
    {
        const FabricCounter* a = counters[tor * kNumSpines + 0].get();
        const FabricCounter* b = counters[tor * kNumSpines + 1].get();

        std::vector<std::tuple<uint32_t, uint32_t, uint16_t, uint16_t>> shared;
        std::set_intersection(a->tuples.begin(),
                              a->tuples.end(),
                              b->tuples.begin(),
                              b->tuples.end(),
                              std::back_inserter(shared));

        const uint64_t total = a->bytes + b->bytes;
        std::cout << "  ToR " << tor << ": spine0 " << (total ? 100.0 * a->bytes / total : 0.0)
                  << "% / spine1 " << (total ? 100.0 * b->bytes / total : 0.0) << "% of bytes; "
                  << shared.size() << " flow(s) seen on BOTH spines";
        if (!shared.empty())
        {
            std::cout << " -> PER-PACKET spraying (TCP will see reordering)";
        }
        else if (total > 0)
        {
            std::cout << " -> per-flow path selection";
        }
        std::cout << "\n";
    }
}

int
main(int argc, char* argv[])
{
    bool verbose = false;
    bool traceFabric = false;
    double stopSeconds = 30.0;
    std::string csvPath = "fct.csv";

    CommandLine cmd(__FILE__);
    cmd.AddValue("verbose", "Print every link in the topology", verbose);
    cmd.AddValue("stop", "Simulation stop time in seconds", stopSeconds);
    cmd.AddValue("csv", "Path for the per-flow completion time CSV", csvPath);
    cmd.AddValue("trace-fabric",
                 "Count bytes and flows on each ToR-spine link (adds per-packet overhead)",
                 traceFabric);
    cmd.Parse(argc, argv);

    // Spread flows over both spines instead of always picking the first route.
    Config::SetDefault("ns3::Ipv4GlobalRouting::RandomEcmpRouting", BooleanValue(true));

    // A 100 Gbps NIC over a 6 us fabric RTT has a bandwidth-delay product of
    // roughly 75 kB, so the default 128 kB socket buffers would cap a flow well
    // below line rate once queueing is added. 2 MB is ~16x the BDP, leaving the
    // congestion control algorithm rather than the window as the limit, while
    // keeping total buffer memory across 992 concurrent sockets near 2 GB --
    // BulkSendApplication fills every send buffer it is given, so a larger
    // value here is paid for 992 times over.
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(2 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(2 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));

    LeafSpineTopology topo = BuildLeafSpine();
    if (verbose)
    {
        PrintTopology(topo);
    }

    std::vector<std::unique_ptr<FabricCounter>> fabric;
    if (traceFabric)
    {
        fabric = InstrumentFabric(topo);
    }

    std::vector<std::unique_ptr<FlowRecord>> flows = SetupFlows(topo);
    std::cout << "Launched " << flows.size() << " flows of " << kFlowBytes
              << " bytes each, all starting at " << kFlowStart.GetSeconds() << " s\n";

    Simulator::Stop(kFlowStart + Seconds(stopSeconds));
    Simulator::Run();

    const uint32_t unfinished = ReportFlows(flows, topo, csvPath);
    if (traceFabric)
    {
        ReportFabric(fabric);
    }

    Simulator::Destroy();
    return unfinished == 0 ? 0 : 1;
}
