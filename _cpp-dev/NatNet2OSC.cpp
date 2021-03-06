// FreeBSD license
// 
// Copyright 2011 ICST ZHdK. All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
//    1. Redistributions of source code must retain the above copyright notice, this list of
//       conditions and the following disclaimer.
// 
//    2. Redistributions in binary form must reproduce the above copyright notice, this list
//       of conditions and the following disclaimer in the documentation and/or other materials
//       provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
// ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// The views and conclusions contained in the software and documentation are those of the
// authors and should not be interpreted as representing official policies, either expressed
// or implied, of ICST ZHdK.
//
// Original SDK license:
//
//=============================================================================
// Copyright � 2010 NaturalPoint, Inc. All Rights Reserved.
// 
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall NaturalPoint, Inc. or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//=============================================================================

/*
NatNet2OSC.cpp

Decodes NatNet packets directly.

Usage [optional]:

// OSCNatNetClient [OSCIP] [OSCPort]
// 
// [OSCIP]				IP address of client ( defaults to 255.255.255.255)
// [OSCPort]			Port of client ( defaults to 12345)

*/

#include <stdio.h>
#include <tchar.h>
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <time.h>
#include <windows.h>

#include "natnet/NatNetTypes.h"
#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"

#pragma warning( disable : 4996 )
#pragma comment(lib, "winmm.lib")

#define MAX_NAMELENGTH              256

// NATNET message ids
#define NAT_PING                    0 
#define NAT_PINGRESPONSE            1
#define NAT_REQUEST                 2
#define NAT_RESPONSE                3
#define NAT_REQUEST_MODELDEF        4
#define NAT_MODELDEF                5
#define NAT_REQUEST_FRAMEOFDATA     6
#define NAT_FRAMEOFDATA             7
#define NAT_MESSAGESTRING           8
#define NAT_UNRECOGNIZED_REQUEST    100
#define UNDEFINED                   999999.9999

#define MAX_PACKETSIZE				100000	// max size of packet (actual packet size is dynamic)

bool IPAddress_StringToAddr(char *szNameOrAddress, struct in_addr *Address);
void Unpack(char* pData);
int GetLocalIPAddresses(unsigned long Addresses[], int nMax);
char* _getOSCTimeStamp();

#define MULTICAST_ADDRESS		"239.255.42.99"     // IANA, local network
#define PORT_COMMAND            1510
#define PORT_DATA  			    1511                // Default multicast group

SOCKET CommandSocket;
SOCKET DataSocket;
in_addr ServerAddress;
sockaddr_in HostAddr;  

in_addr LocalAddress;
UdpTransmitSocket* transmitSocket;
char* OSCIP = NULL;
int OSCPort = 54321;

short verbose = 0;
short legacy = 0;


int NatNetVersion[4] = {0,0,0,0};
int ServerVersion[4] = {0,0,0,0};

// command response listener thread
DWORD WINAPI CommandListenThread(void* dummy)
{
    int addr_len;
    int nDataBytesReceived;
    char str[256];
    sockaddr_in TheirAddress;
    sPacket PacketIn;
    addr_len = sizeof(struct sockaddr);

    while (1)
    {
        // blocking
        nDataBytesReceived = recvfrom( CommandSocket,(char *)&PacketIn, sizeof(sPacket),
            0, (struct sockaddr *)&TheirAddress, &addr_len);

        if((nDataBytesReceived == 0) || (nDataBytesReceived == SOCKET_ERROR) )
            continue;

        // debug - print message
        sprintf(str, "[Client] Received command from %d.%d.%d.%d: Command=%d, nDataBytes=%d",
            TheirAddress.sin_addr.S_un.S_un_b.s_b1, TheirAddress.sin_addr.S_un.S_un_b.s_b2,
            TheirAddress.sin_addr.S_un.S_un_b.s_b3, TheirAddress.sin_addr.S_un.S_un_b.s_b4,
            (int)PacketIn.iMessage, (int)PacketIn.nDataBytes);


        // handle command
        switch (PacketIn.iMessage)
        {
        case NAT_MODELDEF:
            Unpack((char*)&PacketIn);
            break;
        case NAT_FRAMEOFDATA:
            Unpack((char*)&PacketIn);
            break;
        case NAT_PINGRESPONSE:
            for(int i=0; i<4; i++)
            {
                NatNetVersion[i] = (int)PacketIn.Data.Sender.NatNetVersion[i];
                ServerVersion[i] = (int)PacketIn.Data.Sender.Version[i];
            }
            break;
        case NAT_RESPONSE:
            {
                char* szResponse = (char *)PacketIn.Data.cData;
                printf("Response : %s",szResponse);
                break;
            }
        case NAT_UNRECOGNIZED_REQUEST:
            printf("[Client] received 'unrecognized request'\n");
            break;
        case NAT_MESSAGESTRING:
            printf("[Client] Received message: %s\n", PacketIn.Data.szData);
            break;
        }
    }

    return 0;
}

// Data listener thread
DWORD WINAPI DataListenThread(void* dummy)
{
    char  szData[20000];
    int addr_len = sizeof(struct sockaddr);
    sockaddr_in TheirAddress;

    while (1)
    {
        // Block until we receive a datagram from the network (from anyone including ourselves)
        int nDataBytesReceived = recvfrom(DataSocket, szData, sizeof(szData), 0, (sockaddr *)&TheirAddress, &addr_len);
        Unpack(szData);
    }

    return 0;
}

SOCKET CreateCommandSocket(unsigned long IP_Address, unsigned short uPort)
{
    struct sockaddr_in my_addr;     
    static unsigned long ivalue;
    static unsigned long bFlag;
    int nlengthofsztemp = 64;  
    SOCKET sockfd;

    // Create a blocking, datagram socket
    if ((sockfd=socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET)
    {
        return -1;
    }

    // bind socket
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(uPort);
    my_addr.sin_addr.S_un.S_addr = IP_Address;
    if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == SOCKET_ERROR)
    {
        closesocket(sockfd);
        return -1;
    }

    // set to broadcast mode
    ivalue = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (char *)&ivalue, sizeof(ivalue)) == SOCKET_ERROR)
    {
        closesocket(sockfd);
        return -1;
    }

    return sockfd;
}

int main(int argc, char* argv[])
{
	if((argc < 2) || (argc > 5)){
		printf("\nUsage: NatNet2OSC <OSCIP (localIP)> <OSCPort (54321)> [verbose] [legacy]\n");
    } 

	printf("\n---- NatNet2OSC v. 1.0         ----");
	printf("\n---- 20141002 by maybites      ----");
	printf("\n----    based on code by jasch ----\n");

    int retval;
    char szMyIPAddress[128] = "";
    char szServerIPAddress[128] = "";
    in_addr MyAddress, MultiCastAddress;
    WSADATA WsaData; 
    int optval = 0x100000;
    int optval_size = 4;

    if (WSAStartup(0x202, &WsaData) == SOCKET_ERROR)
    {
		printf("[NatNet2OSC] WSAStartup failed (error: %d)\n", WSAGetLastError());
        WSACleanup();
        return 0;
    }

	// server address
	GetLocalIPAddresses((unsigned long *)&ServerAddress, 1);
	sprintf_s(szServerIPAddress, "%d.%d.%d.%d", ServerAddress.S_un.S_un_b.s_b1, ServerAddress.S_un.S_un_b.s_b2, ServerAddress.S_un.S_un_b.s_b3, ServerAddress.S_un.S_un_b.s_b4);

	// client address
	GetLocalIPAddresses((unsigned long *)&MyAddress, 1);
	sprintf_s(szMyIPAddress, "%d.%d.%d.%d", MyAddress.S_un.S_un_b.s_b1, MyAddress.S_un.S_un_b.s_b2, MyAddress.S_un.S_un_b.s_b3, MyAddress.S_un.S_un_b.s_b4);

	MultiCastAddress.S_un.S_addr = inet_addr(MULTICAST_ADDRESS);   
    printf("Client: %s\n", szMyIPAddress);
    printf("Server: %s\n", szServerIPAddress);
    printf("Multicast Group: %s\n", MULTICAST_ADDRESS);

    // create "Command" socket
    int port = 0;
    CommandSocket = CreateCommandSocket(MyAddress.S_un.S_addr,port);
    if(CommandSocket == -1)
    {
        // error
    }
    else
    {
        // [optional] set to non-blocking
        //u_long iMode=1;
        //ioctlsocket(CommandSocket,FIONBIO,&iMode); 
        // set buffer
        setsockopt(CommandSocket, SOL_SOCKET, SO_RCVBUF, (char *)&optval, 4);
        getsockopt(CommandSocket, SOL_SOCKET, SO_RCVBUF, (char *)&optval, &optval_size);
        if (optval != 0x100000)
        {
            // err - actual size...
        }
        // startup our "Command Listener" thread
        SECURITY_ATTRIBUTES security_attribs;
        security_attribs.nLength = sizeof(SECURITY_ATTRIBUTES);
        security_attribs.lpSecurityDescriptor = NULL;
        security_attribs.bInheritHandle = TRUE;
        DWORD CommandListenThread_ID;
        HANDLE CommandListenThread_Handle;
        CommandListenThread_Handle = CreateThread( &security_attribs, 0, CommandListenThread, NULL, 0, &CommandListenThread_ID);
    }

    // create a "Data" socket
    DataSocket = socket(AF_INET, SOCK_DGRAM, 0);

    // allow multiple clients on same machine to use address/port
    int value = 1;
    retval = setsockopt(DataSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&value, sizeof(value));
    if (retval == SOCKET_ERROR)
    {
        closesocket(DataSocket);
        return -1;
    }

    struct sockaddr_in MySocketAddr;
    memset(&MySocketAddr, 0, sizeof(MySocketAddr));
    MySocketAddr.sin_family = AF_INET;
    MySocketAddr.sin_port = htons(PORT_DATA);
    MySocketAddr.sin_addr = MyAddress; 
    if (bind(DataSocket, (struct sockaddr *)&MySocketAddr, sizeof(struct sockaddr)) == SOCKET_ERROR)
    {
		printf("[NatNet2OSC] bind failed (error: %d)\n", WSAGetLastError());
        WSACleanup();
        return 0;
    }
    // join multicast group
    struct ip_mreq Mreq;
    Mreq.imr_multiaddr = MultiCastAddress;
    Mreq.imr_interface = MyAddress;
    retval = setsockopt(DataSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&Mreq, sizeof(Mreq));
    if (retval == SOCKET_ERROR)
    {
        printf("[NatNet2OSC] join failed (error: %d)\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }
	// create a 1MB buffer
    setsockopt(DataSocket, SOL_SOCKET, SO_RCVBUF, (char *)&optval, 4);
    getsockopt(DataSocket, SOL_SOCKET, SO_RCVBUF, (char *)&optval, &optval_size);
    if (optval != 0x100000)
    {
        printf("[NatNet2OSC] ReceiveBuffer size = %d", optval);
    }
    // startup our "Data Listener" thread
    SECURITY_ATTRIBUTES security_attribs;
    security_attribs.nLength = sizeof(SECURITY_ATTRIBUTES);
    security_attribs.lpSecurityDescriptor = NULL;
    security_attribs.bInheritHandle = TRUE;
    DWORD DataListenThread_ID;
    HANDLE DataListenThread_Handle;
    DataListenThread_Handle = CreateThread( &security_attribs, 0, DataListenThread, NULL, 0, &DataListenThread_ID);

    // server address for commands
    memset(&HostAddr, 0, sizeof(HostAddr));
    HostAddr.sin_family = AF_INET;        
    HostAddr.sin_port = htons(PORT_COMMAND); 
    HostAddr.sin_addr = ServerAddress;

    // send initial ping command
    sPacket PacketOut;
    PacketOut.iMessage = NAT_PING;
    PacketOut.nDataBytes = 0;
    int nTries = 3;
    while (nTries--)
    {
        int iRet = sendto(CommandSocket, (char *)&PacketOut, 4 + PacketOut.nDataBytes, 0, (sockaddr *)&HostAddr, sizeof(HostAddr));
        if(iRet != SOCKET_ERROR)
            break;
    }

	// OSC address
	char szLocaIPAddress[128] = "";
	if(argc>1){
		OSCIP = &argv[1][0];	// specified on command line
	} else {
		GetLocalIPAddresses((unsigned long *)&LocalAddress, 1);
		sprintf_s(szLocaIPAddress, "%d.%d.%d.%d", LocalAddress.S_un.S_un_b.s_b1, LocalAddress.S_un.S_un_b.s_b2, LocalAddress.S_un.S_un_b.s_b3, LocalAddress.S_un.S_un_b.s_b4);		
		OSCIP =  &szLocaIPAddress[0];
	}

	// OSC Port
	if(argc>2){
		OSCPort = atoi(argv[2]);
	}

	// verbose
	if(argc>4){
		verbose = atoi(argv[3]);
		verbose = (verbose != 0) ? 1 : 0;
	}

	// legacy
	if(argc>4){
		legacy = atoi(argv[4]);
		legacy = (legacy != 0) ? 1 : 0;
	}

	//EU - init UDP socket
	transmitSocket = new UdpTransmitSocket(IpEndpointName(OSCIP, OSCPort));

	if(transmitSocket){
		printf("OSC Forwarding IP: %s\n", OSCIP);
		printf("OSC Forwarding Port: %d\n\n", OSCPort);

		printf("Ready to stream some OSC...\n\n");
	}

    printf("Packet Client started\n\n");
    printf("Commands:\ns\tsend data descriptions\nf\tsend frame of data\nt\tsend test request\nq\tquit\n\n");
    int c;
    char szRequest[512];
    bool bExit = false;
    nTries = 3;
    while (!bExit)
    {
        c =_getch();
        switch(c)
        {
        case 's':
            // send NAT_REQUEST_MODELDEF command to server (will respond on the "Command Listener" thread)
            PacketOut.iMessage = NAT_REQUEST_MODELDEF;
            PacketOut.nDataBytes = 0;
            nTries = 3;
            while (nTries--)
            {
                int iRet = sendto(CommandSocket, (char *)&PacketOut, 4 + PacketOut.nDataBytes, 0, (sockaddr *)&HostAddr, sizeof(HostAddr));
                if(iRet != SOCKET_ERROR)
                    break;
            }
            break;	
        case 'f':
            // send NAT_REQUEST_FRAMEOFDATA (will respond on the "Command Listener" thread)
            PacketOut.iMessage = NAT_REQUEST_FRAMEOFDATA;
            PacketOut.nDataBytes = 0;
            nTries = 3;
            while (nTries--)
            {
                int iRet = sendto(CommandSocket, (char *)&PacketOut, 4 + PacketOut.nDataBytes, 0, (sockaddr *)&HostAddr, sizeof(HostAddr));
                if(iRet != SOCKET_ERROR)
                    break;
            }
            break;	
        case 't':
            // send NAT_MESSAGESTRING (will respond on the "Command Listener" thread)
            strcpy(szRequest, "TestRequest");
            PacketOut.iMessage = NAT_REQUEST;
            PacketOut.nDataBytes = (int)strlen(szRequest) + 1;
            strcpy(PacketOut.Data.szData, szRequest);
            nTries = 3;
            while (nTries--)
            {
                int iRet = sendto(CommandSocket, (char *)&PacketOut, 4 + PacketOut.nDataBytes, 0, (sockaddr *)&HostAddr, sizeof(HostAddr));
                if(iRet != SOCKET_ERROR)
                    break;
            }
            break;	
        case 'p':
            // send NAT_MESSAGESTRING (will respond on the "Command Listener" thread)
            strcpy(szRequest, "Ping");
            PacketOut.iMessage = NAT_PING;
            PacketOut.nDataBytes = (int)strlen(szRequest) + 1;
            strcpy(PacketOut.Data.szData, szRequest);
            nTries = 3;
            while (nTries--)
            {
                int iRet = sendto(CommandSocket, (char *)&PacketOut, 4 + PacketOut.nDataBytes, 0, (sockaddr *)&HostAddr, sizeof(HostAddr));
                if(iRet != SOCKET_ERROR)
                    break;
            }
            break;	
        case 'q':
            bExit = true;		
            break;	
        default:
            break;
        }
    }

    return 0;
}


// convert ipp address string to addr
bool IPAddress_StringToAddr(char *szNameOrAddress, struct in_addr *Address)
{
	int retVal;
	struct sockaddr_in saGNI;
	char hostName[256];
	char servInfo[256];
	u_short port;
	port = 0;

	// Set up sockaddr_in structure which is passed to the getnameinfo function
	saGNI.sin_family = AF_INET;
	saGNI.sin_addr.s_addr = inet_addr(szNameOrAddress);
	saGNI.sin_port = htons(port);

	// getnameinfo
	if ((retVal = getnameinfo((SOCKADDR *)&saGNI, sizeof(sockaddr), hostName, 256, servInfo, 256, NI_NUMERICSERV)) != 0)
	{
        printf("[NatNet2OSC] GetHostByAddr failed. Error #: %ld\n", WSAGetLastError());
		return false;
	}

    Address->S_un.S_addr = saGNI.sin_addr.S_un.S_addr;
	
    return true;
}

// get ip addresses on local host
int GetLocalIPAddresses(unsigned long Addresses[], int nMax)
{
    unsigned long  NameLength = 128;
    char szMyName[1024];
    struct addrinfo aiHints;
	struct addrinfo *aiList = NULL;
    struct sockaddr_in addr;
    int retVal = 0;
    char* port = "0";
    
    if(GetComputerName(szMyName, &NameLength) != TRUE)
    {
        printf("[NatNet2OSC] get computer name  failed. Error #: %ld\n", WSAGetLastError());
        return 0;       
    };

	memset(&aiHints, 0, sizeof(aiHints));
	aiHints.ai_family = AF_INET;
	aiHints.ai_socktype = SOCK_DGRAM;
	aiHints.ai_protocol = IPPROTO_UDP;
	if ((retVal = getaddrinfo(szMyName, port, &aiHints, &aiList)) != 0) 
	{
        printf("[NatNet2OSC] getaddrinfo failed. Error #: %ld\n", WSAGetLastError());
        return 0;
	}

    memcpy(&addr, aiList->ai_addr, aiList->ai_addrlen);
    freeaddrinfo(aiList);
    Addresses[0] = addr.sin_addr.S_un.S_addr;

    return 1;
}

bool DecodeTimecode(unsigned int inTimecode, unsigned int inTimecodeSubframe, int* hour, int* minute, int* second, int* frame, int* subframe)
{
	bool bValid = true;

	*hour = (inTimecode>>24)&255;
	*minute = (inTimecode>>16)&255;
	*second = (inTimecode>>8)&255;
	*frame = inTimecode&255;
	*subframe = inTimecodeSubframe;

	return bValid;
}

bool TimecodeStringify(unsigned int inTimecode, unsigned int inTimecodeSubframe, char *Buffer, int BufferSize)
{
	bool bValid;
	int hour, minute, second, frame, subframe;
	bValid = DecodeTimecode(inTimecode, inTimecodeSubframe, &hour, &minute, &second, &frame, &subframe);

	sprintf_s(Buffer,BufferSize,"%2d:%2d:%2d:%2d.%d",hour, minute, second, frame, subframe);
	for(unsigned int i=0; i<strlen(Buffer); i++)
		if(Buffer[i]==' ')
			Buffer[i]='0';

	return bValid;
}


void Unpack(char* pData)
{
    int major = NatNetVersion[0];
    int minor = NatNetVersion[1];

 	char* ns;
	char *ptr = pData;

	ns = _getOSCTimeStamp();
	// new for OSC
	char* ts = _getOSCTimeStamp();
	long timestamp = atoi(ts);
	char buffer[(sizeof(sFrameOfMocapData))];
	osc::OutboundPacketStream p(buffer, sizeof(sFrameOfMocapData));
	
	p << osc::BeginBundleImmediate;

    if(verbose) printf("Begin Packet\n-------\n");

    // message ID
    int MessageID = 0;
    memcpy(&MessageID, ptr, 2); ptr += 2;
	if(verbose) printf("Message ID : %d\n", MessageID);

    // size
    int nBytes = 0;
    memcpy(&nBytes, ptr, 2); ptr += 2;
	if(verbose) printf("Byte count : %d\n", nBytes);
	
    if(MessageID == 7)      // FRAME OF MOCAP DATA packet
    {
		p << osc::BeginMessage("/frame/start");
		p << osc::EndMessage;

		p << osc::BeginMessage("/frame/timestamp");
		p << timestamp;
		p << osc::EndMessage;


        // frame number
        int frameNumber = 0; memcpy(&frameNumber, ptr, 4); ptr += 4;
        if(verbose) printf("Frame # : %d\n", frameNumber);

		p << osc::BeginMessage("/frame/number");
		p << frameNumber;
		p << osc::EndMessage;
    	
	    // number of data sets (markersets, rigidbodies, etc)
        int nMarkerSets = 0; memcpy(&nMarkerSets, ptr, 4); ptr += 4;
		if(verbose) printf("Marker Set Count : %d\n", nMarkerSets);

		p << osc::BeginMessage("/markerset/count");
		p << nMarkerSets;
		p << osc::EndMessage;

        for (int i=0; i < nMarkerSets; i++)
        {    
            // Markerset name
            char szName[256];
            strcpy_s(szName, ptr);
            int nDataBytes = (int) strlen(szName) + 1;
            ptr += nDataBytes;
            if(verbose) printf("Model Name: %s\n", szName);

			//sprintf(ns,"/markerset/%d/name", i);
			//p << osc::BeginMessage(ns);
			//p << szName;
			//p << osc::EndMessage;

        	// marker data
            int nMarkers = 0; memcpy(&nMarkers, ptr, 4); ptr += 4;
            if(verbose) printf("Marker Count : %d\n", nMarkers);

			//sprintf(ns,"/markerset/%d/count", i);
			//p << osc::BeginMessage(ns);
			//p << nMarkers;
			//p << osc::EndMessage;

            for(int j=0; j < nMarkers; j++)
            {
                float x = 0; memcpy(&x, ptr, 4); ptr += 4;
                float y = 0; memcpy(&y, ptr, 4); ptr += 4;
                float z = 0; memcpy(&z, ptr, 4); ptr += 4;
                if(verbose) printf("\tMarker %d : [x=%3.2f,y=%3.2f,z=%3.2f]\n",j,x,y,z);
					
				//sprintf(ns,"/markerset/%d/marker/%d/position", i, j);

				p << osc::BeginMessage("/markerset");
				p << szName;
				p << nMarkers;
				p << j;
				p << x; 
				p << y;
				p << z;
				p << osc::EndMessage;
            }
        }

	    // unidentified markers
        int nOtherMarkers = 0; memcpy(&nOtherMarkers, ptr, 4); ptr += 4;
        if(verbose) printf("Unidentified Marker Count : %d\n", nOtherMarkers);
		
		p << osc::BeginMessage("/othermarker/count");
		p << nOtherMarkers;
		p << osc::EndMessage;

        for(int j=0; j < nOtherMarkers; j++)
        {
            float x = 0.0f; memcpy(&x, ptr, 4); ptr += 4;
            float y = 0.0f; memcpy(&y, ptr, 4); ptr += 4;
            float z = 0.0f; memcpy(&z, ptr, 4); ptr += 4;
			if(verbose) printf("\tMarker %d : pos = [%3.3f,%3.3f,%3.3f]\n",j,x,y,z);
			
			p << osc::BeginMessage("/othermarker/position");
			p << j;
			p << x;
			p << y;
			p << z;
			p << osc::EndMessage;
         }
        
        // rigid bodies
        int nRigidBodies = 0;
        memcpy(&nRigidBodies, ptr, 4); ptr += 4;
        if(verbose) printf("Rigid Body Count : %d\n", nRigidBodies);

		p << osc::BeginMessage("/rigidbody/count");
		p << nRigidBodies;
		p << osc::EndMessage;

        for (int j=0; j < nRigidBodies; j++)
        {
            // rigid body pos/ori
            int ID = 0; memcpy(&ID, ptr, 4); ptr += 4;
            float x = 0.0f; memcpy(&x, ptr, 4); ptr += 4;
            float y = 0.0f; memcpy(&y, ptr, 4); ptr += 4;
            float z = 0.0f; memcpy(&z, ptr, 4); ptr += 4;
            float qx = 0; memcpy(&qx, ptr, 4); ptr += 4;
            float qy = 0; memcpy(&qy, ptr, 4); ptr += 4;
            float qz = 0; memcpy(&qz, ptr, 4); ptr += 4;
            float qw = 0; memcpy(&qw, ptr, 4); ptr += 4;
 			if(verbose){
				printf("ID : %d\n", ID);
				printf("pos: [%3.3f,%3.3f,%3.3f]\n", x,y,z);
				printf("ori: [%3.3f,%3.3f,%3.3f,%3.3f]\n", qx,qy,qz,qw);
			}

			p << osc::BeginMessage("/rigidbody/transform");
			p << ID;
			p << x;
			p << y;
			p << z;
			p << qx;
			p << qy;
			p << qz;
			p << qw;
			p << osc::EndMessage;

            // associated marker positions
            int nRigidMarkers = 0;  memcpy(&nRigidMarkers, ptr, 4); ptr += 4;
            if(verbose)printf("Marker Count: %d\n", nRigidMarkers);

            int nBytes = nRigidMarkers*3*sizeof(float);
            float* markerData = (float*)malloc(nBytes);
            memcpy(markerData, ptr, nBytes);
            ptr += nBytes;
            
            if(major >= 2)
            {
                // associated marker IDs
                nBytes = nRigidMarkers*sizeof(int);
                int* markerIDs = (int*)malloc(nBytes);
                memcpy(markerIDs, ptr, nBytes);
                ptr += nBytes;

                // associated marker sizes
                nBytes = nRigidMarkers*sizeof(float);
                float* markerSizes = (float*)malloc(nBytes);
                memcpy(markerSizes, ptr, nBytes);
                ptr += nBytes;

                // 2.6 and later
                if( ((major == 2)&&(minor >= 6)) || (major > 2) || (major == 0) ) 
                {
                    // params
                    short params = 0; memcpy(&params, ptr, 2); ptr += 2;
                    bool bTrackingValid = params & 0x01; // 0x01 : rigid body was successfully tracked in this frame
                }

                for(int k=0; k < nRigidMarkers; k++)
                {
                    if(verbose) printf("\tMarker %d: id=%d\tsize=%3.1f\tpos=[%3.3f,%3.3f,%3.3f]\n", k, markerIDs[k], markerSizes[k], markerData[k*3], markerData[k*3+1],markerData[k*3+2]);
 
					p << osc::BeginMessage("/rigidbody/marker/position");		
					p << ID;
					p << nRigidMarkers;
					p << k;
					p << markerData[k*3];
					p << markerData[k*3+1];
					p << markerData[k*3+2];
					p << osc::EndMessage;
               }

                if(markerIDs)
                    free(markerIDs);
                if(markerSizes)
                    free(markerSizes);

            }
            else
            {
                for(int k=0; k < nRigidMarkers; k++)
                {
					if(verbose) printf("\tMarker %d: pos = [%3.2f,%3.2f,%3.2f]\n", k, markerData[k*3], markerData[k*3+1],markerData[k*3+2]);
 					
					p << osc::BeginMessage("/rigidbody/marker/position");		
					p << ID;
					p << nRigidMarkers;
					p << k;
					p << markerData[k*3];
					p << markerData[k*3+1];
					p << markerData[k*3+2];
					p << osc::EndMessage;
              }
            }
            if(markerData)
                free(markerData);

            if(major >= 2)
            {
                // Mean marker error
                float fError = 0.0f; memcpy(&fError, ptr, 4); ptr += 4;
                if(verbose) printf("Mean marker error: %3.2f\n", fError);

				p << osc::BeginMessage("/rigidbody/meanerror");
				p << ID;
				p << fError;
				p << osc::EndMessage;
            }

            
	    } // next rigid body


        // skeletons (version 2.1 and later)
        if( ((major == 2)&&(minor>0)) || (major>2))
        {
            int nSkeletons = 0;
            memcpy(&nSkeletons, ptr, 4); ptr += 4;
            if(verbose) printf("Skeleton Count : %d\n", nSkeletons);

			p << osc::BeginMessage("/skeleton/count");
			p << nSkeletons;
			p << osc::EndMessage;

            for (int j=0; j < nSkeletons; j++)
            {
                // skeleton id
                int skeletonID = 0;
                memcpy(&skeletonID, ptr, 4); ptr += 4;
                // # of rigid bodies (bones) in skeleton
                int nRigidBodies = 0;
                memcpy(&nRigidBodies, ptr, 4); ptr += 4;
                if(verbose) printf("Rigid Body Count : %d\n", nRigidBodies);

				sprintf(ns,"/skeleton/%d/rigidbody/count", j);
				p << osc::BeginMessage(ns);
				p << nRigidBodies;
				p << osc::EndMessage;

                for (int i=0; i < nRigidBodies; i++)
                {
                    // rigid body pos/ori
                    int ID = 0; memcpy(&ID, ptr, 4); ptr += 4;
                    float x = 0.0f; memcpy(&x, ptr, 4); ptr += 4;
                    float y = 0.0f; memcpy(&y, ptr, 4); ptr += 4;
                    float z = 0.0f; memcpy(&z, ptr, 4); ptr += 4;
                    float qx = 0; memcpy(&qx, ptr, 4); ptr += 4;
                    float qy = 0; memcpy(&qy, ptr, 4); ptr += 4;
                    float qz = 0; memcpy(&qz, ptr, 4); ptr += 4;
                    float qw = 0; memcpy(&qw, ptr, 4); ptr += 4;

					if(verbose){
	                    printf("ID : %d\n", ID);
		                printf("pos: [%3.2f,%3.2f,%3.2f]\n", x,y,z);
			            printf("ori: [%3.2f,%3.2f,%3.2f,%3.2f]\n", qx,qy,qz,qw);
					}

					sprintf(ns,"/skeleton/%d/rigidbody/%d/ID",j, i);
					p << osc::BeginMessage(ns);
					p << ID;
					p << osc::EndMessage;

					sprintf(ns,"/skeleton/%d/rigidbody/%d/position",j, i);
					p << osc::BeginMessage(ns);
					p << x;
					p << y;
					p << z;
					p << qx;
					p << qy;
					p << qz;
					p << qw;
					p << osc::EndMessage;


                    // associated marker positions
                    int nRigidMarkers = 0;  memcpy(&nRigidMarkers, ptr, 4); ptr += 4;
                    if(verbose) printf("Marker Count: %d\n", nRigidMarkers);

					sprintf(ns,"/skeleton/%d/rigidbody/%d/marker/count", j, i);
					p << osc::BeginMessage(ns);
					p << nRigidMarkers;
					p << osc::EndMessage;

                    int nBytes = nRigidMarkers*3*sizeof(float);
                    float* markerData = (float*)malloc(nBytes);
                    memcpy(markerData, ptr, nBytes);
                    ptr += nBytes;

                    // associated marker IDs
                    nBytes = nRigidMarkers*sizeof(int);
                    int* markerIDs = (int*)malloc(nBytes);
                    memcpy(markerIDs, ptr, nBytes);
                    ptr += nBytes;

                    // associated marker sizes
                    nBytes = nRigidMarkers*sizeof(float);
                    float* markerSizes = (float*)malloc(nBytes);
                    memcpy(markerSizes, ptr, nBytes);
                    ptr += nBytes;

                    for(int k=0; k < nRigidMarkers; k++)
                    {
                        if(verbose) printf("\tMarker %d: id=%d\tsize=%3.1f\tpos=[%3.2f,%3.2f,%3.2f]\n", k, markerIDs[k], markerSizes[k], markerData[k*3], markerData[k*3+1],markerData[k*3+2]);
 
						sprintf(ns,"/skeleton/%d/rigidbody/%d/marker/%d/ID",j, i, k);
						p << osc::BeginMessage(ns);
						p << markerIDs[k];
						p << osc::EndMessage;

						sprintf(ns,"/skeleton/%d/rigidbody/%d/marker/%d/position",j, i, k);
						p << osc::BeginMessage(ns);			
						p << markerData[k*3];
						p << markerData[k*3+1];
						p << markerData[k*3+2];
						p << osc::EndMessage;
					
					}

                    // Mean marker error
                    float fError = 0.0f; memcpy(&fError, ptr, 4); ptr += 4;
                    if(verbose) printf("Mean marker error: %3.2f\n", fError);

					sprintf(ns,"/skeleton/%d/rigidbody/%d/meanerror", j, i);
					p << osc::BeginMessage(ns);
					p << fError;
					p << osc::EndMessage;

                    // release resources
                    if(markerIDs)
                        free(markerIDs);
                    if(markerSizes)
                        free(markerSizes);
                    if(markerData)
                        free(markerData);

                } // next rigid body

            } // next skeleton
        }
        
		// labeled markers (version 2.3 and later)
		if( ((major == 2)&&(minor>=3)) || (major>2))
		{
			int nLabeledMarkers = 0;
			memcpy(&nLabeledMarkers, ptr, 4); ptr += 4;
			if(verbose) printf("Labeled Marker Count : %d\n", nLabeledMarkers);

			p << osc::BeginMessage("/labeledmarker/count");
			p << nLabeledMarkers;
			p << osc::EndMessage;

			for (int j=0; j < nLabeledMarkers; j++)
			{
				// id
				int ID = 0; memcpy(&ID, ptr, 4); ptr += 4;
				// x
				float x = 0.0f; memcpy(&x, ptr, 4); ptr += 4;
				// y
				float y = 0.0f; memcpy(&y, ptr, 4); ptr += 4;
				// z
				float z = 0.0f; memcpy(&z, ptr, 4); ptr += 4;
				// size
				float size = 0.0f; memcpy(&size, ptr, 4); ptr += 4;

                // 2.6 and later
                if( ((major == 2)&&(minor >= 6)) || (major > 2) || (major == 0) ) 
                {
                    // marker params
                    short params = 0; memcpy(&params, ptr, 2); ptr += 2;
                    bool bOccluded = params & 0x01;     // marker was not visible (occluded) in this frame
                    bool bPCSolved = params & 0x02;     // position provided by point cloud solve
                    bool bModelSolved = params & 0x04;  // position provided by model solve
                }

				if(verbose){
					printf("ID  : %d\n", ID);
					printf("pos : [%3.2f,%3.2f,%3.2f]\n", x,y,z);
					printf("size: [%3.2f]\n", size);
				}

				sprintf(ns,"/labeledmarker/%d/position",ID);
				p << osc::BeginMessage(ns);
				p << x;
				p << y;
				p << z;
				p << osc::EndMessage;

				sprintf(ns,"/labeledmarker/%d/size",ID);
				p << osc::BeginMessage(ns);
				p << size;
				p << osc::EndMessage;
			}
		}

		// latency
        float latency = 0.0f; memcpy(&latency, ptr, 4);	ptr += 4;
		if(verbose) printf("latency : %3.3f\n", latency);

		p << osc::BeginMessage("/frame/latency");
		p << latency;
		p << osc::EndMessage;

		// timecode
		unsigned int timecode = 0; 	memcpy(&timecode, ptr, 4);	ptr += 4;
		unsigned int timecodeSub = 0; memcpy(&timecodeSub, ptr, 4); ptr += 4;
		char szTimecode[128] = "";
		TimecodeStringify(timecode, timecodeSub, szTimecode, 128);

        // timestamp
        float timestamp = 0.0f;  memcpy(&timestamp, ptr, 4); ptr += 4;

        // frame params
        short params = 0;  memcpy(&params, ptr, 2); ptr += 2;
        bool bIsRecording = params & 0x01;                  // 0x01 Motive is recording
        bool bTrackedModelsChanged = params & 0x02;         // 0x02 Actively tracked model list has changed

		// end of data tag
        int eod = 0; memcpy(&eod, ptr, 4); ptr += 4;

        if(verbose) printf("End Packet\n-------------\n");

		p << osc::BeginMessage("/frame/end");
		p << osc::EndMessage;

		transmitSocket->Send(p.Data(), p.Size());
 

    }
    else if(MessageID == 5) // Data Descriptions
    {
		p << osc::BeginMessage("/dataset/start");
		p << osc::EndMessage;

		p << osc::BeginMessage("/dataset/timestamp");
		p << timestamp;
		p << osc::EndMessage;

        // number of datasets
        int nDatasets = 0; memcpy(&nDatasets, ptr, 4); ptr += 4;
		if(verbose) printf("Dataset Count : %d\n", nDatasets);

		p << osc::BeginMessage("/dataset/count");
		p << nDatasets;
		p << osc::EndMessage;

        for(int i=0; i < nDatasets; i++)
        {
 
            int type = 0; memcpy(&type, ptr, 4); ptr += 4;

			if(verbose){
				printf("Dataset %d\n", i);
				printf("Type : %d\n", i, type);
			}

			sprintf(ns,"/dataset/%d/type", i);
			p << osc::BeginMessage(ns);
			p << type;
			p << osc::EndMessage;

            if(type == 0)   // markerset
            {
                // name
                char szName[256];
                strcpy_s(szName, ptr);
                int nDataBytes = (int) strlen(szName) + 1;
                ptr += nDataBytes;

                if(verbose) printf("Markerset Name: %s\n", szName);

				sprintf(ns,"/dataset/%d/markerset/name", i);
				p << osc::BeginMessage(ns);
				p << szName;
				p << osc::EndMessage;

        	    // marker data
                int nMarkers = 0; memcpy(&nMarkers, ptr, 4); ptr += 4;
                
				if(verbose) printf("Marker Count : %d\n", nMarkers);
				
				sprintf(ns,"/dataset/%d/markerset/%s/count", i, szName);
				p << osc::BeginMessage(ns);
				p << nMarkers;
				p << osc::EndMessage;

                for(int j=0; j < nMarkers; j++)
                {
                    char szName2[256];
                    strcpy_s(szName2, ptr);
                    int nDataBytes = (int) strlen(szName) + 1;
                    ptr += nDataBytes;
                    
					if(verbose) printf("Marker Name: %s\n", szName2);
					
					sprintf(ns,"/dataset/%d/markerset/%s/marker/%d/name",i, szName, j);					
					p << osc::BeginMessage(ns);
					p << szName2;
					p << osc::EndMessage;

				}
            }
            else if(type ==1)   // rigid body
            {
                if(major >= 2)
                {
                    // name
                    char szName[MAX_NAMELENGTH];
                    strcpy(szName, ptr);
                    ptr += strlen(ptr) + 1;
                    
					if(verbose) printf("Name: %s\n", szName);

					sprintf(ns,"/dataset/%d/rigidbody/name", i);
					p << osc::BeginMessage(ns);
					p << szName;
					p << osc::EndMessage;
               }

                int ID = 0; memcpy(&ID, ptr, 4); ptr +=4;         
                int parentID = 0; memcpy(&parentID, ptr, 4); ptr +=4;
 
				if(verbose){
					printf("ID : %d\n", ID);
					printf("Parent ID : %d\n", parentID);
				}
 
				sprintf(ns,"/dataset/%d/rigidbody/ID", i);
				p << osc::BeginMessage(ns);
				p << ID;
				p << osc::EndMessage;
				sprintf(ns,"/dataset/%d/rigidbody/%d/parentID", i, ID);
				p << osc::BeginMessage(ns);
				p << parentID;
				p << osc::EndMessage;

                float xoffset = 0; memcpy(&xoffset, ptr, 4); ptr +=4;
                float yoffset = 0; memcpy(&yoffset, ptr, 4); ptr +=4;
				float zoffset = 0; memcpy(&zoffset, ptr, 4); ptr +=4;

 				if(verbose){
					printf("X Offset : %3.2f\n", xoffset);
					printf("Y Offset : %3.2f\n", yoffset);
					printf("Z Offset : %3.2f\n", zoffset);
				}

				sprintf(ns,"/dataset/%d/rigidbody/%d/offset", i, ID);
				p << osc::BeginMessage(ns);
				p << xoffset;
				p << yoffset;
				p << zoffset;
				p << osc::EndMessage;

            }
            else if(type ==2)   // skeleton
            {
                char szName[MAX_NAMELENGTH];
                strcpy(szName, ptr);
                ptr += strlen(ptr) + 1;
                if(verbose) printf("Name: %s\n", szName);

				sprintf(ns,"/dataset/%d/skeleton/name", i);
				p << osc::BeginMessage(ns);
				p << szName;
				p << osc::EndMessage;

                int skeletonID = 0; memcpy(&skeletonID, ptr, 4); ptr +=4;
                if(verbose) printf("ID : %d\n", skeletonID);

				sprintf(ns,"/dataset/%d/skeleton/ID", i);
				p << osc::BeginMessage(ns);
				p << skeletonID;
				p << osc::EndMessage;

                int nRigidBodies = 0; memcpy(&nRigidBodies, ptr, 4); ptr +=4;
                if(verbose) printf("RigidBody (Bone) Count : %d\n", nRigidBodies);

				sprintf(ns,"/dataset/%d/skeleton/%d/rigidbody/count", i, skeletonID);
				p << osc::BeginMessage(ns);
				p << nRigidBodies;
				p << osc::EndMessage;

                for(int j=0; j< nRigidBodies; j++)
                {
                    if(major >= 2)
                    {
                        // RB name
                        char szName[MAX_NAMELENGTH];
                        strcpy(szName, ptr);
                        ptr += strlen(ptr) + 1;
                        if(verbose) printf("Rigid Body Name: %s\n", szName);
 					
						sprintf(ns,"/dataset/%d/skeleton/%d/rigidbody/%d/name", i, skeletonID, j);
						p << osc::BeginMessage(ns);
						p << szName;
						p << osc::EndMessage;

					}

                    int ID = 0; memcpy(&ID, ptr, 4); ptr +=4;
                    if(verbose) printf("RigidBody ID : %d\n", ID);

					sprintf(ns,"/dataset/%d/skeleton/%d/rigidbody/%d/ID", i, skeletonID, j);
					p << osc::BeginMessage(ns);
					p << ID;
					p << osc::EndMessage;

                    int parentID = 0; memcpy(&parentID, ptr, 4); ptr +=4;
					if(verbose)  printf("Parent ID : %d\n", parentID);

					sprintf(ns,"/dataset/%d/skeleton/%d/rigidbody/%d/parentID", i, skeletonID, j);
					p << osc::BeginMessage(ns);
					p << parentID;
					p << osc::EndMessage;

                    float xoffset = 0; memcpy(&xoffset, ptr, 4); ptr +=4;
                    float yoffset = 0; memcpy(&yoffset, ptr, 4); ptr +=4;
                    float zoffset = 0; memcpy(&zoffset, ptr, 4); ptr +=4;
  					
					if(verbose) {
						printf("X Offset : %3.2f\n", xoffset);					
						printf("Y Offset : %3.2f\n", yoffset);
						printf("Z Offset : %3.2f\n", zoffset);
					}

					sprintf(ns,"/dataset/%d/skeleton/%d/rigidbody/%d/offset", i, skeletonID, j);
					p << osc::BeginMessage(ns);
					p << xoffset;
					p << yoffset;
					p << zoffset;
					p << osc::EndMessage;
              }
            }

        }   // next dataset

		if(verbose) printf("End Packet\n-------------\n");

		p << osc::BeginMessage("/dataset/end");
		p << osc::EndMessage;

		transmitSocket->Send(p.Data(), p.Size());
 
    }
    else
    {
        printf("Unrecognized Packet Type.\n");
    }


}

char* _getOSCTimeStamp(){

	SYSTEMTIME lt;
    
    GetLocalTime(&lt);

	unsigned int tmili = ((int)lt.wHour)*3600000 + ((int)lt.wMinute)*60000 + ((int)lt.wSecond)*1000 + ((int)lt.wMilliseconds);

	char* ts = (char*)malloc(100);
	sprintf(ts, "%d", tmili);

	return ts;
}
