#pragma once
#include <winsock2.h>

namespace udp_receive_policy
{
    // Windows UDP reports ICMP port-unreachable as WSAECONNRESET. QoS PC
    // 102447E0 tolerates only WSAEWOULDBLOCK; KisakCOD Sys_GetPacket treats
    // both as no packet. Preserve TCP resets and every other socket error.
    inline int normalize(int error, int socket_type)
    {
        return socket_type == SOCK_DGRAM && error == WSAECONNRESET
            ? WSAEWOULDBLOCK : error;
    }
}
