// AsyncSocketServ_select.cpp: implementation of the CAsyncSocketServ class.
//
//This class implements the OSX-Specific IAsyncSocketServ implementation
//of socket support that uses blocking sockets, with a thread that uses select
//to determine when sockets are ready for reading.
//////////////////////////////////////////////////////////////////////

//TODO: VALIDATE IPV6 SUPPORT!!!

#include <vector>
#include <map>
#include <list>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
// Darwin's headers pull this in transitively; glibc's do not.
#include <string.h>

#include "deftypes.h"
#include "defpack.h"
#include "ipaddr.h"
#include "ReaderWriter.h"
#include "MemPool.h"
#include "AsyncSocketInterface.h"
#include "OSX_AsyncSocketInterface.h"
#include "Subscriptions.h"
#include "SockUtil.h"
#include "IfaceSupport.h"
#include "AsyncSocketServ_select.h"

//////////////////////////////////////////////////////////////////////////////////////////////////
// Local constants

#define SOCKET_INVALID	-1
// Max UDP packet size constant from PALSocketApi.h.
// The Vxworks SENS stack had a problem with packets just slightly larger than 33000.
// The newer stack can get up over 40K, but this size should be sufficient in here.
// Especially since ACN sends packets with a maximum size of MTU.
#define PAL_MAX_UDP_PACKET_SIZE		32768
#define MAX_SEND_LENGTH PAL_MAX_UDP_PACKET_SIZE

/////////////////////////////////////////////////////////////////////////////////////
// Platform-dependent Interface class member functions.

//The actual creation
IOSXAsyncSocketServ* IOSXAsyncSocketServ::CreateInstance()
{
	return new CAsyncSocketServ;
}

//Destroys the pointer.  Call Shutdown first
void IOSXAsyncSocketServ::DestroyInstance(IOSXAsyncSocketServ* p)
{
	delete static_cast<CAsyncSocketServ*>(p);
}

//Only one instance of this thread does the select, and does a
//read lock on the sockmap to create the fdset and do the reads
void* AsyncSocket_ReadThread(void* p)
{
	CAsyncSocketServ* psrv = reinterpret_cast<CAsyncSocketServ*>(p);

	// Check for valid arguments.
	if (psrv == NULL)
		return (void*)1;
		
	fd_set rdset;
	
	// Each cycle of the main loop is a select
	// Check for termination each time.
	while(!psrv->m_terminated)
	{
		FD_ZERO(&rdset);  //We zero every time, since the socket map could have changed.
		int maxfd = 0;  //The largest fd found. Select requires at least one more than this number
		
		if(psrv->m_socklock.ReadLock())
		{
			for(CAsyncSocketServ::sockiter it = psrv->m_sockmap.begin(); it != psrv->m_sockmap.end(); ++it)
			{
				if(it->second.m_Socket > maxfd)
					maxfd = it->second.m_Socket;
				if(!it->second.m_ismanual && it->second.m_Connected)
					FD_SET(it->second.m_Socket, &rdset);
			}
			psrv->m_socklock.ReadUnlock();
		}
		
		//every 200 ms, wake up and see if we should close
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200000;
		int sel_res = select(maxfd + 1, &rdset, NULL, NULL, &tv); 
		
		if((sel_res > 0) && !psrv->m_terminated && psrv->m_socklock.ReadLock())
		{
			for(CAsyncSocketServ::sockiter it = psrv->m_sockmap.begin(); it != psrv->m_sockmap.end() && !psrv->m_terminated; ++it)
			{
				int result = 0;
				
				//Do the read on any ready socket. Ignoring errors for now, since the read
				//triggers notifications that should clean up.  Except for the memory error,
				//which shouldn't be a real problem.
				if(FD_ISSET(it->second.m_Socket, &rdset))
					result = psrv->SocketRead(&it->second);
			}
			psrv->m_socklock.ReadUnlock();
		}
	}
	return (void*) 0;
}


//This function is used for manual_recv sockets.  It does a blocking recvfrom and returns
//the number of bytes received into pbuffer (or <0 for the recvfrom socket errors). It also fills in from.
int CAsyncSocketServ::ReceiveInto(sockid id, CIPAddr& from, uint1* pbuffer, uint buflen)
{
	int sock = SOCKET_INVALID;
	netintid iface = NETID_INVALID;
	bool isv4 = true;

	m_socklock.ReadLock();
	sockiter sockit = m_sockmap.find(id);
	if(sockit != m_sockmap.end())
	{
		sock = sockit->second.m_Socket;
		iface = sockit->second.boundaddr.GetNetInterface();
		isv4 = sockit->second.boundaddr.IsV4Address();
	}
	m_socklock.ReadUnlock();

	if(sock == SOCKET_INVALID)
		return -1;
	
	sockaddr_in fromaddr;
	sockaddr_in6 from6addr;
	socklen_t fromsize;
	sockaddr* pfrom;
	if(isv4)
	{
		pfrom = (sockaddr*)&fromaddr;
		fromsize = sizeof(sockaddr_in);
	}
	else
	{
		pfrom = (sockaddr*)&from6addr;
		fromsize = sizeof(sockaddr_in6);
	}
	
	int numread = recvfrom(sock, (char*)pbuffer, buflen, 0, pfrom, &fromsize);
	if(numread >= 0)
	{
		from.SetNetInterface(iface);
		if(isv4)
		{
			from.SetIPPort(ntohs(fromaddr.sin_port));
			from.SetV4Address(ntohl(fromaddr.sin_addr.s_addr));
		}
		else
		{
			from.SetIPPort(ntohs(from6addr.sin6_port));
			from.SetV6Address(from6addr.sin6_addr.s6_addr);
		}
	}

	return numread;
}


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CAsyncSocketServ::CAsyncSocketServ():m_recvpool(1500)
{
	m_id = 0;
	m_terminated = false;
	m_readsize = 0;
	m_readbuffer = NULL;
	lion_or_better = false;
}

CAsyncSocketServ::~CAsyncSocketServ()
{
	if(m_readbuffer)
		delete [] m_readbuffer;
}


//Re-allocates read buffer to larger size
bool CAsyncSocketServ::AssureReadSize(int size)
{
	if(size > m_readsize)
	{
		if(m_readbuffer)
			delete [] m_readbuffer;
			
		m_readsize = size;
		m_readbuffer = new uint1 [m_readsize];
		return m_readbuffer != NULL;
	}
	return true;
} 

//Startup and shutdown functions -- these should be called once directly from the app that
//has the class instance
bool CAsyncSocketServ::Startup()
{
	//The ACN libraries need all fields initialized properly.
	CIfaceSupport::SETUP_RESULT result = m_ifs.SetUpIfaces();
	//printf("Setup_result returned %d\n", result); 
	if(CIfaceSupport::SETUP_OK != result)
		return false;
		
	if(!AssureReadSize(1500))  //Start at MTU
		return false;

	m_recvpool.Reserve(20);  //Prime the pump

	m_terminated = false;
	m_id = 0;
	
	//Start the read thread			
	if(0 != pthread_create(&m_task_id, NULL, AsyncSocket_ReadThread, this))
		return false;
	
	//We need to detect whether or not we are working on Lion or better.
	//Adapted from http://www.cocoadev.com/index.pl?DeterminingOSVersion
	//If we don't get version numbers, we'll assume the old OS
//	long int major = 0;
//	long int minor = 0;
//	Gestalt(gestaltSystemVersionMajor, &major);
//	Gestalt(gestaltSystemVersionMinor, &minor);
//	
//	lion_or_better = ((major > 10) || 
//					  ((major == 10) && (minor >= 7)));
  lion_or_better = true;
	
	return true;
}

void CAsyncSocketServ::Shutdown()
{
	m_terminated = true;
	
	//Close all remaining sockets 
	for(sockiter it1 = m_sockmap.begin(); it1 != m_sockmap.end(); ++it1)
		close(it1->second.m_Socket);
	for(subiter it2 = m_submap.begin(); it2 != m_submap.end(); ++it2)
		close(it2->sockfd);

	//Wait for the read thread to die
	pthread_join(m_task_id, NULL);
			
}

//If this returns true, any socket that binds to a port will receive the mcast message
//if even only one of the sockets subscribed on that network interface.
bool CAsyncSocketServ::McastMessagesSharePort()
{
	return !lion_or_better;  //As of lion, sockets must be subscribed
}
	
//If this returns true, any socket that binds to a port will receive the mcast message,
//even if the one that subscribed was on a different network interface (as long as 
//some socket on that interface subscribed).
bool CAsyncSocketServ::McastMessagesIgnoreSubscribedIface()
{
	return !lion_or_better;  //As of lion, sockets must be subscribed
}

//Returns the current number of network interfaces on the machine
int CAsyncSocketServ::GetNumInterfaces()
{
	return m_ifs.GetNumInterfaces();
}

//This function copies the list of network interfaces into a passed in array.
//list MUST contain the necessary amount of memory (new [GetNumInterfaces()] netintinfo)
//The interface numbers are only valid across this instance of the
// async socket library,  To persist the selected interfaces,
// use the ip address to identify the network interface across executions. 
void CAsyncSocketServ::CopyInterfaceList(netintinfo* list)
{
	m_ifs.CopyInterfaceList(list);
}

//Fills in the network interface info for a particular interface id
//Returns false if not found
bool CAsyncSocketServ::CopyInterfaceInfo(netintid id, netintinfo& info)
{
	return m_ifs.CopyInterfaceInfo(id, info);
}

//Returns the network interface that is used as the default
netintid CAsyncSocketServ::GetDefaultInterface()
{
	return m_ifs.GetDefaultInterface();
}

//Returns the network id (or NETID_INVALID) of the first network interface that
//could communicate directly with this address (ignoring port and iface fields).
//if isdefault is true, this was not directly resolveable and would go through 
//the default interface
netintid CAsyncSocketServ::GetIfaceForDestination(const CIPAddr& destaddr, bool& isdefault)
{
	return m_ifs.GetIfaceForDestination(destaddr, isdefault);
}

/////////////////////////////////////////////////////////////////////////
//The IAsyncSocketServ interface

void CAsyncSocketServ::DeletePacket(uint1* pbuffer, uint buflen)
{
	m_recvpool.Free(pbuffer);
}

//Grabs the local address -- address and port
bool CAsyncSocketServ::GetLocalAddress(sockid sock, CIPAddr& addr)
{
	//On this platform, the bound address may not be the local address, but the interface is the correct one.
	bool result = GetBoundAddress(sock, addr);
	if(result)
		result = GetLocalAddress(addr.GetNetInterface(), addr);
	return result;
}

//Grabs the local address -- only the address portion is filled in
bool CAsyncSocketServ::GetLocalAddress(netintid netid, CIPAddr& addr)
{
	return m_ifs.GetLocalAddress(netid, addr);
}

//Gets the bound address of the socket or an empty address
bool CAsyncSocketServ::safe_GetBoundAddress(sockiter it, CIPAddr& addr)
{
	if(it != m_sockmap.end())
	{
		addr = it->second.boundaddr;
		return true;
	}
	
	return false;
}


//Gets the bound address of the socket or an empty address
bool CAsyncSocketServ::GetBoundAddress(sockid sock, CIPAddr& addr)
{
	bool result = false;

	if ( m_socklock.ReadLock() )
    {
		result = safe_GetBoundAddress(m_sockmap.find(sock), addr);
    	m_socklock.ReadUnlock();
    }

	return result;
}

//Returns the MTU for this socket, as set by the caller
uint CAsyncSocketServ::safe_GetMTU(sockiter it)
{
	if(it != m_sockmap.end())
		return it->second.readsize;
		
	return 0;
}


//Returns the MTU for this socket, as set by the caller
uint CAsyncSocketServ::GetMTU(sockid sock)
{
	uint result = 0;

	if ( m_socklock.ReadLock() )
    {
		result = safe_GetMTU(m_sockmap.find(sock));
    	m_socklock.ReadUnlock();
    }

	return result;
}

//Returns the OS interface index 
//Returns -1 if the index is unknown.
int CAsyncSocketServ::safe_GetOSIndex(sockiter it)
{
	CIPAddr addr;
	if(safe_GetBoundAddress(it, addr))
		return m_ifs.m_ifaces[addr.GetNetInterface()].ifindex;

	return -1;
}


//Returns the OS interface index 
//Returns -1 if the index is unknown.
int CAsyncSocketServ::GetOSIndex(sockid sock)
{
	int result = -1;

	if ( m_socklock.ReadLock() )
	{
		result = safe_GetOSIndex(m_sockmap.find(sock));
		m_socklock.ReadUnlock();
	}
	
	return result;
}

//Returns whether or now this socket layer is on a v6 network
bool CAsyncSocketServ::safe_IsV6(sockiter it)
{
	CIPAddr addr;
	if(safe_GetBoundAddress(it, addr))
		return !addr.IsV4Address();
		
	return false;
}

//Returns whether or now this socket layer is on a v6 network
bool CAsyncSocketServ::IsV6(sockid sock)
{
	bool result = false;

	if ( m_socklock.ReadLock() )
	{
		result = safe_IsV6(m_sockmap.find(sock));
		m_socklock.ReadUnlock();
	}
	
	return result;
}

//Returns true if the socket is already subscribed to the address
bool CAsyncSocketServ::safe_IsSubscribed(sockiter it, const CIPAddr& addr)
{
	if(lion_or_better)
	{
		return it != m_sockmap.end() && it->second.sublist.IsSubscribed(addr);
	}
	else 
	{
		//Because we share everything on a port, just look through m_sockmap on that sock's port
		CIPAddr sockaddr;
		if(!safe_GetBoundAddress(it, sockaddr))  //Checks validity of it
			return false;
		
		if(it->second.m_standalone)
			return it->second.sublist.IsSubscribed(addr);
		
		//See if anyone on that port is already subscribed to the address
		bool result = false;
		for(subiter it = m_submap.begin(); !result && it != m_submap.end(); ++it)
			result = it->subaddrs.IsSubscribed(addr);

		return result;
	}
}


//Returns true if the socket is already subscribed to the address
bool CAsyncSocketServ::IsSubscribed(sockid id, const CIPAddr& addr)
{
	bool result = false;
	if(m_socklock.ReadLock())
	{
		result = safe_IsSubscribed(m_sockmap.find(id), addr);
		m_socklock.ReadUnlock();
	}
	return result;
}

//There may be a system-determined limit for the number of multicast
//addresses that can be subscribed to by the same socket.  Call this
//function to determine if there is room avaliable (or if
//SubscribeMulticast return false).  If the addr passed in is one the
//socket is already subscribed to, this returns true -- you can always
//keep subscribing to the same socket, as it just refcounts internally.
bool CAsyncSocketServ::safe_RoomForSubscribe(sockiter it, const CIPAddr& addr)
{
	bool result = false;
	
	if(lion_or_better)
	{
		//If the socket is already subscribed, it's an immediate pass, otherwise check space.
		if(it != m_sockmap.end())
			result = (it->second.sublist.IsSubscribed(addr) || 
					  ((it->second.m_standalone && (!it->second.sublist.MaxReached(1, 0))) ||
					   (!it->second.m_standalone && (!it->second.sublist.MaxReached(IP_MAX_MEMBERSHIPS, 2)))));
	}
	else 
	{
		//The way we support mcast sockets for OSX, there is always room for subscribe, unless id points to a standalone socket
		result = true;
		if(it != m_sockmap.end() && it->second.m_standalone)
			result = it->second.sublist.IsSubscribed(addr);
	}
	
	return result;
}


//There may be a system-determined limit for the number of multicast
//addresses that can be subscribed to by the same socket.  Call this
//function to determine if there is room avaliable (or if
//SubscribeMulticast return false).  If the addr passed in is one the
//socket is already subscribed to, this returns true -- you can always
//keep subscribing to the same socket, as it just refcounts internally.
bool CAsyncSocketServ::RoomForSubscribe(sockid id, const CIPAddr& addr)
{
	bool result = false;
	
	if(m_socklock.ReadLock())
	{
		result = safe_RoomForSubscribe(m_sockmap.find(id), addr);		
		m_socklock.ReadUnlock();
	}
	
	return result;
}

//Only used if we aren't doing lion or better:
//Adds the subscription to m_msockmap, returning the socket or -1 on error.
//Creates new subscription sockets whenever necessary.
//Sets dosocksubscribe to true when the socket level subscribe should be performed.
//Assumes m_socklock has a WriteLock, as it modifies the internal sub sock list
int CAsyncSocketServ::SubscribeSocketAddSubscription(const CIPAddr& maddr, bool& dosocksubscribe)
{
	if(maddr.GetNetInterface() == NETID_INVALID)
		return -1;
		
	//See if anyone on that port/iface is already subscribed to the address
	for(subiter it = m_submap.begin(); it != m_submap.end(); ++it)
	{
		if(it->subaddrs.IsSubscribed(maddr))
		{
			dosocksubscribe = it->subaddrs.AddSubscription(maddr);
			return it->sockfd;
		}
	}
	
	//See if anyone on that port/iface has room for the subscription
	for(subiter it = m_submap.begin(); it != m_submap.end(); ++it)
	{
		if((it->sockiface == maddr.GetNetInterface()) &&
		   !it->subaddrs.MaxReached(IP_MAX_MEMBERSHIPS, 0))
		{
			dosocksubscribe = it->subaddrs.AddSubscription(maddr);
			return it->sockfd;
		}
	}
		
	//Ok.  We'll create and add a new socket for the subscription
	int fd = socket( maddr.IsV4Address() ? AF_INET : AF_INET6, SOCK_DGRAM, 0);
	if(fd >= 0)
	{
		m_submap.push_back(subref());
		m_submap.back().sockfd = fd;
		m_submap.back().sockiface = maddr.GetNetInterface();
		dosocksubscribe = m_submap.back().subaddrs.AddSubscription(maddr);
	}
	return fd;
}

//Only used if we aren't doing lion or better:
//Does the RemoveSubscription call on the appropriate subscribe socket.
//Returns -1 if the socket level unsubscribe shouldn't happen, otherwise
//returns the sockid to perform the unsubscribe on.
//It assumes m_socklock has a write-lock
int CAsyncSocketServ::SubscribeSocketRemoveSubscription(const CIPAddr& maddr)
{
	for(subiter it = m_submap.begin(); it != m_submap.end(); ++it)
	{
		if(it->subaddrs.IsSubscribed(maddr))
		{
			if(it->subaddrs.RemoveSubscription(maddr))
				return it->sockfd;
			else
				return -1;  //Other guys are using the subscription
		}
	}
	
	return -1;  //Nothing to do
}


//Subscribes a multicast socket to a multicast address.  
//It will return false on error, or if the maximum number of subscription
//addresses has been reached.  If McastMessagesIgnoreSubscribedIface is false,
//the Network interface must match the interface the socket is bound to.  If
//McastMessagesIgnoreSubscribedIface is true, this function turns on the 
//multicast "spigot" for this iface (and in this case, an iface of NETID_INVALID
//turns on multicast for all network interfaces.
bool CAsyncSocketServ::SubscribeMulticast(sockid id, const CIPAddr& addr)
{
	if(!addr.IsMulticastAddress())
		return false;

	CIPAddr boundaddr;
	if(!GetBoundAddress(id, boundaddr))
		return false;
		
	if(!lion_or_better)
	{
		//Because we are sharing ifaces, don't validate the netiface with the
		//bound addr -- but we do need the iface addr 
		if(addr.GetNetInterface() == NETID_INVALID)
		{
			//In the ALL case, just do one level of recursion
			bool result = false;
			CIPAddr tmpaddr = addr;
			IAsyncSocketServ::netintinfo* pinfo = new IAsyncSocketServ::netintinfo [m_ifs.GetNumInterfaces()];
			if(!pinfo)
				return false;
			m_ifs.CopyInterfaceList(pinfo);
			for(int i = 0; i < m_ifs.GetNumInterfaces(); ++i)
			{
				tmpaddr.SetNetInterface(pinfo[i].id);
				result |= SubscribeMulticast(id, tmpaddr);
			}
			delete [] pinfo;
			
			return result;
		}
	}
	
	CIPAddr ifaceaddr;
	if(!m_ifs.GetLocalAddress(addr.GetNetInterface(), ifaceaddr))
		return false;
		
		
	bool dosubscribe = false;
	int subid = -1;  //Will be the socket to do real subscribe to, if one was found

    if ( m_socklock.WriteLock() )
    {
		if(lion_or_better)
		{
			sockiter it = m_sockmap.find(id);
			if(safe_RoomForSubscribe(it, addr))
			{
				dosubscribe = it->second.sublist.AddSubscription(addr);
				subid = it->second.m_Socket;
			}			
		}
		else
		{
			//first, update the overall subscription list for this socket
			sockiter it = m_sockmap.find(id);
			if(it != m_sockmap.end())
				dosubscribe = it->second.sublist.AddSubscription(addr);  //this dosubscribe is overridden by the subscription socket stuff, but needed for standalone sockets.
			
			//Now, do the real subscription logic
			if(it->second.m_standalone)
				subid = it->second.m_Socket;
			else
				subid = SubscribeSocketAddSubscription(addr, dosubscribe);
		}
		
		m_socklock.WriteUnlock();
    }
		
	if(subid >= 0)
	{
		if(dosubscribe)
		{		
			if(addr.IsV4Address())
			{
				struct ip_mreq mreq;
				memset(&mreq, 0, sizeof(mreq));
				mreq.imr_multiaddr.s_addr = htonl(addr.GetV4Address());
				mreq.imr_interface.s_addr = htonl(ifaceaddr.GetV4Address());
				
				return 0 == setsockopt(subid, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
			}
			else
			{
				int ifindex;
				if(m_ifs.GetIFIndex(addr.GetNetInterface(), ifindex))
					return false;
					
				struct ipv6_mreq mreq;
				memset(&mreq, 0, sizeof(mreq));
				memcpy(mreq.ipv6mr_multiaddr.s6_addr, addr.GetV6Address(), CIPAddr::ADDRBYTES);
				mreq.ipv6mr_interface = ifindex;
				
				return 0 == setsockopt(subid, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));									
			}
		}
		return true;  //subscription was already there
	}

    return false;
}

//Unsubscribes a multicast socket from a multicast address.  
//Returns true if the unsubscribe actually ocurred (otherwise, the
//reference count was just lowered).
//Note that on some platforms, if you unsubscribe you can't reuse that
//socket to subscribe to another address.  can_reuse will be set to 
//false in that situation (can_reuse is set whatever this function returns).
//If you can't reuse and the unsubscribe ocurred, you might as well destroy the socket.
//UnsubscribeMulticast follows the same rules as SubcribeMulticast with respect
//to the network interface of addr.
bool CAsyncSocketServ::UnsubscribeMulticast(sockid id, const CIPAddr& addr, bool& can_reuse)
{
	can_reuse = true;  //We can now always reuse a multicast socket -- except in the standalone case.
	
	CIPAddr boundaddr;
	if(!GetBoundAddress(id, boundaddr))
		return true;  //This error is ignored.
	
	//Because we are sharing ifaces, don't validate the netiface with the
	//bound addr -- but we do need the iface addr
	if(addr.GetNetInterface() == NETID_INVALID)
	{
		//In the ALL case, just do one level of recursion
		bool result = false;
		CIPAddr tmpaddr = addr;
		IAsyncSocketServ::netintinfo* pinfo = new IAsyncSocketServ::netintinfo [m_ifs.GetNumInterfaces()];
		if(!pinfo)
			return false;
		m_ifs.CopyInterfaceList(pinfo);
		for(int i = 0; i < m_ifs.GetNumInterfaces(); ++i)
		{
			tmpaddr.SetNetInterface(pinfo[i].id);
			result |= UnsubscribeMulticast(id, tmpaddr, can_reuse);
		}
		delete [] pinfo;
		
		return result;
	}

	CIPAddr ifaceaddr;
	if(!m_ifs.GetLocalAddress(addr.GetNetInterface(), ifaceaddr))
		return false;
		
	int sockfd = -1;  //Will be the socket to unsubscribe from.
    if ( m_socklock.WriteLock() )
    {
		if(lion_or_better)
		{
			sockiter it = m_sockmap.find(id);
			if(it != m_sockmap.end() && it->second.sublist.RemoveSubscription(addr))
			{
				sockfd = it->second.m_Socket;
				can_reuse = !it->second.m_standalone;  //This standalone socket should be destroyed.
			}
		}
		else
		{
			//first, update the overall subscription list for this socket
			sockiter it = m_sockmap.find(id);
			if(it != m_sockmap.end() && it->second.sublist.RemoveSubscription(addr))
				sockfd = it->second.m_Socket;    //Overridden by subscription socket, but needed for standalone
			
			//Now, do the real subscription logic
			if(it->second.m_standalone)
				can_reuse = sockfd > -1;  //If the RemoveSubscription call didn't set the sockfd, they subscribed to the same address multiple times...
			else
				sockfd = SubscribeSocketRemoveSubscription(addr);
		}
		
		m_socklock.WriteUnlock();
    }
		
	if(sockfd >= 0)
	{
		if(addr.IsV4Address())
		{
			struct ip_mreq mreq;
			memset( &mreq, 0, sizeof(mreq) );
			mreq.imr_multiaddr.s_addr = htonl(addr.GetV4Address());
			mreq.imr_interface.s_addr = htonl(ifaceaddr.GetV4Address());
			setsockopt(sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
		}
		else
		{
			int ifindex;
			if(!m_ifs.GetIFIndex(addr.GetNetInterface(), ifindex))
			{					
				struct ipv6_mreq mreq;
				memset(&mreq, 0, sizeof(mreq));
				memcpy(mreq.ipv6mr_multiaddr.s6_addr, addr.GetV6Address(), CIPAddr::ADDRBYTES);
				mreq.ipv6mr_interface = ifindex;		
				setsockopt(sockfd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq));		
			}
		}
		
		return true;  //Even if the setsockopt failed, everything else has cleaned up.  Besides, that should succeed if the join group succeeded.
	}
	else
		return false;  //Can't do the real unsubscribe, probably others there.
}

//Creates a multicast socket, binding to the correct port and network interface.
//This does not subscribe to an address, call SubscribeMulticast instead.
//if port == 0, a random port is assigned and a new socket is automatically
//generated.
//If manual_recv is true, the user must call ReceiveInto to receive any data,
//as the ReceivePacket notification is not used.
//On success, newsock is filled in.
bool CAsyncSocketServ::CreateMulticastSocket(IAsyncSocketClient* pnotify,
											 netintid netid,
											 sockid& newsock,
											 IPPort& port, 
											 uint maxdatasize,
											 bool manual_recv)
{
	return RealCreateSocket(false, pnotify, netid, newsock, port, maxdatasize, manual_recv, NULL);
}

//In instances where McastMessagesSharePort or McastMessagesIgnoreSubscribedIface,
//it may not be adviseable to share subscriptions if multiple protocols will be using
//that port.  In the case of sACN vs. ACN, while the protocol code does filter out
//invalid packets the packets have to get all the way to that level before they are
//filtered.  This needs to be balanced with scaling needs, however, so while sACN will
//most likely use this function, ACN will not.
//This function creates a multicast socket for the explicit use of a particular multicast
//address, and will only receive messages for that address. It also immediately subscribes
//to the address. Otherwise it works like the previous function. 
//The mcast address must be completely filled in, port must not be 0.
bool CAsyncSocketServ::CreateStandaloneMulticastSocket(IAsyncSocketClient* pnotify,
														const CIPAddr& maddr,
														sockid& newsock,
														uint maxdatasize,
														bool manual_recv)
{
	IPPort tmpport = maddr.GetIPPort();  
	CIPAddr tmpaddr = maddr;
	if (RealCreateSocket(false, pnotify, maddr.GetNetInterface(), newsock, tmpport, maxdatasize, manual_recv, &tmpaddr))
		return SubscribeMulticast(newsock, maddr);
	return false;
}



//Creates, sets up, and binds a listening unicast socket.
//If port == 0, a random port is assigned.
//If manual_recv is true, the user must call ReceiveInto to receive any data,
//as the ReceivePacket notification is not used.
//On success, newsock is filled in
bool CAsyncSocketServ::CreateUnicastSocket(IAsyncSocketClient* pnotify,
	 									    netintid netid,
											sockid& newsock,
											IPPort& port,
											uint maxdatasize,
											bool manual_recv)
{
	return RealCreateSocket(true, pnotify, netid, newsock, port, maxdatasize, manual_recv, NULL);
}
	
//The real socket creation function -- whether or not we are doing unicast
//affects what address we bind to.
bool CAsyncSocketServ::RealCreateSocket(bool unicast, IAsyncSocketClient* pnotify,
									netintid netid,
									sockid& newsock,
									IPPort& port,
									uint maxdatasize,
									bool manual_recv,
									CIPAddr* mcast_bind)
{
	std::pair<sockiter, bool> mapsock_result;
	CIPAddr addr;
	socketref ref;
	sockid testsock;
	int result;
	
	// Setup the bound address for the socket,
	// which is the address of its network interface,
	// along with the requested port number.
	if(!GetLocalAddress(netid, addr))
		return false;
	addr.SetIPPort(port);
	addr.SetNetInterface(netid);
	
	// Init necessary socketref variables before call to SocketConnect.
	ref.readsize = maxdatasize;
	if(mcast_bind)
	{
		ref.m_standalone = true;
		ref.boundaddr = *mcast_bind;
	}
	else
	{
		ref.m_standalone = false;
		ref.boundaddr = addr;
	}
	ref.m_sockcb = pnotify;
	ref.m_ismanual = manual_recv;


	// Create socket, set options, bind, allocate read buffer.
	result = SocketConnect(&ref, unicast, mcast_bind);
	if(0 == result)
	{
		if ( m_socklock.WriteLock() )
		{
			//Make sure we still have room for this in the select
			if(m_sockmap.size() + 1 >= FD_SETSIZE)
			{
				SocketDisconnect(&ref);
				m_socklock.WriteUnlock();
				return false;
			}
			
			//To make sure we haven't flipped over, make sure the id isn't used already
			//If all the sockets are in use, the socket creation fails
			testsock = m_id;
			while(m_sockmap.find(m_id) != m_sockmap.end())
			{
				IncID(m_id);
				if(m_id == testsock)  //we've wrapped around again
				{
					SocketDisconnect(&ref);
					m_socklock.WriteUnlock();
					return false;
				}
			}
			ref.m_id = m_id;
			mapsock_result = m_sockmap.insert(sockval(m_id, ref));
			newsock = m_id;
			IncID(m_id);
			
			if(manual_recv || (SocketStartAutoRecv(&mapsock_result.first->second)))
			{
				m_socklock.WriteUnlock();
				return true;
			}
			else
			{
				// Remove the just-added map element, using the iterator returned from insert.
				m_sockmap.erase(mapsock_result.first);
				SocketDisconnect(&ref);
				m_socklock.WriteUnlock();
				return false;
			}
		}
	}

	return false;
}

//Destroys the socket if all references have been destroyed.
//All messages for that socket will be sent before destruction
//The socket id should no longer be used for that socket, and may be reused on another create.
void CAsyncSocketServ::DestroySocket(sockid id)
{
	if ( m_socklock.WriteLock() )
    {
    	sockiter sockit = m_sockmap.find(id);
    	if(sockit != m_sockmap.end())
    	{
            //Kill the socket
            SocketDisconnect(&sockit->second);

            m_sockmap.erase(sockit);
    	}
    	m_socklock.WriteUnlock();
    }
}


//Sends the packet to a particular address.
//pbuffer is considered INVALID by this library after this function
//returns, as the calling code may reuse or delete it.
//If there is an error, this will only trigger a SocketBad notification
//if error_is_failure is true.  Note that in many cases (like SDT), an error on 
//sending should actually be ignored.
void CAsyncSocketServ::SendPacket(sockid id, const CIPAddr& addr, const uint1* pbuffer, uint buflen, bool error_is_failure)
{
    int sentLength;
    struct sockaddr_in sa;
	struct sockaddr_in6 sa6;
	struct sockaddr* psa;
	socklen_t salen;

    if ( pbuffer == NULL )
        return;
    if ( buflen > MAX_SEND_LENGTH )
        return;

    // Setup destination address structure for sendto.
    if(addr.IsV4Address())
	{
		psa = (struct sockaddr*) &sa;
		salen = sizeof(sa);
		memset(psa, 0, salen);
		sa.sin_family = AF_INET;
		sa.sin_port = htons(addr.GetIPPort());
		sa.sin_addr.s_addr = htonl(addr.GetV4Address());
	}
	else
	{
		psa = (struct sockaddr*) &sa6;
		salen = sizeof(sa6);
		memset(psa, 0, salen);
		sa6.sin6_family = AF_INET6;
		sa6.sin6_port = htons(addr.GetIPPort());
		memcpy(sa6.sin6_addr.s6_addr, addr.GetV6Address(), CIPAddr::ADDRBYTES);
	}

    if ( m_socklock.ReadLock() )
    {
        // Find the socket by its socket map id
    	CAsyncSocketServ::sockiter it = m_sockmap.find(id);
    	if(it != m_sockmap.end())
    	{
            // if port == 0, use local port number;
            if(addr.IsV4Address() && (sa.sin_port == 0))
                sa.sin_port = htons(it->second.boundaddr.GetIPPort());
			else if(!addr.IsV4Address() && (sa6.sin6_port == 0))
				sa6.sin6_port = htons(it->second.boundaddr.GetIPPort());

            // Transmit the data
            sentLength = sendto(it->second.m_Socket, (caddr_t)pbuffer, buflen, /*flags*/ 0,
                                psa, salen);

            if (((uint)sentLength != buflen) && error_is_failure )
    		{
                SocketDisconnect(&it->second);
    			it->second.m_sockcb->SocketBad(id);
    		}
    	}
    	m_socklock.ReadUnlock();
    }
}

//The version that takes async_chunk instead of a straight buffer.  The etcchunk
//chain should be considered INVALID by this library after this function returns,
//as the calling code may reuse or delete it.
void CAsyncSocketServ::SendPacket(sockid id, const CIPAddr& addr, const async_chunk* chunks, bool error_is_failure)
{
    uint1 *pbuffer, *pbuf;
	uint count = 0;
	const async_chunk* pchunk = NULL;

    for(pchunk = chunks; pchunk != NULL; pchunk = pchunk->pnext)
		count += pchunk->len;

	//Allocate a new buffer to hold the entire packet
	pbuffer = new uint1 [count];
	if(pbuffer)
	{
		pbuf = pbuffer;
		for(pchunk = chunks; pchunk != NULL; pchunk = pchunk->pnext)
		{
			memcpy(pbuf, pchunk->pbuf, pchunk->len);
			pbuf += pchunk->len;
		}
		SendPacket(id, addr, pbuffer, count, error_is_failure);
		delete [] pbuffer;
	}
}


////////////////////////////////////////////////////////////////////////////////////
// Socket helper functions

int CAsyncSocketServ::SocketConnect(socketref* pref, bool unicast, CIPAddr* mcast_bind)
{
    int optInt;
	int optChar;

    // Ensure the socket is disconnected first.
	SocketDisconnect(pref);

    // Only connect initially or if previously disconnected.
    if (pref->m_Connected )
        return 0;
		
	bool isv4 = pref->boundaddr.IsV4Address();
	int osindex = m_ifs.m_ifaces[pref->boundaddr.GetNetInterface()].ifindex;
	CIPAddr localaddr = m_ifs.m_ifaces[pref->boundaddr.GetNetInterface()].addr;

    // Create the socket
    if(isv4)
		pref->m_Socket = socket(AF_INET, SOCK_DGRAM, 0);
	else
		pref->m_Socket = socket(AF_INET6, SOCK_DGRAM, 0);
		
    if (pref->m_Socket < 0 )
        return -1;

    // Set socket options: REUSE ADDR, REUSE PORT, MULTICAST TTL, MULTICAST INTERFACE
    optInt = 1;
    if (0 != setsockopt(pref->m_Socket, SOL_SOCKET, SO_REUSEPORT, &optInt, sizeof(optInt)))
    {
        close(pref->m_Socket);
		pref->m_Socket = SOCKET_INVALID;
        return -2;
    }

    if (0 != setsockopt(pref->m_Socket, SOL_SOCKET, SO_REUSEADDR, &optInt, sizeof(optInt)))
    {
        close(pref->m_Socket);
		pref->m_Socket = SOCKET_INVALID;
        return -2;
    }
	
	if(isv4)
	{
		optChar = 64;
		if(0 != setsockopt(pref->m_Socket, IPPROTO_IP, IP_MULTICAST_TTL, &optChar, sizeof(optChar)))
		{
			close(pref->m_Socket);
			pref->m_Socket = SOCKET_INVALID;
			return -3;
		}
	}
	else
	{
		optInt = 64;
		if(0 != setsockopt(pref->m_Socket, IPPROTO_IP, IPV6_MULTICAST_HOPS, &optInt, sizeof(optInt)))
		{
			close(pref->m_Socket);
			pref->m_Socket = SOCKET_INVALID;
			return -3;
		}
	}
	
    // Assign the bound interface address as the interface for multicast packets,
    if(isv4)
	{
		struct in_addr sa;
		sa.s_addr = htonl(localaddr.GetV4Address());
		if(0 != setsockopt(pref->m_Socket, IPPROTO_IP, IP_MULTICAST_IF, &sa, sizeof(sa)))
		{
			close(pref->m_Socket);
			pref->m_Socket = SOCKET_INVALID;
			return -5;
		}
	}
	else
	{
		if(0 != setsockopt(pref->m_Socket, IPPROTO_IP, IPV6_MULTICAST_IF, &osindex, sizeof(osindex)))
		{
			close(pref->m_Socket);
			pref->m_Socket = SOCKET_INVALID;
			return -5;
		}
		
	}

	if(unicast)
	{
		int result = SocketUnicastBind(pref, isv4);
		if(result == 0)
		{
			pref->m_Connected = true;
			return 0;
		}
		else
		{
			close(pref->m_Socket);
			pref->m_Socket = SOCKET_INVALID;
			return result;
		}
	}
	else
	{		
		if(0 == SocketMcastBind(pref->m_Socket, isv4, pref->boundaddr.GetIPPort(), mcast_bind))
		{
			pref->m_Connected = true;
			return 0;
		}
		else
		{
			close(pref->m_Socket);
			pref->m_Socket = SOCKET_INVALID;
			return -6;
		}
	}	
}

//Helper utility that does the appropriate bind to any.
int CAsyncSocketServ::SocketMcastBind(int sockfd, bool isv4, IPPort port, CIPAddr* mcast_bind)
{
	struct sockaddr_storage saddr;
	memset(&saddr, 0, sizeof(struct sockaddr_storage));	
	if(isv4)
	{
// sockaddr_storage has no ss_len outside the BSDs; it is informational there.
#ifdef HAVE_SOCKADDR_SA_LEN
		saddr.ss_len = sizeof(struct sockaddr_in);
#endif
		saddr.ss_family = AF_INET;
		if(!mcast_bind)
			((struct sockaddr_in*)&saddr)->sin_addr.s_addr = htonl(INADDR_ANY);
		else
			((struct sockaddr_in*)&saddr)->sin_addr.s_addr = htonl(mcast_bind->GetV4Address());
		((struct sockaddr_in*)&saddr)->sin_port = htons(port);

		return bind(sockfd, (struct sockaddr*) &saddr, sizeof(struct sockaddr_in));
	}
	else
	{
// sockaddr_storage has no ss_len outside the BSDs; it is informational there.
#ifdef HAVE_SOCKADDR_SA_LEN
		saddr.ss_len = sizeof(struct sockaddr_in6);
#endif
		saddr.ss_family = AF_INET6;
		if(!mcast_bind)
			((struct sockaddr_in6*)&saddr)->sin6_addr = in6addr_any;
		else
			memcpy(((struct sockaddr_in6*)&saddr)->sin6_addr.s6_addr, mcast_bind->GetV6Address(), CIPAddr::ADDRBYTES);
		((struct sockaddr_in6*)&saddr)->sin6_port = htons(port);
			
		return bind(sockfd, (struct sockaddr*) &saddr, sizeof(struct sockaddr_in6));
	}	
}

//Binds to the network interface, changing the port if necessary
int CAsyncSocketServ::SocketUnicastBind(socketref* pref, bool isv4)
{
	// Bind to port and localaddr addr.
	struct sockaddr_storage saddr;
	memset(&saddr, 0, sizeof(struct sockaddr_storage));
	
	int bindresult = 0;
	
	if(isv4)
	{
// sockaddr_storage has no ss_len outside the BSDs; it is informational there.
#ifdef HAVE_SOCKADDR_SA_LEN
		saddr.ss_len = sizeof(struct sockaddr_in);
#endif
		saddr.ss_family = AF_INET;
		((struct sockaddr_in*)&saddr)->sin_addr.s_addr = htonl(pref->boundaddr.GetV4Address()); 
		((struct sockaddr_in*)&saddr)->sin_port = htons(pref->boundaddr.GetIPPort());  //If it is port 0, it will be bound to a unique local port
			
		bindresult = bind(pref->m_Socket, (struct sockaddr*) &saddr, sizeof(struct sockaddr_in));
	}
	else
	{
// sockaddr_storage has no ss_len outside the BSDs; it is informational there.
#ifdef HAVE_SOCKADDR_SA_LEN
		saddr.ss_len = sizeof(struct sockaddr_in);
#endif
		saddr.ss_family = AF_INET6;
		memcpy(((struct sockaddr_in6*)&saddr)->sin6_addr.s6_addr, pref->boundaddr.GetV6Address(), CIPAddr::ADDRBYTES);
		((struct sockaddr_in6*)&saddr)->sin6_port = htons(pref->boundaddr.GetIPPort());
			
		bindresult = bind(pref->m_Socket, (struct sockaddr*) &saddr, sizeof(struct sockaddr_in6));
	}
	
	 if(0 != bindresult)
		return -8;

	 // Get the port number from the socket, store it in the local address variable.
	 int getresult = -1;
	 if(isv4)
	 {
		 socklen_t size = sizeof(struct sockaddr_in);
		 getresult = getsockname(pref->m_Socket, (struct sockaddr*)&saddr, &size);
		 if(0 == getresult)
			pref->boundaddr.SetIPPort(ntohs(((struct sockaddr_in*)&saddr)->sin_port));
	 }
	 else
	 {
		 socklen_t size = sizeof(struct sockaddr_in6);
		 getresult = getsockname(pref->m_Socket, (struct sockaddr*)&saddr, &size);
		 if(0 == getresult)
			pref->boundaddr.SetIPPort(ntohs(((struct sockaddr_in6*)&saddr)->sin6_port));
	 }
    
	 if(getresult != 0)
		 return -9;

	 return 0;	
}

bool CAsyncSocketServ::SocketStartAutoRecv(socketref* pref)
{
	if(!pref->m_ismanual)
	{
		//In the select way of things, m_ismanual controls the select, so nothing to do
		return true;
	}

	return true;
}

int CAsyncSocketServ::SocketDisconnect(socketref* pref)
{
   if(!pref->m_Connected)
		return -1;

    // Set flag before closing socket so that AsyncSocket_ReadThread will
    // see it false when the closing socket wakes them up.
	pref->m_Connected = false;	
		
    //We're going to unsubscribe from all multicast groups on the subscription sockets
    //that contain our subscriptions.
	struct ip_mreq mreq;
	struct ipv6_mreq m6req;
	memset(&mreq, 0, sizeof(mreq));
	memset(&m6req, 0, sizeof(m6req));
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	m6req.ipv6mr_interface = 0;

    CIPAddr addr;
	int refcnt;
 
    while(pref->sublist.PopSubscription(addr, refcnt))
	{
		for(int i = 0; i < refcnt; ++i)
		{
			int sockfd = SubscribeSocketRemoveSubscription(addr);
			if(sockfd >= 0)
			{
				// Unsubscribe from this address
				if(addr.IsV4Address())
				{
					mreq.imr_multiaddr.s_addr = htonl(addr.GetV4Address());	
					setsockopt(sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
				}
				else
				{
					memcpy(m6req.ipv6mr_multiaddr.s6_addr, addr.GetV6Address(), CIPAddr::ADDRBYTES);
					setsockopt(sockfd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &m6req, sizeof(m6req));
				}
			}
		}
	}

	//In linux, shutdown seems to trigger the recvfrom to exit, and close does not.
	shutdown(pref->m_Socket, SHUT_RDWR);
	close(pref->m_Socket);
	pref->m_Socket = SOCKET_INVALID;

    return 0;
}

//Does the actual read.  Assumes epoll has said it's readable, and there's a read lock
//on the socket map.  Returns 0 on success.
int CAsyncSocketServ::SocketRead(socketref* pref)
{
	if(!pref || !pref->m_Connected || !pref->m_sockcb)
		return 0;  //Again, can't read.
	
	if(!AssureReadSize(pref->readsize))
		return -1; //Out of memory?		
	
	// Setup recvfrom parameters.
	struct sockaddr_in sa;
	struct sockaddr_in6 sa6;
	socklen_t sa_len;
	struct sockaddr* psa;
	if(pref->boundaddr.IsV4Address())
	{
		sa_len = sizeof(sa);
		psa = (sockaddr*) &sa;
	}
	else
	{
		sa_len = sizeof(sa6);
		psa = (sockaddr*) &sa6;
	}			
	memset(psa, 0, sa_len); 

	//Do the actual read.  Shouldn't usually block, since we did an epoll first
	int read_result = recvfrom(pref->m_Socket, (char *)m_readbuffer, m_readsize, 0, psa, &sa_len);
	if(read_result == -1)
	{
		// Tell app about error -- The app will call DestroySocket.
		pref->m_sockcb->SocketBad(pref->m_id);
		return -2;
	}
	else if((read_result > 0) && (read_result < 1500)) //Got some bytes, and it fits in memory pool
	{
		//Massage the recvfrom address.
		CIPAddr fromaddr;
		fromaddr.SetNetInterface(pref->boundaddr.GetNetInterface());
		if(pref->boundaddr.IsV4Address())
		{
			fromaddr.SetIPPort(ntohs(sa.sin_port));
			fromaddr.SetV4Address(ntohl(sa.sin_addr.s_addr));
		}
		else
		{
			fromaddr.SetIPPort(ntohs(sa6.sin6_port));
			fromaddr.SetV6Address(sa6.sin6_addr.s6_addr);
		}

		//We're calling process packet here, since we know it is handled async
		uint1* buffer = (uint1*)m_recvpool.Alloc();
		if(buffer)
		{
			// Copy packet from input buffer to newly allocated buffer.
			memcpy(buffer, m_readbuffer, read_result);
			// Tell app that the packet is ready.
			// The app takes care of deleting the packet buffer.
			pref->m_sockcb->ReceivePacket(pref->m_id, fromaddr, buffer, read_result);
		}
	}
	return 0;
}


