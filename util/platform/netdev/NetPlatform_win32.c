/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _WIN32_WINNT 0x0600
#include "exception/WinFail.h"
#include "util/platform/netdev/NetPlatform.h"
#include "util/Bits.h"
#include "util/platform/Sockaddr.h"

#include <winsock2.h>
#include <windows.h>

#define NET_LUID misalligned_NET_LUID
#define PNET_LUID misalligned_PNET_LUID
#define IF_LUID misalligned_IF_LUID
#define PIF_LUID misalligned_PIF_LUID
#include <ifdef.h>
#undef NET_LUID
#undef PNET_LUID
#undef IF_LUID
#undef PIF_LUID

// mingw-w64 incorrectly pragma pack's this to 1 byte.
typedef union NET_LUID {
  ULONG64 Value;
  __C89_NAMELESS struct { /* bitfield with 64 bit types. */
    ULONG64 Reserved  :24;
    ULONG64 NetLuidIndex  :24;
    ULONG64 IfType  :16;
  } Info;
} NET_LUID, *PNET_LUID;
Assert_compileTime(sizeof(NET_LUID) == 8);

typedef NET_LUID IF_LUID, *PIF_LUID;

#include <ws2ipdef.h>
#include <naptypes.h>
#include <ntddndis.h>

#include <ws2def.h>
#include <iprtrmib.h>
#include <ifdef.h>
#include <iphlpapi.h>

static NET_LUID getLuid(const char* name, struct Except* eh)
{
    uint16_t ifName[IF_MAX_STRING_SIZE + 1] = {0};
    WinFail_check(eh,
        (!MultiByteToWideChar(CP_UTF8, 0, name, CString_strlen(name), ifName, IF_MAX_STRING_SIZE + 1))
    );
    NET_LUID out;
    WinFail_check(eh, ConvertInterfaceAliasToLuid(ifName, &out));
    return out;
}

static LONG flushAddresses(NET_LUID luid, MIB_UNICASTIPADDRESS_TABLE* table)
{
    LONG out = NO_ERROR;
    for (int i = 0; i < (int)table->NumEntries; i++) {
        if (table->Table[i].InterfaceLuid.Value == luid.Value) {
            if ((out = DeleteUnicastIpAddressEntry(&table->Table[i]))) {
                return out;
            }
        }
    }
    return out;
}

void NetPlatform_flushAddresses(const char* deviceName, struct Except* eh)
{
    NET_LUID luid = getLuid(deviceName, eh);
    MIB_UNICASTIPADDRESS_TABLE* table;

    WinFail_check(eh, GetUnicastIpAddressTable(AF_INET, &table));
    LONG ret = flushAddresses(luid, table);
    FreeMibTable(table);
    if (ret) {
        WinFail_fail(eh, "DeleteUnicastIpAddressEntry(&table->Table[i])", ret);
    }

    WinFail_check(eh, GetUnicastIpAddressTable(AF_INET6, &table));
    ret = flushAddresses(luid, table);
    FreeMibTable(table);
    if (ret) {
        WinFail_fail(eh, "DeleteUnicastIpAddressEntry(&table->Table[i])", ret);
    }
}
#include "util/Hex.h"
#include <stdio.h>

static void setupRoute(const char* deviceName,
                       const uint8_t* addrBytes,
                       int prefixLen,
                       int addrFam,
                       struct Except* eh)
{
    void WINAPI InitializeIpForwardEntry(PMIB_IPFORWARD_ROW2 Row);

    MIB_IPFORWARD_ROW2 row = {
        .InterfaceLuid = getLuid(deviceName, eh),
        .ValidLifetime = WSA_INFINITE,
        .PreferredLifetime = WSA_INFINITE,
        .Metric = 0xffffffff,
        .Protocol = MIB_IPPROTO_NETMGMT,
        .SitePrefixLength = 255,

        .DestinationPrefix = {
            .PrefixLength = prefixLen,
            .Prefix = { .si_family = addrFam }
        },

        .NextHop = { .si_family = addrFam },

        .Loopback = false,
        .AutoconfigureAddress = false,
        .Immortal = false,
        .Age = 0,
        .Origin = 0
    };

    if (addrFam == AF_INET6) {
        Bits_memcpyConst(&row.DestinationPrefix.Prefix.Ipv6.sin6_addr, addrBytes, 15);
        row.DestinationPrefix.Prefix.Ipv6.sin6_family = AF_INET6;
        // set the gateway addr to the client's addr +1
        uint64_t addr[2];
        Bits_memcpyConst(addr, addrBytes, 16);
        addr[1] = Endian_hostToBigEndian64(Endian_bigEndianToHost64(addr[1]) + 1);
        if (!addr[1]) {
            addr[0] = Endian_hostToBigEndian64(Endian_bigEndianToHost64(addr[0]) + 1);
        }
//        Bits_memcpyConst(&row.NextHop.Ipv6.sin6_addr, addr, 16);
    } else {
        Bits_memcpyConst(&row.DestinationPrefix.Prefix.Ipv4.sin_addr, addrBytes, 4);
        row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
        uint32_t addr;
        Bits_memcpyConst(&addr, addrBytes, 4);
        addr = Endian_hostToBigEndian32(Endian_bigEndianToHost32(addr) + 1);
        Bits_memcpyConst(&row.NextHop.Ipv4.sin_addr, &addr, 4);
    }

    //InitializeIpForwardEntry(&row);
    uint8_t buff[sizeof(row) * 2 + 1];
    Hex_encode(buff, sizeof(buff), (uint8_t*) &row, sizeof(row));
    printf("%s %d\n", buff, row.SitePrefixLength);
//    Hex_encode(buff, sizeof(buff), (uint8_t*) &row, sizeof(row));
//    printf("%s %d<\n", buff, row.SitePrefixLength);

    WinFail_check(eh, CreateIpForwardEntry2(&row));
}

void NetPlatform_addAddress(const char* name,
                            const uint8_t* addrBytes,
                            int prefixLen,
                            int addrFam,
                            struct Log* logger,
                            struct Except* eh)
{
    MIB_UNICASTIPADDRESS_ROW ipRow = {
        .PrefixOrigin = IpPrefixOriginUnchanged,
        .SuffixOrigin = IpSuffixOriginUnchanged,
        .ValidLifetime = 0xFFFFFFFF,
        .PreferredLifetime = 0xFFFFFFFF,
        .OnLinkPrefixLength = 0xFF
    };

    ipRow.InterfaceLuid = getLuid(name, eh);

    ipRow.Address.si_family = addrFam;
    if (addrFam == Sockaddr_AF_INET6) {
        Bits_memcpyConst(&ipRow.Address.Ipv6.sin6_addr, addrBytes, 16);
    } else if (addrFam == Sockaddr_AF_INET) {
        Bits_memcpyConst(&ipRow.Address.Ipv4.sin_addr, addrBytes, 4);
    } else {
        Assert_true(0);
    }

    ipRow.OnLinkPrefixLength = prefixLen;

    WinFail_check(eh, CreateUnicastIpAddressEntry(&ipRow));
return;
    setupRoute(name, addrBytes, prefixLen, addrFam, eh);
}

void NetPlatform_setMTU(const char* interfaceName,
                        uint32_t mtu,
                        struct Log* logger,
                        struct Except* eh)
{
    Except_throw(eh, "unimplemented");
}
