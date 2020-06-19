
/*
 *  Copyright (C) 2020 Mayco S. Berghetti
 *
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>           // malloc
#include <arpa/inet.h>        // htons
#include <errno.h>            // variable errno
#include <linux/if_ether.h>   // defined ETH_P_ALL
#include <linux/if_packet.h>  // struct sockaddr_ll
#include <net/if.h>           // if_nametoindex
#include <string.h>           // strerror
#include <sys/socket.h>       // socket
#include <sys/types.h>        // socket
#include <fcntl.h>            // fcntl
#include <unistd.h>           // close

#include "sock.h"
#include "m_error.h"

// defined in main.c
extern char *iface;

static void
socket_setnonblocking ( int sock );

static void
bind_interface ( int sock, const char *iface );

int
create_socket ( void )
{
  int sock;

  if ( ( sock = socket ( AF_PACKET, SOCK_RAW, htons ( ETH_P_ALL ) ) ) == -1 )
    fatal_error ( "Error create socket: %s", strerror ( errno ) );

  socket_setnonblocking ( sock );

  bind_interface ( sock, iface );

  return sock;
}

void
close_socket ( int sock )
{
  if ( sock > 0 )
    close ( sock );
}

static void
socket_setnonblocking ( int sock )
{
  int flag;

  if ( ( flag = fcntl ( sock, F_GETFL ) ) == -1 )
    fatal_error ( "Cannot get socket flags: \"%s\"", strerror ( errno ) );

  if ( fcntl ( sock, F_SETFL, flag | O_NONBLOCK ) == -1 )
    fatal_error ( "Cannot set socket to non-blocking mode: \"%s\"",
                  strerror ( errno ) );
}

static void
bind_interface ( int sock, const char *iface )
{
  struct sockaddr_ll my_sock = {0};
  my_sock.sll_family = AF_PACKET;
  my_sock.sll_protocol = htons ( ETH_P_ALL );

  // 0 match all interfaces
  if ( !iface )
    my_sock.sll_ifindex = 0;
  else
    {
      if ( !( my_sock.sll_ifindex = if_nametoindex ( iface ) ) )
        fatal_error ( "Interface: %s", strerror ( errno ) );
    }

  if ( bind ( sock, ( struct sockaddr * ) &my_sock, sizeof ( my_sock ) ) == -1 )
    fatal_error ( "Error bind interface %s", strerror ( errno ) );
}
