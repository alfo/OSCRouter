// IfaceSupport.cpp
//
// Linux implementation of the network interface helper used by the
// AsyncSocketServ implementations. This mirrors the OSX version in
// ../OSX/IfaceSupport.cpp; only the three platform-specific pieces differ:
//
//   * Hardware address / interface index discovery. BSD reports these on an
//     AF_LINK ifaddrs entry carrying a sockaddr_dl; Linux uses AF_PACKET with a
//     sockaddr_ll instead.
//   * Gateway and default-route discovery. BSD walks the routing table via a
//     CTL_NET/NET_RT_DUMP sysctl; Linux exposes it as text in /proc/net/route.
//   * FindIfaceBySa's link-layer case, for the same AF_LINK/AF_PACKET reason.
//
// Two deliberate differences from the OSX version, both defensive:
//
//   * ifa_addr and ifa_netmask are documented to be NULL for some Linux
//     interfaces (the OSX code dereferences them unconditionally), so they are
//     null-checked here.
//   * A missing or unreadable routing table is not treated as fatal. Gateway
//     information is purely informational for this library, but CAsyncSocketServ
//     ::Startup aborts on any result other than SETUP_OK, and inside a container
//     the route table may legitimately be empty. Interfaces simply report
//     themselves as their own gateway in that case, matching what the OSX build
//     already does for every non-default interface.
//
//////////////////////////////////////////////////////////////////////

#include <vector>
#include <map>
#include <string>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "deftypes.h"
#include "ipaddr.h"
#include "AsyncSocketInterface.h"
#include "IfaceSupport.h"

namespace
{
//Link-layer facts about an interface, gathered from the AF_PACKET ifaddrs entries.
struct LinkInfo
{
	int ifindex = 0;
	bool hasmac = false;
	char mac[IAsyncSocketServ::NETINTID_MACLEN] = {0};
};

typedef std::map<std::string, LinkInfo> LINK_INFO_MAP;

//Flag values from <linux/route.h>, redefined here so this file does not have to
//pull a kernel header in alongside the userspace net headers.
const unsigned int kRouteFlagUp = 0x0001;
const unsigned int kRouteFlagGateway = 0x0002;
}  // namespace

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CIfaceSupport::CIfaceSupport()
{
	m_defaultiface = NETID_INVALID;
}

CIfaceSupport::~CIfaceSupport()
{
}

////////////////////////////////////////////////////////////////
// IAsyncSocketServ functions

//A quick util to do the casting, etc to fill in the address of a CIPAddr
void CIfaceSupport::SetAddress(CIPAddr& addr, struct sockaddr* psa)
{
	if(!psa)
		addr.SetV4Address(0);
	else if(AF_INET == psa->sa_family)
		addr.SetV4Address(ntohl(((struct sockaddr_in*)(psa))->sin_addr.s_addr));
	else if(AF_INET6 == psa->sa_family)
		addr.SetV6Address(((struct sockaddr_in6*)(psa))->sin6_addr.s6_addr);
	else
		addr.SetV4Address(0);
}

//The real setup functionality for a particular protocol family -- does the same thing but filters based on protocol
CIfaceSupport::SETUP_RESULT CIfaceSupport::SetUpProtIfaces(int protocol)
{
	//We only currently support ip and ipv6
	if((protocol != AF_INET) && (protocol != AF_INET6))
		return SETUP_BADFAMILY;

	struct ifaddrs* paddrs = NULL;
	if(0 != getifaddrs(&paddrs))
		return SETUP_BADIOCTL;

	//Unlike BSD, Linux makes no ordering guarantee between an interface's AF_PACKET
	//entry and its address entries, so collect the link-layer data in its own pass
	//rather than relying on having just walked past it.
	LINK_INFO_MAP linkinfo;
	for(struct ifaddrs* p = paddrs; p != NULL; p = p->ifa_next)
	{
		if(!p->ifa_addr || (AF_PACKET != p->ifa_addr->sa_family) || !p->ifa_name)
			continue;

		struct sockaddr_ll* link_addr = (struct sockaddr_ll*)(p->ifa_addr);

		LinkInfo info;
		info.ifindex = link_addr->sll_ifindex;
		if(IAsyncSocketServ::NETINTID_MACLEN == link_addr->sll_halen)
		{
			memcpy(info.mac, link_addr->sll_addr, IAsyncSocketServ::NETINTID_MACLEN);
			info.hasmac = true;
		}

		linkinfo[std::string(p->ifa_name)] = info;
	}

	//Now step through the list again, keeping the addresses for our protocol.
	for(struct ifaddrs* p = paddrs; p != NULL; p = p->ifa_next)
	{
		//Filter out loopback and down interfaces immediately
		if(!p->ifa_addr || !p->ifa_name)
			continue;
		if((p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP))
			continue;
		if(p->ifa_addr->sa_family != protocol)
			continue;

		IAsyncSocketServ::netintinfo info;
		memset(info.name, 0, sizeof(info.name));
		memset(info.desc, 0, sizeof(info.desc));
		memset(info.mac, 0, sizeof(info.mac));
		info.ifindex = 0;

		strncpy(info.name, p->ifa_name, IAsyncSocketServ::NETINTID_STRLEN - 1);
		strncpy(info.desc, p->ifa_name, IAsyncSocketServ::NETINTID_STRLEN - 1);

		LINK_INFO_MAP::const_iterator it_link = linkinfo.find(std::string(p->ifa_name));
		if(it_link != linkinfo.end())
		{
			info.ifindex = it_link->second.ifindex;
			if(it_link->second.hasmac)
				memcpy(info.mac, it_link->second.mac, IAsyncSocketServ::NETINTID_MACLEN);
		}
		else
		{
			//No AF_PACKET entry (can happen for some virtual interfaces) -- the index
			//is still needed for multicast joins, so ask for it by name.
			info.ifindex = static_cast<int>(if_nametoindex(p->ifa_name));
		}

		SetAddress(info.addr, p->ifa_addr);
		SetAddress(info.mask, p->ifa_netmask);

		info.id = m_ifaces.size();
		m_ifaces.push_back(info);
	}

	freeifaddrs(paddrs);

	//Now that the iface list is set, fill in gateways
	if(!FillInGateways(protocol))
		return SETUP_NOROUTETABLE;

	return SETUP_OK;
}

CIfaceSupport::SETUP_RESULT CIfaceSupport::SetUpIfaces()
{
	SETUP_RESULT repl = SetUpProtIfaces(AF_INET);
	//TODO: We don't fully support IPv6 yet in our products.  When we do, turn this on (and validate support again).
	//if(repl == SETUP_OK)
		//repl = SetUpProtIfaces(AF_INET6);
	return repl;
}

//Searches the routing table for a protocol family and sets the default gateway and
//gateway fields of the ifaces in the pool
bool CIfaceSupport::FillInGateways(int protfamily)
{
	//Only IPv4 routes live in /proc/net/route. IPv6 would be /proc/net/ipv6_route
	//in a different format; there is nothing to do for it until IPv6 is turned on
	//in SetUpIfaces above.
	if(AF_INET == protfamily)
	{
		FILE* f = fopen("/proc/net/route", "r");
		if(f)
		{
			char line[512];

			//Discard the header row
			if(fgets(line, sizeof(line), f))
			{
				while(fgets(line, sizeof(line), f))
				{
					char iface[64];
					unsigned long dest = 0;
					unsigned long gateway = 0;
					unsigned int flags = 0;
					unsigned long mask = 0;

					if(5 != sscanf(line, "%63s %lx %lx %x %*d %*d %*d %lx", iface, &dest, &gateway, &flags, &mask))
						continue;

					if(!(flags & kRouteFlagUp))
						continue;

					//A destination and mask of zero is the default route.
					const bool isdefault = (0 == dest) && (0 == mask);

					for(ifaceiter it = m_ifaces.begin(); it != m_ifaces.end(); ++it)
					{
						if(0 != strncmp(it->name, iface, IAsyncSocketServ::NETINTID_STRLEN))
							continue;

						if(isdefault && (NETID_INVALID == m_defaultiface))
							m_defaultiface = it->id;

						//Prefer a real next-hop address; the default route's gateway wins
						//over any earlier on-link route for the same interface.
						if((flags & kRouteFlagGateway) && (0 != gateway))
						{
							if(isdefault || (it->gate == CIPAddr()))
								it->gate.SetV4Address(ntohl(static_cast<uint4>(gateway)));
						}
					}
				}
			}

			fclose(f);
		}
	}

	//Any interface with no next-hop of its own reports itself as its gateway, which
	//is what the OSX implementation ends up doing for non-default interfaces too.
	for(ifaceiter it = m_ifaces.begin(); it != m_ifaces.end(); ++it)
	{
		if(it->gate == CIPAddr())
			it->gate = it->addr;
	}

	//If we haven't found a default interface, pick one.
	if((m_defaultiface == NETID_INVALID) && (!m_ifaces.empty()))
		m_defaultiface = m_ifaces.front().id;

	return true;
}

//Given a sockaddr, attempts to find the network interface in the list.
//The sockaddr could be AF_INET, AF_INET6, or AF_PACKET.  If the sockaddr is an
//address, usenetmask determines whether or not both addrs are masked with the netmask
//before the comparison
CIfaceSupport::ifaceiter CIfaceSupport::FindIfaceBySa(struct sockaddr* sa, bool usenetmask)
{
	if(!sa)
		return m_ifaces.end();

	switch (sa->sa_family)
	{
		case AF_PACKET:
		{
			int index = ((struct sockaddr_ll*)(sa))->sll_ifindex;
			for(ifaceiter it = m_ifaces.begin(); it != m_ifaces.end(); ++it)
			{
				if(it->ifindex == index)
					return it;
			}
			break;
		}

		case AF_INET:
		case AF_INET6:
		{
			CIPAddr addr;
			SetAddress(addr, sa);

			for(ifaceiter it = m_ifaces.begin(); it != m_ifaces.end(); ++it)
			{
				if((usenetmask && MaskCompare(addr.GetV6Address(), it->addr.GetV6Address(), it->mask.GetV6Address())) ||
				   (!usenetmask && (addr == it->addr)))
					return it;
			}
			break;
		}

		default:
			break;

	}
	return m_ifaces.end();
}

//Returns the current number of network interfaces on the machine
int CIfaceSupport::GetNumInterfaces()
{
	return m_ifaces.size();
}

//This function copies the list of network interfaces into a passed in array.
//list MUST contain the necessary amount of memory (new [GetNumInterfaces()] netintinfo)
//The interface numbers are only valid across this instance of the
// async socket library,  To persist the selected interfaces,
// use the ip address to identify the network interface across executions.
void CIfaceSupport::CopyInterfaceList(IAsyncSocketServ::netintinfo* list)
{
	int numinfo = m_ifaces.size();
	for(int i = 0; i < numinfo; ++i)
		list[i] = m_ifaces[i];
}

//Fills in the network interface info for a particular interface id
//Returns false if not found
bool CIfaceSupport::CopyInterfaceInfo(netintid id, IAsyncSocketServ::netintinfo& info)
{
	if((id < 0) || (static_cast<uint>(id) >= m_ifaces.size()))
		return false;
	info = m_ifaces[id];
	return true;
}

//Returns the network interface that is used as the default
netintid CIfaceSupport::GetDefaultInterface()
{
	return m_defaultiface;
}

//Returns true if the mask of the two addresses are equal
//Assumes items passed in are what is returned from CIPAddr.GetV6Address()
bool CIfaceSupport::MaskCompare(const uint1* addr1, const uint1* addr2, const uint1* mask)
{
	//Instead of a byte compare, we'll do a int compare
	const uint4* p1 = reinterpret_cast<const uint4*>(addr1);
	const uint4* p2	= reinterpret_cast<const uint4*>(addr2);
	const uint4* pm = reinterpret_cast<const uint4*>(mask);

	for(int i = 0; i < CIPAddr::ADDRBYTES/4; ++i,++p1,++p2,++pm)
	{
		if((*p1 & *pm) != (*p2 & *pm))
			return false;
	}
	return true;
}

//Returns true if the "mask" address is all 0's (which would skew the Mask compare)
bool CIfaceSupport::MaskIsEmpty(const uint1* mask)
{
	uint4 blob = 0;

	//Instead of a byte check, we'll do an int check
	const uint4* p = reinterpret_cast<const uint4*>(mask);
	for(int i = 0; i < CIPAddr::ADDRBYTES/4; ++i,++p)
		blob |= *p;

	return blob == 0;
}

//Returns the network id (or NETID_INVALID) of the first network interface that
//could communicate directly with this address (ignoring port and iface fields).
//if isdefault is true, this was not directly resolveable and would go through
//the default interface
netintid CIfaceSupport::GetIfaceForDestination(const CIPAddr& destaddr, bool& isdefault)
{
	isdefault = false;

	if(m_ifaces.size() == 0)
		return NETID_INVALID;

	for(ifaceiter it = m_ifaces.begin(); it != m_ifaces.end(); ++it)
	{
		if((!MaskIsEmpty(it->mask.GetV6Address())) &&
			(MaskCompare(it->addr.GetV6Address(), destaddr.GetV6Address(), it->mask.GetV6Address())))
			return it->id;
	}

	if(m_defaultiface != NETID_INVALID)
		isdefault = true;
	return m_defaultiface;
}

//Grabs the local address -- only the address portion is filled in
bool CIfaceSupport::GetLocalAddress(netintid netid, CIPAddr& addr)
{
	IAsyncSocketServ::netintinfo info;
	if(CopyInterfaceInfo(netid, info))
	{
		addr = info.addr;
		addr.SetNetInterface(netid);
		return true;

	}
	return false;
}

//Get the ifindex of a local network interface
bool CIfaceSupport::GetIFIndex(netintid netid, int& ifindex)
{
	IAsyncSocketServ::netintinfo info;
	if(CopyInterfaceInfo(netid, info))
	{
		ifindex = info.ifindex;
		return true;

	}
	return false;
}
