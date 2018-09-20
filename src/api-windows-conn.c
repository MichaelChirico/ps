
// Fixes clash between winsock2.h and windows.h
#define WIN32_LEAN_AND_MEAN

#include "common.h"
#include "windows.h"
#include <winsock2.h>
#if (_WIN32_WINNT >= 0x0600) // Windows Vista and above
#include <ws2tcpip.h>
#endif
#include <wincrypt.h>
#include <iphlpapi.h>

#define BYTESWAP_USHORT(x) ((((USHORT)(x) << 8) | ((USHORT)(x) >> 8)) & 0xffff)
#ifndef AF_INET6
#define AF_INET6 23
#endif

typedef DWORD (WINAPI * _GetExtendedTcpTable)(PVOID, PDWORD, BOOL, ULONG,
                                              TCP_TABLE_CLASS, ULONG);

// https://msdn.microsoft.com/library/aa365928.aspx
static DWORD __GetExtendedTcpTable(_GetExtendedTcpTable call,
                                   ULONG address_family,
                                   PVOID * data, DWORD * size) {

  // Due to other processes being active on the machine, it's possible
  // that the size of the table increases between the moment where we
  // query the size and the moment where we query the data.  Therefore, it's
  // important to call this in a loop to retry if that happens.
  //
  // Also, since we may loop a theoretically unbounded number of times here,
  // release the GIL while we're doing this.
  DWORD error = ERROR_INSUFFICIENT_BUFFER;
  *size = 0;
  *data = NULL;
  error = call(NULL, size, FALSE, address_family,
	       TCP_TABLE_OWNER_PID_ALL, 0);
  while (error == ERROR_INSUFFICIENT_BUFFER) {
    *data = malloc(*size);
    if (*data == NULL) {
      error = ERROR_NOT_ENOUGH_MEMORY;
      continue;
    }
    error = call(*data, size, FALSE, address_family,
		 TCP_TABLE_OWNER_PID_ALL, 0);
    if (error != NO_ERROR) {
      free(*data);
      *data = NULL;
    }
  }
  return error;
}

typedef DWORD (WINAPI * _GetExtendedUdpTable)(PVOID, PDWORD, BOOL, ULONG,
                                              UDP_TABLE_CLASS, ULONG);

// https://msdn.microsoft.com/library/aa365930.aspx
static DWORD __GetExtendedUdpTable(_GetExtendedUdpTable call,
                                   ULONG address_family,
                                   PVOID * data, DWORD * size) {

  // Due to other processes being active on the machine, it's possible
  // that the size of the table increases between the moment where we
  // query the size and the moment where we query the data.  Therefore, it's
  // important to call this in a loop to retry if that happens.
  //
  // Also, since we may loop a theoretically unbounded number of times here,
  // release the GIL while we're doing this.
  DWORD error = ERROR_INSUFFICIENT_BUFFER;
  *size = 0;
  *data = NULL;
  error = call(NULL, size, FALSE, address_family,
	       UDP_TABLE_OWNER_PID, 0);
  while (error == ERROR_INSUFFICIENT_BUFFER) {
    *data = malloc(*size);
    if (*data == NULL) {
      error = ERROR_NOT_ENOUGH_MEMORY;
      continue;
    }
    error = call(*data, size, FALSE, address_family,
		 UDP_TABLE_OWNER_PID, 0);
    if (error != NO_ERROR) {
      free(*data);
      *data = NULL;
    }
  }
  return error;
}

SEXP psll_connections(SEXP p) {

  static long null_address[4] = { 0, 0, 0, 0 };
  unsigned long pid;
  int pid_return;
  typedef PSTR (NTAPI * _RtlIpv4AddressToStringA)(struct in_addr *, PSTR);
  _RtlIpv4AddressToStringA rtlIpv4AddressToStringA;
  typedef PSTR (NTAPI * _RtlIpv6AddressToStringA)(struct in6_addr *, PSTR);
  _RtlIpv6AddressToStringA rtlIpv6AddressToStringA;
  _GetExtendedTcpTable getExtendedTcpTable;
  _GetExtendedUdpTable getExtendedUdpTable;
  PVOID table = NULL;
  DWORD tableSize;
  DWORD err;
  PMIB_TCPTABLE_OWNER_PID tcp4Table;
  PMIB_UDPTABLE_OWNER_PID udp4Table;
  PMIB_TCP6TABLE_OWNER_PID tcp6Table;
  PMIB_UDP6TABLE_OWNER_PID udp6Table;
  ULONG i;
  CHAR addressBufferLocal[65];
  CHAR addressBufferRemote[65];

  SEXP retlist;
  PROTECT_INDEX ret_idx;
  int ret_len = 10, ret_num = -1;

  SEXP conn;
  char *addr_local = NULL, *addr_remote = NULL;
  int port_local = 0, port_remote = 0;

  ps_handle_t *handle = R_ExternalPtrAddr(p);
  if (!handle) error("Process pointer cleaned up already");
  pid = handle->pid;

  // Import some functions.
  {
    HMODULE ntdll;
    HMODULE iphlpapi;

    ntdll = LoadLibrary(TEXT("ntdll.dll"));
    rtlIpv4AddressToStringA = (_RtlIpv4AddressToStringA)GetProcAddress(
      ntdll, "RtlIpv4AddressToStringA");
    rtlIpv6AddressToStringA = (_RtlIpv6AddressToStringA)GetProcAddress(
      ntdll, "RtlIpv6AddressToStringA");
    /* TODO: Check these two function pointers */

    iphlpapi = LoadLibrary(TEXT("iphlpapi.dll"));
    getExtendedTcpTable = (_GetExtendedTcpTable)GetProcAddress(iphlpapi,
      "GetExtendedTcpTable");
    getExtendedUdpTable = (_GetExtendedUdpTable)GetProcAddress(iphlpapi,
      "GetExtendedUdpTable");
    FreeLibrary(ntdll);
    FreeLibrary(iphlpapi);
  }

  if ((getExtendedTcpTable == NULL) || (getExtendedUdpTable == NULL)) {
    ps__not_implemented("ps_connections");
    ps__throw_error();
  }

  PROTECT_WITH_INDEX(retlist = allocVector(VECSXP, ret_len), &ret_idx);

  // TCP IPv4

  table = NULL;
  conn = R_NilValue;
  addr_local = 0;
  addr_remote = 0;
  port_local = 0;
  port_remote = 0;
  tableSize = 0;

  err = __GetExtendedTcpTable(getExtendedTcpTable,
			      AF_INET, &table, &tableSize);
  if (err == ERROR_NOT_ENOUGH_MEMORY) {
    ps__no_memory("");
    ps__throw_error();
  }

  if (err == NO_ERROR) {
    tcp4Table = table;

    for (i = 0; i < tcp4Table->dwNumEntries; i++) {

      if (tcp4Table->table[i].dwOwningPid != pid) continue;

      if (tcp4Table->table[i].dwLocalAddr != 0 ||
	  tcp4Table->table[i].dwLocalPort != 0) {
	struct in_addr addr;

	addr.S_un.S_addr = tcp4Table->table[i].dwLocalAddr;
	rtlIpv4AddressToStringA(&addr, addressBufferLocal);
	addr_local = addressBufferLocal;
	port_local = BYTESWAP_USHORT(tcp4Table->table[i].dwLocalPort);
      }

      // On Windows <= XP, remote addr is filled even if socket
      // is in LISTEN mode in which case we just ignore it.
      if ((tcp4Table->table[i].dwRemoteAddr != 0 ||
	   tcp4Table->table[i].dwRemotePort != 0) &&
	  (tcp4Table->table[i].dwState != MIB_TCP_STATE_LISTEN)) {
	struct in_addr addr;

	addr.S_un.S_addr = tcp4Table->table[i].dwRemoteAddr;
	rtlIpv4AddressToStringA(&addr, addressBufferRemote);
	addr_remote = addressBufferRemote;
	port_remote = BYTESWAP_USHORT(tcp4Table->table[i].dwRemotePort);
      }

      PROTECT(conn = ps__build_list("iiisisii",
        NA_INTEGER, AF_INET, SOCK_STREAM, addr_local, port_local, addr_remote,
        port_remote, tcp4Table->table[i].dwState));

      if (++ret_num == ret_len) {
	ret_len *= 2;
	REPROTECT(retlist = Rf_lengthgets(retlist, ret_len), ret_idx);
      }
      SET_VECTOR_ELT(retlist, ret_num, conn);
      UNPROTECT(1);
    }
  } else {
    ps__set_error_from_windows_error(err);
    free(table);
    ps__throw_error();
  }

  free(table);
  table = NULL;
  tableSize = 0;


  UNPROTECT(1);
  return retlist;
}
