/*******************************************
HostInfo internal include file
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_NETWORK__
#define __KINESIS_VIDEO_WEBRTC_CLIENT_NETWORK__

#pragma once

#ifdef  __cplusplus
extern "C" {
#endif

#define MAX_LOCAL_NETWORK_INTERFACE_COUNT               128

// string buffer size for ipv4 and ipv6. Null terminator included.
// for ipv6: 0000:0000:0000:0000:0000:0000:0000:0000 = 39
// for ipv4 mapped ipv6: 0000:0000:0000:0000:0000:ffff:192.168.100.228 = 45
#define KVS_IP_ADDRESS_STRING_BUFFER_LEN                46

// 000.000.000.000
#define KVS_MAX_IPV4_ADDRESS_STRING_LEN                 15

#define KVS_GET_IP_ADDRESS_PORT(a)                      ((UINT16) getInt16((a)->port))

#if defined(__MACH__)
#    define NO_SIGNAL SO_NOSIGPIPE
#else
#    define NO_SIGNAL MSG_NOSIGNAL
#endif

typedef enum {
    KVS_SOCKET_PROTOCOL_NONE,
    KVS_SOCKET_PROTOCOL_TCP,
    KVS_SOCKET_PROTOCOL_UDP,
} KVS_SOCKET_PROTOCOL;

/**
 * @param - PKvsIpAddress - IN/OUT - array for getLocalhostIpAddresses to store any local ips it found. The ip address and port
 *                                   will be in network byte order.
 * @param - UINT32 - IN/OUT - length of the array, upon return it will be updated to the actual number of ips in the array
 *
 *@param - IceSetInterfaceFilterFunc - IN - set to custom interface filter callback
 *
 *@param - UINT64 - IN - Set to custom data that can be used in the callback later
 * @return - STATUS status of execution
 */
STATUS getLocalhostIpAddresses(PKvsIpAddress, PUINT32, IceSetInterfaceFilterFunc, UINT64);

/**
 * @param - PKvsIpAddress - IN - Attempt to create an udp socket with the ip address given. Upon success, fill PKvsIpAddress'
 *                                     port field with the actual port number.
 * @param - PKvsIpAddress - IN - Peer ip address for tcp socket creation
 * @param - KVS_SOCKET_PROTOCOL - IN - either tcp or udp
 * @param - UINT32 - IN - send buffer size in bytes
 * @param - PINT32 - OUT - PINT32 for the socketfd
 *
 * @return - STATUS status of execution
 */
STATUS createSocket(PKvsIpAddress, PKvsIpAddress, KVS_SOCKET_PROTOCOL, UINT32, PINT32);

/**
 * @param - PCHAR - IN - hostname to resolve
 *
 * @param - PKvsIpAddress - OUT - resolved ip address
 *
 * @return - STATUS status of execution
 */
STATUS getIpWithHostName(PCHAR, PKvsIpAddress);

STATUS getIpAddrStr(PKvsIpAddress, PCHAR, UINT32);

BOOL isSameIpAddress(PKvsIpAddress, PKvsIpAddress, BOOL);

#ifdef  __cplusplus
}
#endif
#endif  /* __KINESIS_VIDEO_WEBRTC_CLIENT_NETWORK__ */
