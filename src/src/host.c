/* $Cambridge: exim/src/src/host.c,v 1.4 2004/12/29 10:16:53 ph10 Exp $ */

/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) University of Cambridge 1995 - 2004 */
/* See the file NOTICE for conditions of use and distribution. */

/* Functions for finding hosts, either by gethostbyname(), gethostbyaddr(), or
directly via the DNS. When IPv6 is supported, getipnodebyname() and
getipnodebyaddr() may be used instead of gethostbyname() and gethostbyaddr(),
if the newer functions are available. This module also contains various other
functions concerned with hosts and addresses, and a random number function,
used for randomizing hosts with equal MXs but available for use in other parts
of Exim. */


#include "exim.h"


/* Static variable for preserving the list of interface addresses in case it is
used more than once. */

static ip_address_item *local_interface_data = NULL;


#ifdef USE_INET_NTOA_FIX
/*************************************************
*         Replacement for broken inet_ntoa()     *
*************************************************/

/* On IRIX systems, gcc uses a different structure passing convention to the
native libraries. This causes inet_ntoa() to always yield 0.0.0.0 or
255.255.255.255. To get round this, we provide a private version of the
function here. It is used only if USE_INET_NTOA_FIX is set, which should happen
only when gcc is in use on an IRIX system. Code send to me by J.T. Breitner,
with these comments:

  code by Stuart Levy
  as seen in comp.sys.sgi.admin

Arguments:  sa  an in_addr structure
Returns:        pointer to static text string
*/

char *
inet_ntoa(struct in_addr sa)
{
static uschar addr[20];
sprintf(addr, "%d.%d.%d.%d",
        (US &sa.s_addr)[0],
        (US &sa.s_addr)[1],
        (US &sa.s_addr)[2],
        (US &sa.s_addr)[3]);
  return addr;
}
#endif



/*************************************************
*              Random number generator           *
*************************************************/

/* This is a simple pseudo-random number generator. It does not have to be
very good for the uses to which it is put. When running the regression tests,
start with a fixed seed.

Arguments:
  limit:    one more than the largest number required

Returns:    a pseudo-random number in the range 0 to limit-1
*/

int
random_number(int limit)
{
if (random_seed == 0)
  {
  if (running_in_test_harness) random_seed = 42; else
    {
    int p = (int)getpid();
    random_seed = (int)time(NULL) ^ ((p << 16) | p);
    }
  }
random_seed = 1103515245 * random_seed + 12345;
return (unsigned int)(random_seed >> 16) % limit;
}



/*************************************************
*         Sort addresses when testing            *
*************************************************/

/* This function is called only when running in the test harness. It sorts a
number of multihomed host IP addresses into the order, so as to get
repeatability. This doesn't have to be efficient. But don't interchange IPv4
and IPv6 addresses!

Arguments:
  host        -> the first host item
  last        -> the last host item
  
Returns:      nothing
*/  

static void
sort_addresses(host_item *host, host_item *last)
{
BOOL done = FALSE;
while (!done)
  {
  host_item *h;
  done = TRUE;
  for (h = host; h != last; h = h->next)
    {
    if ((Ustrchr(h->address, ':') == NULL) !=
        (Ustrchr(h->next->address, ':') == NULL))
      continue;
    if (Ustrcmp(h->address, h->next->address) > 0)
      {
      uschar *temp = h->address;
      h->address = h->next->address;
      h->next->address = temp;
      done = FALSE;
      }
    }
  }
}



/*************************************************
*       Build chain of host items from list      *
*************************************************/

/* This function builds a chain of host items from a textual list of host
names. It does not do any lookups. If randomize is true, the chain is build in
a randomized order. There may be multiple groups of independently randomized
hosts; they are delimited by a host name consisting of just "+".

Arguments:
  anchor      anchor for the chain
  list        text list
  randomize   TRUE for randomizing

Returns:      nothing
*/

void
host_build_hostlist(host_item **anchor, uschar *list, BOOL randomize)
{
int sep = 0;
int fake_mx = MX_NONE;          /* This value is actually -1 */
uschar *name;
uschar buffer[1024];

if (list == NULL) return;
if (randomize) fake_mx--;       /* Start at -2 for randomizing */

*anchor = NULL;

while ((name = string_nextinlist(&list, &sep, buffer, sizeof(buffer))) != NULL)
  {
  host_item *h;

  if (name[0] == '+' && name[1] == 0)   /* "+" delimits a randomized group */
    {                                   /* ignore if not randomizing */
    if (randomize) fake_mx--;
    continue;
    }

  h = store_get(sizeof(host_item));
  h->name = string_copy(name);
  h->address = NULL;
  h->port = PORT_NONE;
  h->mx = fake_mx;
  h->sort_key = randomize? (-fake_mx)*1000 + random_number(1000) : 0;
  h->status = hstatus_unknown;
  h->why = hwhy_unknown;
  h->last_try = 0;

  if (*anchor == NULL)
    {
    h->next = NULL;
    *anchor = h;
    }
  else
    {
    host_item *hh = *anchor;
    if (h->sort_key < hh->sort_key)
      {
      h->next = hh;
      *anchor = h;
      }
    else
      {
      while (hh->next != NULL && h->sort_key >= (hh->next)->sort_key)
        hh = hh->next;
      h->next = hh->next;
      hh->next = h;
      }
    }
  }
}





/*************************************************
*        Extract port from address string        *
*************************************************/

/* In the spool file, and in the -oMa and -oMi options, a host plus port is
given as an IP address followed by a dot and a port number. This function
decodes this.

An alternative format for the -oMa and -oMi options is [ip address]:port which
is what Exim 4 uses for output, because it seems to becoming commonly used,
whereas the dot form confuses some programs/people. So we recognize that form
too.

Argument:
  address    points to the string; if there is a port, the '.' in the string
             is overwritten with zero to terminate the address; if the string
             is in the [xxx]:ppp format, the address is shifted left and the
             brackets are removed

Returns:     0 if there is no port, else the port number. If there's a syntax
             error, leave the incoming address alone, and return 0.
*/

int
host_extract_port(uschar *address)
{
int port = 0;
uschar *endptr;

/* Handle the "bracketed with colon on the end" format */

if (*address == '[')
  {
  uschar *rb = address + 1;
  while (*rb != 0 && *rb != ']') rb++;
  if (*rb++ == 0) return 0;        /* Missing ]; leave invalid address */
  if (*rb == ':')
    {
    port = Ustrtol(rb + 1, &endptr, 10);
    if (*endptr != 0) return 0;    /* Invalid port; leave invalid address */
    }
  else if (*rb != 0) return 0;     /* Bad syntax; leave invalid address */
  memmove(address, address + 1, rb - address - 2);
  rb[-2] = 0;
  }

/* Handle the "dot on the end" format */

else
  {
  int skip = -3;                   /* Skip 3 dots in IPv4 addresses */
  address--;
  while (*(++address) != 0)
    {
    int ch = *address;
    if (ch == ':') skip = 0;       /* Skip 0 dots in IPv6 addresses */
      else if (ch == '.' && skip++ >= 0) break;
    }
  if (*address == 0) return 0;
  port = Ustrtol(address + 1, &endptr, 10);
  if (*endptr != 0) return 0;      /* Invalid port; leave invalid address */
  *address = 0;
  }

return port;
}



#ifndef STAND_ALONE    /* Omit when standalone testing */

/*************************************************
*     Build sender_fullhost and sender_rcvhost   *
*************************************************/

/* This function is called when sender_host_name and/or sender_helo_name
have been set. Or might have been set - for a local message read off the spool
they won't be. In that case, do nothing. Otherwise, set up the fullhost string
as follows:

(a) No sender_host_name or sender_helo_name: "[ip address]"
(b) Just sender_host_name: "host_name [ip address]"
(c) Just sender_helo_name: "(helo_name) [ip address]"
(d) The two are identical: "host_name [ip address]"
(e) The two are different: "host_name (helo_name) [ip address]"

If log_incoming_port is set, the sending host's port number is added to the IP
address.

This function also builds sender_rcvhost for use in Received: lines, whose
syntax is a bit different. This value also includes the RFC 1413 identity.
There wouldn't be two different variables if I had got all this right in the
first place.

Because this data may survive over more than one incoming SMTP message, it has
to be in permanent store.

Arguments:  none
Returns:    nothing
*/

void
host_build_sender_fullhost(void)
{
uschar *address;
int old_pool = store_pool;

if (sender_host_address == NULL) return;

store_pool = POOL_PERM;

/* Set up address, with or without the port. After discussion, it seems that
the only format that doesn't cause trouble is [aaaa]:pppp. However, we can't
use this directly as the first item for Received: because it ain't an RFC 2822
domain. Sigh. */

address = string_sprintf("[%s]:%d", sender_host_address, sender_host_port);
if ((log_extra_selector & LX_incoming_port) == 0 || sender_host_port <= 0)
  *(Ustrrchr(address, ':')) = 0;

/* Host name is not verified */

if (sender_host_name == NULL)
  {
  uschar *portptr = Ustrstr(address, "]:");
  int size = 0;
  int ptr = 0;
  int adlen;    /* Sun compiler doesn't like ++ in initializers */

  adlen = (portptr == NULL)? Ustrlen(address) : (++portptr - address);
  sender_fullhost = (sender_helo_name == NULL)? address :
    string_sprintf("(%s) %s", sender_helo_name, address);

  sender_rcvhost = string_cat(NULL, &size, &ptr, address, adlen);

  if (sender_ident != NULL || sender_helo_name != NULL || portptr != NULL)
    {
    int firstptr;
    sender_rcvhost = string_cat(sender_rcvhost, &size, &ptr, US" (", 2);
    firstptr = ptr;

    if (portptr != NULL)
      sender_rcvhost = string_append(sender_rcvhost, &size, &ptr, 2, US"port=",
        portptr + 1);

    if (sender_helo_name != NULL)
      sender_rcvhost = string_append(sender_rcvhost, &size, &ptr, 2,
        (firstptr == ptr)? US"helo=" : US" helo=", sender_helo_name);

    if (sender_ident != NULL)
      sender_rcvhost = string_append(sender_rcvhost, &size, &ptr, 2,
        (firstptr == ptr)? US"ident=" : US" ident=", sender_ident);

    sender_rcvhost = string_cat(sender_rcvhost, &size, &ptr, US")", 1);
    }

  sender_rcvhost[ptr] = 0;   /* string_cat() always leaves room */

  /* Release store, because string_cat allocated a minimum of 100 bytes that
  are rarely completely used. */

  store_reset(sender_rcvhost + ptr + 1);
  }

/* Host name is known and verified. */

else
  {
  int len;
  if (sender_helo_name == NULL ||
      strcmpic(sender_host_name, sender_helo_name) == 0 ||
        (sender_helo_name[0] == '[' &&
         sender_helo_name[(len=Ustrlen(sender_helo_name))-1] == ']' &&
         strncmpic(sender_helo_name+1, sender_host_address, len - 2) == 0))
    {
    sender_fullhost = string_sprintf("%s %s", sender_host_name, address);
    sender_rcvhost = (sender_ident == NULL)?
      string_sprintf("%s (%s)", sender_host_name, address) :
      string_sprintf("%s (%s ident=%s)", sender_host_name, address,
        sender_ident);
    }
  else
    {
    sender_fullhost = string_sprintf("%s (%s) %s", sender_host_name,
      sender_helo_name, address);
    sender_rcvhost = (sender_ident == NULL)?
      string_sprintf("%s (%s helo=%s)", sender_host_name,
        address, sender_helo_name) :
      string_sprintf("%s\n\t(%s helo=%s ident=%s)", sender_host_name,
        address, sender_helo_name, sender_ident);
    }
  }

store_pool = old_pool;

DEBUG(D_host_lookup) debug_printf("sender_fullhost = %s\n", sender_fullhost);
DEBUG(D_host_lookup) debug_printf("sender_rcvhost = %s\n", sender_rcvhost);
}



/*************************************************
*          Build host+ident message              *
*************************************************/

/* Used when logging rejections and various ACL and SMTP incidents. The text
return depends on whether sender_fullhost and sender_ident are set or not:

  no ident, no host   => U=unknown
  no ident, host set  => H=sender_fullhost
  ident set, no host  => U=ident
  ident set, host set => H=sender_fullhost U=ident

Arguments:
  useflag   TRUE if first item to be flagged (H= or U=); if there are two
              items, the second is always flagged

Returns:    pointer to a string in big_buffer
*/

uschar *
host_and_ident(BOOL useflag)
{
if (sender_fullhost == NULL)
  {
  (void)string_format(big_buffer, big_buffer_size, "%s%s", useflag? "U=" : "",
     (sender_ident == NULL)? US"unknown" : sender_ident);
  }
else
  {
  uschar *flag = useflag? US"H=" : US"";
  uschar *iface = US"";
  if ((log_extra_selector & LX_incoming_interface) != 0 &&
       interface_address != NULL)
    iface = string_sprintf(" I=[%s]:%d", interface_address, interface_port);
  if (sender_ident == NULL)
    (void)string_format(big_buffer, big_buffer_size, "%s%s%s",
      flag, sender_fullhost, iface);
  else
    (void)string_format(big_buffer, big_buffer_size, "%s%s%s U=%s",
      flag, sender_fullhost, iface, sender_ident);
  }
return big_buffer;
}

#endif   /* STAND_ALONE */




/*************************************************
*         Build list of local interfaces         *
*************************************************/

/* This function interprets the contents of the local_interfaces or
extra_local_interfaces options, and creates an ip_address_item block for each
item on the list. There is no special interpretation of any IP addresses; in
particular, 0.0.0.0 and ::0 are returned without modification. If any address
includes a port, it is set in the block. Otherwise the port value is set to
zero.

Arguments:
  list        the list
  name        the name of the option being expanded

Returns:      a chain of ip_address_items, each containing to a textual
              version of an IP address, and a port number (host order) or
              zero if no port was given with the address
*/

ip_address_item *
host_build_ifacelist(uschar *list, uschar *name)
{
int sep = 0;
uschar *s;
uschar buffer[64];
ip_address_item *yield = NULL;
ip_address_item *last = NULL;
ip_address_item *next;

while ((s = string_nextinlist(&list, &sep, buffer, sizeof(buffer))) != NULL)
  {
  int port = host_extract_port(s);            /* Leaves just the IP address */
  if (!string_is_ip_address(s, NULL))
    log_write(0, LOG_MAIN|LOG_PANIC_DIE, "Malformed IP address \"%s\" in %s",
      s, name);

  /* This use of strcpy() is OK because we have checked that s is a valid IP
  address above. The field in the ip_address_item is large enough to hold an
  IPv6 address. */

  next = store_get(sizeof(ip_address_item));
  next->next = NULL;
  Ustrcpy(next->address, s);
  next->port = port;
  next->v6_include_v4 = FALSE;

  if (yield == NULL) yield = last = next; else
    {
    last->next = next;
    last = next;
    }
  }

return yield;
}





/*************************************************
*         Find addresses on local interfaces     *
*************************************************/

/* This function finds the addresses of local IP interfaces. These are used
when testing for routing to the local host. As the function may be called more
than once, the list is preserved in permanent store, pointed to by a static
variable, to save doing the work more than once per process.

The generic list of interfaces is obtained by calling host_build_ifacelist()
for local_interfaces and extra_local_interfaces. This list scanned to remove
duplicates (which may exist with different ports - not relevant here). If
either of the wildcard IP addresses (0.0.0.0 and ::0) are encountered, they are
replaced by the appropriate (IPv4 or IPv6) list of actual local interfaces,
obtained from os_find_running_interfaces().

Arguments:    none
Returns:      a chain of ip_address_items, each containing to a textual
              version of an IP address; the port numbers are not relevant
*/


/* First, a local subfunction to add an interface to a list in permanent store,
but only if there isn't a previous copy of that address on the list. */

static ip_address_item *
add_unique_interface(ip_address_item *list, ip_address_item *ipa)
{
ip_address_item *ipa2;
for (ipa2 = list; ipa2 != NULL; ipa2 = ipa2->next)
  if (Ustrcmp(ipa2->address, ipa->address) == 0) return list;
ipa2 = store_get_perm(sizeof(ip_address_item));
*ipa2 = *ipa;
ipa2->next = list;
return ipa2;
}


/* This is the globally visible function */

ip_address_item *
host_find_interfaces(void)
{
ip_address_item *running_interfaces = NULL;

if (local_interface_data == NULL)
  {
  void *reset_item = store_get(0);
  ip_address_item *dlist = host_build_ifacelist(local_interfaces,
    US"local_interfaces");
  ip_address_item *xlist = host_build_ifacelist(extra_local_interfaces,
    US"extra_local_interfaces");
  ip_address_item *ipa;

  if (dlist == NULL) dlist = xlist; else
    {
    for (ipa = dlist; ipa->next != NULL; ipa = ipa->next);
    ipa->next = xlist;
    }

  for (ipa = dlist; ipa != NULL; ipa = ipa->next)
    {
    if (Ustrcmp(ipa->address, "0.0.0.0") == 0 ||
        Ustrcmp(ipa->address, "::0") == 0)
      {
      ip_address_item *ipa2;
      BOOL ipv6 = ipa->address[0] == ':';
      if (running_interfaces == NULL)
        running_interfaces = os_find_running_interfaces();
      for (ipa2 = running_interfaces; ipa2 != NULL; ipa2 = ipa2->next)
        {
        if ((Ustrchr(ipa2->address, ':') != NULL) == ipv6)
          local_interface_data = add_unique_interface(local_interface_data,
          ipa2);
        }
      }
    else
      {
      local_interface_data = add_unique_interface(local_interface_data, ipa);
      DEBUG(D_interface)
        {
        debug_printf("Configured local interface: address=%s", ipa->address);
        if (ipa->port != 0) debug_printf(" port=%d", ipa->port);
        debug_printf("\n");
        }
      }
    }
  store_reset(reset_item);
  }

return local_interface_data;
}





/*************************************************
*        Convert network IP address to text      *
*************************************************/

/* Given an IPv4 or IPv6 address in binary, convert it to a text
string and return the result in a piece of new store. The address can
either be given directly, or passed over in a sockaddr structure. Note
that this isn't the converse of host_aton() because of byte ordering
differences. See host_nmtoa() below.

Arguments:
  type       if < 0 then arg points to a sockaddr, else
             either AF_INET or AF_INET6
  arg        points to a sockaddr if type is < 0, or
             points to an IPv4 address (32 bits), or
             points to an IPv6 address (128 bits),
             in both cases, in network byte order
  buffer     if NULL, the result is returned in gotten store;
             else points to a buffer to hold the answer
  portptr    points to where to put the port number, if non NULL; only
             used when type < 0

Returns:     pointer to character string
*/

uschar *
host_ntoa(int type, const void *arg, uschar *buffer, int *portptr)
{
uschar *yield;

/* The new world. It is annoying that we have to fish out the address from
different places in the block, depending on what kind of address it is. It
is also a pain that inet_ntop() returns a const uschar *, whereas the IPv4
function inet_ntoa() returns just uschar *, and some picky compilers insist
on warning if one assigns a const uschar * to a uschar *. Hence the casts. */

#if HAVE_IPV6
uschar addr_buffer[46];
if (type < 0)
  {
  int family = ((struct sockaddr *)arg)->sa_family;
  if (family == AF_INET6)
    {
    struct sockaddr_in6 *sk = (struct sockaddr_in6 *)arg;
    yield = (uschar *)inet_ntop(family, &(sk->sin6_addr), CS addr_buffer,
      sizeof(addr_buffer));
    if (portptr != NULL) *portptr = ntohs(sk->sin6_port);
    }
  else
    {
    struct sockaddr_in *sk = (struct sockaddr_in *)arg;
    yield = (uschar *)inet_ntop(family, &(sk->sin_addr), CS addr_buffer,
      sizeof(addr_buffer));
    if (portptr != NULL) *portptr = ntohs(sk->sin_port);
    }
  }
else
  {
  yield = (uschar *)inet_ntop(type, arg, CS addr_buffer, sizeof(addr_buffer));
  }

/* If the result is a mapped IPv4 address, show it in V4 format. */

if (Ustrncmp(yield, "::ffff:", 7) == 0) yield += 7;

#else  /* HAVE_IPV6 */

/* The old world */

if (type < 0)
  {
  yield = US inet_ntoa(((struct sockaddr_in *)arg)->sin_addr);
  if (portptr != NULL) *portptr = ntohs(((struct sockaddr_in *)arg)->sin_port);
  }
else
  yield = US inet_ntoa(*((struct in_addr *)arg));
#endif

/* If there is no buffer, put the string into some new store. */

if (buffer == NULL) return string_copy(yield);

/* Callers of this function with a non-NULL buffer must ensure that it is
large enough to hold an IPv6 address, namely, at least 46 bytes. That's what
makes this use of strcpy() OK. */

Ustrcpy(buffer, yield);
return buffer;
}




/*************************************************
*         Convert address text to binary         *
*************************************************/

/* Given the textual form of an IP address, convert it to binary in an
array of ints. IPv4 addresses occupy one int; IPv6 addresses occupy 4 ints.
The result has the first byte in the most significant byte of the first int. In
other words, the result is not in network byte order, but in host byte order.
As a result, this is not the converse of host_ntoa(), which expects network
byte order. See host_nmtoa() below.

Arguments:
  address    points to the textual address, checked for syntax
  bin        points to an array of 4 ints

Returns:     the number of ints used
*/

int
host_aton(uschar *address, int *bin)
{
int x[4];
int v4offset = 0;

/* Handle IPv6 address, which may end with an IPv4 address. This code is NOT
enclosed in #if HAVE_IPV6 in order that IPv6 addresses are recognized even if
IPv6 is not supported. */

if (Ustrchr(address, ':') != NULL)
  {
  uschar *p = address;
  uschar *component[8];
  BOOL ipv4_ends = FALSE;
  int ci = 0;
  int nulloffset = 0;
  int v6count = 8;
  int i;

  /* If the address starts with a colon, it will start with two colons.
  Just lose the first one, which will leave a null first component. */

  if (*p == ':') p++;

  /* Split the address into components separated by colons. The input address 
  is supposed to be checked for syntax. There was a case where this was 
  overlooked; to guard against that happening again, check here and crash if 
  there is a violation. */

  while (*p != 0)
    {
    int len = Ustrcspn(p, ":");
    if (len == 0) nulloffset = ci;
    if (ci > 7) log_write(0, LOG_MAIN|LOG_PANIC_DIE, 
      "Internal error: invalid IPv6 address \"%s\" passed to host_aton()",
      address);  
    component[ci++] = p;
    p += len;
    if (*p == ':') p++;
    }

  /* If the final component contains a dot, it is a trailing v4 address.
  As the syntax is known to be checked, just set up for a trailing
  v4 address and restrict the v6 part to 6 components. */

  if (Ustrchr(component[ci-1], '.') != NULL)
    {
    address = component[--ci];
    ipv4_ends = TRUE;
    v4offset = 3;
    v6count = 6;
    }

  /* If there are fewer than 6 or 8 components, we have to insert some
  more empty ones in the middle. */

  if (ci < v6count)
    {
    int insert_count = v6count - ci;
    for (i = v6count-1; i > nulloffset + insert_count; i--)
      component[i] = component[i - insert_count];
    while (i > nulloffset) component[i--] = US"";
    }

  /* Now turn the components into binary in pairs and bung them
  into the vector of ints. */

  for (i = 0; i < v6count; i += 2)
    bin[i/2] = (Ustrtol(component[i], NULL, 16) << 16) +
      Ustrtol(component[i+1], NULL, 16);

  /* If there was no terminating v4 component, we are done. */

  if (!ipv4_ends) return 4;
  }

/* Handle IPv4 address */

sscanf(CS address, "%d.%d.%d.%d", x, x+1, x+2, x+3);
bin[v4offset] = (x[0] << 24) + (x[1] << 16) + (x[2] << 8) + x[3];
return v4offset+1;
}


/*************************************************
*           Apply mask to an IP address          *
*************************************************/

/* Mask an address held in 1 or 4 ints, with the ms bit in the ms bit of the
first int, etc.

Arguments:
  count        the number of ints
  binary       points to the ints to be masked
  mask         the count of ms bits to leave, or -1 if no masking

Returns:       nothing
*/

void
host_mask(int count, int *binary, int mask)
{
int i;
if (mask < 0) mask = 99999;
for (i = 0; i < count; i++)
  {
  int wordmask;
  if (mask == 0) wordmask = 0;
  else if (mask < 32)
    {
    wordmask = (-1) << (32 - mask);
    mask = 0;
    }
  else
    {
    wordmask = -1;
    mask -= 32;
    }
  binary[i] &= wordmask;
  }
}




/*************************************************
*     Convert masked IP address in ints to text  *
*************************************************/

/* We can't use host_ntoa() because it assumes the binary values are in network
byte order, and these are the result of host_aton(), which puts them in ints in
host byte order. Also, we really want IPv6 addresses to be in a canonical
format, so we output them with no abbreviation. In a number of cases we can't
use the normal colon separator in them because it terminates keys in lsearch
files, so we want to use dot instead. There's an argument that specifies what 
to use for IPv6 addresses.

Arguments:
  count       1 or 4 (number of ints)
  binary      points to the ints
  mask        mask value; if < 0 don't add to result
  buffer      big enough to hold the result
  sep         component separator character for IPv6 addresses 

Returns:      the number of characters placed in buffer, not counting
              the final nul.
*/

int
host_nmtoa(int count, int *binary, int mask, uschar *buffer, int sep)
{
int i, j;
uschar *tt = buffer;

if (count == 1)
  {
  j = binary[0];
  for (i = 24; i >= 0; i -= 8)
    {
    sprintf(CS tt, "%d.", (j >> i) & 255);
    while (*tt) tt++;
    }
  }
else
  {
  for (i = 0; i < 4; i++)
    {
    j = binary[i];
    sprintf(CS tt, "%04x%c%04x%c", (j >> 16) & 0xffff, sep, j & 0xffff, sep);
    while (*tt) tt++;
    }
  }

tt--;   /* lose final separator */

if (mask < 0)
  *tt = 0;
else
  {
  sprintf(CS tt, "/%d", mask);
  while (*tt) tt++;
  }

return tt - buffer;
}



/*************************************************
*        Check port for tls_on_connect           *
*************************************************/

/* This function checks whether a given incoming port is configured for tls-
on-connect. It is called from the daemon and from inetd handling. If the global
option tls_on_connect is already set, all ports operate this way. Otherwise, we
check the tls_on_connect_ports option for a list of ports.

Argument:  a port number
Returns:   TRUE or FALSE
*/

BOOL
host_is_tls_on_connect_port(int port)
{
int sep = 0;
uschar buffer[32];
uschar *list = tls_on_connect_ports;
uschar *s;

if (tls_on_connect) return TRUE;

while ((s = string_nextinlist(&list, &sep, buffer, sizeof(buffer))) != NULL)
  {
  uschar *end;
  int lport = Ustrtol(s, &end, 10);
  if (*end != 0) log_write(0, LOG_MAIN|LOG_PANIC_DIE, "tls_on_connect_ports "
    "contains \"%s\", which is not a port number: exim abandoned", s);
  if (lport == port) return TRUE;
  }

return FALSE;
}



/*************************************************
*        Check whether host is in a network      *
*************************************************/

/* This function checks whether a given IP address matches a pattern that
represents either a single host, or a network (using CIDR notation). The caller
of this function must check the syntax of the arguments before calling it.

Arguments:
  host        string representation of the ip-address to check
  net         string representation of the network, with optional CIDR mask
  maskoffset  offset to the / that introduces the mask in the key
              zero if there is no mask

Returns:
  TRUE   the host is inside the network
  FALSE  the host is NOT inside the network
*/

BOOL
host_is_in_net(uschar *host, uschar *net, int maskoffset)
{
int i;
int address[4];
int incoming[4];
int mlen;
int size = host_aton(net, address);
int insize;

/* No mask => all bits to be checked */

if (maskoffset == 0) mlen = 99999;    /* Big number */
  else mlen = Uatoi(net + maskoffset + 1);

/* Convert the incoming address to binary. */

insize = host_aton(host, incoming);

/* Convert IPv4 addresses given in IPv6 compatible mode, which represent
   connections from IPv4 hosts to IPv6 hosts, that is, addresses of the form
   ::ffff:<v4address>, to IPv4 format. */

if (insize == 4 && incoming[0] == 0 && incoming[1] == 0 &&
    incoming[2] == 0xffff)
  {
  insize = 1;
  incoming[0] = incoming[3];
  }

/* No match if the sizes don't agree. */

if (insize != size) return FALSE;

/* Else do the masked comparison. */

for (i = 0; i < size; i++)
  {
  int mask;
  if (mlen == 0) mask = 0;
  else if (mlen < 32)
    {
    mask = (-1) << (32 - mlen);
    mlen = 0;
    }
  else
    {
    mask = -1;
    mlen -= 32;
    }
  if ((incoming[i] & mask) != (address[i] & mask)) return FALSE;
  }

return TRUE;
}



/*************************************************
*       Scan host list for local hosts           *
*************************************************/

/* Scan through a chain of addresses and check whether any of them is the
address of an interface on the local machine. If so, remove that address and
any previous ones with the same MX value, and all subsequent ones (which will
have greater or equal MX values) from the chain. Note: marking them as unusable
is NOT the right thing to do because it causes the hosts not to be used for
other domains, for which they may well be correct.

The hosts may be part of a longer chain; we only process those between the
initial pointer and the "last" pointer.

There is also a list of "pseudo-local" host names which are checked against the
host names. Any match causes that host item to be treated the same as one which
matches a local IP address.

If the very first host is a local host, then all MX records had a precedence
greater than or equal to that of the local host. Either there's a problem in
the DNS, or an apparently remote name turned out to be an abbreviation for the
local host. Give a specific return code, and let the caller decide what to do.
Otherwise, give a success code if at least one host address has been found.

Arguments:
  host        pointer to the first host in the chain
  lastptr     pointer to pointer to the last host in the chain (may be updated)
  removed     if not NULL, set TRUE if some local addresses were removed
                from the list

Returns:
  HOST_FOUND       if there is at least one host with an IP address on the chain
                     and an MX value less than any MX value associated with the
                     local host
  HOST_FOUND_LOCAL if a local host is among the lowest-numbered MX hosts; when
                     the host addresses were obtained from A records or
                     gethostbyname(), the MX values are set to -1.
  HOST_FIND_FAILED if no valid hosts with set IP addresses were found
*/

int
host_scan_for_local_hosts(host_item *host, host_item **lastptr, BOOL *removed)
{
int yield = HOST_FIND_FAILED;
host_item *last = *lastptr;
host_item *prev = NULL;
host_item *h;

if (removed != NULL) *removed = FALSE;

if (local_interface_data == NULL) local_interface_data = host_find_interfaces();

for (h = host; h != last->next; h = h->next)
  {
  #ifndef STAND_ALONE
  if (hosts_treat_as_local != NULL)
    {
    int rc;
    uschar *save = deliver_domain;
    deliver_domain = h->name;   /* set $domain */
    rc = match_isinlist(string_copylc(h->name), &hosts_treat_as_local, 0,
      &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL);
    deliver_domain = save;
    if (rc == OK) goto FOUND_LOCAL;
    }
  #endif

  /* It seems that on many operating systems, 0.0.0.0 is treated as a synonym
  for 127.0.0.1 and refers to the local host. We therefore force it always to
  be treated as local. */

  if (h->address != NULL)
    {
    ip_address_item *ip;
    if (Ustrcmp(h->address, "0.0.0.0") == 0) goto FOUND_LOCAL;
    for (ip = local_interface_data; ip != NULL; ip = ip->next)
      if (Ustrcmp(h->address, ip->address) == 0) goto FOUND_LOCAL;
    yield = HOST_FOUND;  /* At least one remote address has been found */
    }

  /* Update prev to point to the last host item before any that have
  the same MX value as the one we have just considered. */

  if (h->next == NULL || h->next->mx != h->mx) prev = h;
  }

return yield;  /* No local hosts found: return HOST_FOUND or HOST_FIND_FAILED */

/* A host whose IP address matches a local IP address, or whose name matches
something in hosts_treat_as_local has been found. */

FOUND_LOCAL:

if (prev == NULL)
  {
  HDEBUG(D_host_lookup) debug_printf((h->mx >= 0)?
    "local host has lowest MX\n" :
    "local host found for non-MX address\n");
  return HOST_FOUND_LOCAL;
  }

HDEBUG(D_host_lookup)
  {
  debug_printf("local host in host list - removed hosts:\n");
  for (h = prev->next; h != last->next; h = h->next)
    debug_printf("  %s %s %d\n", h->name, h->address, h->mx);
  }

if (removed != NULL) *removed = TRUE;
prev->next = last->next;
*lastptr = prev;
return yield;
}




/*************************************************
*        Remove duplicate IPs in host list       *
*************************************************/

/* You would think that administrators could set up their DNS records so that
one ended up with a list of unique IP addresses after looking up A or MX
records, but apparently duplication is common. So we scan such lists and
remove the later duplicates. Note that we may get lists in which some host
addresses are not set.

Arguments:
  host        pointer to the first host in the chain
  lastptr     pointer to pointer to the last host in the chain (may be updated)

Returns:      nothing
*/

static void
host_remove_duplicates(host_item *host, host_item **lastptr)
{
while (host != *lastptr)
  {
  if (host->address != NULL)
    {
    host_item *h = host;
    while (h != *lastptr)
      {
      if (h->next->address != NULL &&
          Ustrcmp(h->next->address, host->address) == 0)
        {
        DEBUG(D_host_lookup) debug_printf("duplicate IP address %s (MX=%d) "
          "removed\n", host->address, h->next->mx);
        if (h->next == *lastptr) *lastptr = h;
        h->next = h->next->next;
        }
      else h = h->next;
      }
    }
  /* If the last item was removed, host may have become == *lastptr */
  if (host != *lastptr) host = host->next;
  }
}




/*************************************************
*    Find sender host name by gethostbyaddr()    *
*************************************************/

/* This used to be the only way it was done, but it turns out that not all
systems give aliases for calls to gethostbyaddr() - or one of the modern
equivalents like getipnodebyaddr(). Fortunately, multiple PTR records are rare,
but they can still exist. This function is now used only when a DNS lookup of
the IP address fails, in order to give access to /etc/hosts.

Arguments:   none
Returns:     OK, DEFER, FAIL
*/

static int
host_name_lookup_byaddr(void)
{
int len;
uschar *s, *t;
struct hostent *hosts;
struct in_addr addr;

/* Lookup on IPv6 system */

#if HAVE_IPV6
if (Ustrchr(sender_host_address, ':') != NULL)
  {
  struct in6_addr addr6;
  if (inet_pton(AF_INET6, CS sender_host_address, &addr6) != 1)
    log_write(0, LOG_MAIN|LOG_PANIC_DIE, "unable to parse \"%s\" as an "
      "IPv6 address", sender_host_address);
  #if HAVE_GETIPNODEBYADDR
  hosts = getipnodebyaddr(CS &addr6, sizeof(addr6), AF_INET6, &h_errno);
  #else
  hosts = gethostbyaddr(CS &addr6, sizeof(addr6), AF_INET6);
  #endif
  }
else
  {
  if (inet_pton(AF_INET, CS sender_host_address, &addr) != 1)
    log_write(0, LOG_MAIN|LOG_PANIC_DIE, "unable to parse \"%s\" as an "
      "IPv4 address", sender_host_address);
  #if HAVE_GETIPNODEBYADDR
  hosts = getipnodebyaddr(CS &addr, sizeof(addr), AF_INET, &h_errno);
  #else
  hosts = gethostbyaddr(CS &addr, sizeof(addr), AF_INET);
  #endif
  }

/* Do lookup on IPv4 system */

#else
addr.s_addr = (S_ADDR_TYPE)inet_addr(CS sender_host_address);
hosts = gethostbyaddr(CS(&addr), sizeof(addr), AF_INET);
#endif

/* Failed to look up the host. */

if (hosts == NULL)
  {
  HDEBUG(D_host_lookup) debug_printf("IP address lookup failed: h_errno=%d\n",
    h_errno);
  return (h_errno == TRY_AGAIN || h_errno == NO_RECOVERY) ? DEFER : FAIL;
  }

/* It seems there are some records in the DNS that yield an empty name. We
treat this as non-existent. In some operating systems, this is returned as an
empty string; in others as a single dot. */

if (hosts->h_name[0] == 0 || hosts->h_name[0] == '.')
  {
  HDEBUG(D_host_lookup) debug_printf("IP address lookup yielded an empty name: "
    "treated as non-existent host name\n");
  return FAIL;
  }

/* Copy and lowercase the name, which is in static storage in many systems.
Put it in permanent memory. */

s = (uschar *)hosts->h_name;
len = Ustrlen(s) + 1;
t = sender_host_name = store_get_perm(len);
while (*s != 0) *t++ = tolower(*s++);
*t = 0;

/* If the host has aliases, build a copy of the alias list */

if (hosts->h_aliases != NULL)
  {
  int count = 1;
  uschar **aliases, **ptr;
  for (aliases = USS hosts->h_aliases; *aliases != NULL; aliases++) count++;
  ptr = sender_host_aliases = store_get_perm(count * sizeof(uschar *));
  for (aliases = USS hosts->h_aliases; *aliases != NULL; aliases++)
    {
    uschar *s = *aliases;
    int len = Ustrlen(s) + 1;
    uschar *t = *ptr++ = store_get_perm(len);
    while (*s != 0) *t++ = tolower(*s++);
    *t = 0;
    }
  *ptr = NULL;
  }

return OK;
}



/*************************************************
*        Find host name for incoming call        *
*************************************************/

/* Put the name in permanent store, pointed to by sender_host_name. We also set
up a list of alias names, pointed to by sender_host_alias. The list is
NULL-terminated. The incoming address is in sender_host_address, either in
dotted-quad form for IPv4 or in colon-separated form for IPv6.

This function does a thorough check that the names it finds point back to the
incoming IP address. Any that do not are discarded. Note that this is relied on
by the ACL reverse_host_lookup check.

On some systems, get{host,ipnode}byaddr() appears to do this internally, but
this it not universally true. Also, for release 4.30, this function was changed
to do a direct DNS lookup first, by default[1], because it turns out that that
is the only guaranteed way to find all the aliases on some systems. My
experiments indicate that Solaris gethostbyaddr() gives the aliases for but
Linux does not.

[1] The actual order is controlled by the host_lookup_order option.

Arguments:    none
Returns:      OK on success, the answer being placed in the global variable
                sender_host_name, with any aliases in a list hung off
                sender_host_aliases
              FAIL if no host name can be found
              DEFER if a temporary error was encountered

The variable host_lookup_msg is set to an empty string on sucess, or to a
reason for the failure otherwise, in a form suitable for tagging onto an error
message, and also host_lookup_failed is set TRUE if the lookup failed. Any
dynamically constructed string for host_lookup_msg must be in permanent store,
because it might be used for several incoming messages on the same SMTP
connection. */

int
host_name_lookup(void)
{
int old_pool, rc;
int sep = 0;
uschar *hname, *save_hostname;
uschar **aliases;
uschar buffer[256];
uschar *ordername;
uschar *list = host_lookup_order;
dns_record *rr;
dns_answer dnsa;
dns_scan dnss;

HDEBUG(D_host_lookup)
  debug_printf("looking up host name for %s\n", sender_host_address);

/* For testing the case when a lookup does not complete, we have a special
reserved IP address. */

if (running_in_test_harness &&
    Ustrcmp(sender_host_address, "99.99.99.99") == 0)
  {
  HDEBUG(D_host_lookup)
    debug_printf("Test harness: host name lookup returns DEFER\n");
  return DEFER;
  }

/* Do lookups directly in the DNS or via gethostbyaddr() (or equivalent), in
the order specified by the host_lookup_order option. */

while ((ordername = string_nextinlist(&list, &sep, buffer, sizeof(buffer)))
        != NULL)
  {
  if (strcmpic(ordername, US"bydns") == 0)
    {
    dns_init(FALSE, FALSE);
    dns_build_reverse(sender_host_address, buffer);
    rc = dns_lookup(&dnsa, buffer, T_PTR, NULL);

    /* The first record we come across is used for the name; others are
    considered to be aliases. We have to scan twice, in order to find out the
    number of aliases. However, if all the names are empty, we will behave as
    if failure. (PTR records that yield empty names have been encountered in
    the DNS.) */

    if (rc == DNS_SUCCEED)
      {
      uschar **aptr = NULL;
      int ssize = 264;
      int count = 0;
      int old_pool = store_pool;

      store_pool = POOL_PERM;        /* Save names in permanent storage */

      for (rr = dns_next_rr(&dnsa, &dnss, RESET_ANSWERS);
           rr != NULL;
           rr = dns_next_rr(&dnsa, &dnss, RESET_NEXT))
        {
        if (rr->type == T_PTR) count++;
        }

      /* Get store for the list of aliases. For compatibility with
      gethostbyaddr, we make an empty list if there are none. */

      aptr = sender_host_aliases = store_get(count * sizeof(uschar *));

      /* Re-scan and extract the names */

      for (rr = dns_next_rr(&dnsa, &dnss, RESET_ANSWERS);
           rr != NULL;
           rr = dns_next_rr(&dnsa, &dnss, RESET_NEXT))
        {
        uschar *s = NULL;
        if (rr->type != T_PTR) continue;
        s = store_get(ssize);

        /* If an overlong response was received, the data will have been
        truncated and dn_expand may fail. */

        if (dn_expand(dnsa.answer, dnsa.answer + dnsa.answerlen,
             (uschar *)(rr->data), (DN_EXPAND_ARG4_TYPE)(s), ssize) < 0)
          {
          log_write(0, LOG_MAIN, "host name alias list truncated for %s",
            sender_host_address);
          break;
          }

        store_reset(s + Ustrlen(s) + 1);
        if (s[0] == 0)
          {
          HDEBUG(D_host_lookup) debug_printf("IP address lookup yielded an "
            "empty name: treated as non-existent host name\n");
          continue;
          }
        if (sender_host_name == NULL) sender_host_name = s;
          else *aptr++ = s;
        while (*s != 0) { *s = tolower(*s); s++; }
        }

      *aptr = NULL;            /* End of alias list */
      store_pool = old_pool;   /* Reset store pool */

      /* If we've found a names, break out of the "order" loop */

      if (sender_host_name != NULL) break;
      }

    /* If the DNS lookup deferred, we must also defer. */

    if (rc == DNS_AGAIN)
      {
      HDEBUG(D_host_lookup)
        debug_printf("IP address PTR lookup gave temporary error\n");
      return DEFER;
      }
    }

  /* Do a lookup using gethostbyaddr() - or equivalent */

  else if (strcmpic(ordername, US"byaddr") == 0)
    {
    HDEBUG(D_host_lookup)
      debug_printf("IP address lookup using gethostbyaddr()\n");

    rc = host_name_lookup_byaddr();
    if (rc == DEFER) return rc;        /* Can't carry on */
    if (rc == OK) break;               /* Found a name */
    }
  }      /* Loop for bydns/byaddr scanning */

/* If we have failed to find a name, return FAIL and log when required.
NB host_lookup_msg must be in permanent store.  */

if (sender_host_name == NULL)
  {
  if (host_checking || !log_testing_mode)
    log_write(L_host_lookup_failed, LOG_MAIN, "no host name found for IP "
      "address %s", sender_host_address);
  host_lookup_msg = US" (failed to find host name from IP address)";

host_lookup_failed = TRUE;
  return FAIL;
  }

/* We have a host name. If we are running in the test harness, we want the host
name and its alias to appear always the same way round. There are only ever two
names in these tests. If one of them contains "alias", make sure it is second;
otherwise put them in alphabetical order. */

if (running_in_test_harness && *sender_host_aliases != NULL &&
    (
    Ustrstr(sender_host_name, "alias") != NULL ||
      (
      Ustrstr(*sender_host_aliases, "alias") == NULL &&
      Ustrcmp(sender_host_name, *sender_host_aliases) > 0
      )
    ))
  {
  uschar *temp = sender_host_name;
  sender_host_name = *sender_host_aliases;
  *sender_host_aliases = temp;
  }

/* Debug output what was found, after test harness swapping, for consistency */

HDEBUG(D_host_lookup)
  {
  uschar **aliases = sender_host_aliases;
  debug_printf("IP address lookup yielded %s\n", sender_host_name);
  while (*aliases != NULL) debug_printf("  alias %s\n", *aliases++);
  }

/* We need to verify that a forward lookup on the name we found does indeed
correspond to the address. This is for security: in principle a malefactor who
happened to own a reverse zone could set it to point to any names at all.

This code was present in versions of Exim before 3.20. At that point I took it
out because I thought that gethostbyaddr() did the check anyway. It turns out
that this isn't always the case, so it's coming back in at 4.01. This version
is actually better, because it also checks aliases.

The code was made more robust at release 4.21. Prior to that, it accepted all
the names if any of them had the correct IP address. Now the code checks all
the names, and accepts only those that have the correct IP address. */

save_hostname = sender_host_name;   /* Save for error messages */
aliases = sender_host_aliases;
for (hname = sender_host_name; hname != NULL; hname = *aliases++)
  {
  int rc;
  BOOL ok = FALSE;
  host_item h;
  h.next = NULL;
  h.name = hname;
  h.mx = MX_NONE;
  h.address = NULL;

  /* When called with the 5th argument FALSE, host_find_byname() won't return
  HOST_FOUND_LOCAL. If the incoming address is an IPv4 address expressed in
  IPv6 format, we must compare the IPv4 part to any IPv4 addresses. */

  if ((rc = host_find_byname(&h, NULL, NULL, FALSE)) == HOST_FOUND)
    {
    host_item *hh;
    uschar *address_ipv4 = (Ustrncmp(sender_host_address, "::ffff:", 7) == 0)?
      sender_host_address + 7 : sender_host_address;
    HDEBUG(D_host_lookup) debug_printf("checking addresses for %s\n", hname);
    for (hh = &h; hh != NULL; hh = hh->next)
      {
      if ((Ustrcmp(hh->address, (Ustrchr(hh->address, ':') == NULL)?
          address_ipv4 : sender_host_address)) == 0)
        {
        HDEBUG(D_host_lookup) debug_printf("  %s OK\n", hh->address);
        ok = TRUE;
        break;
        }
      else
        {
        HDEBUG(D_host_lookup) debug_printf("  %s\n", hh->address);
        }
      }
    if (!ok) HDEBUG(D_host_lookup)
      debug_printf("no IP address for %s matched %s\n", hname,
        sender_host_address);
    }
  else if (rc == HOST_FIND_AGAIN)
    {
    HDEBUG(D_host_lookup) debug_printf("temporary error for host name lookup\n");
    return DEFER;
    }
  else
    {
    HDEBUG(D_host_lookup) debug_printf("no IP addresses found for %s\n", hname);
    }

  /* If this name is no good, and it's the sender name, set it null pro tem;
  if it's an alias, just remove it from the list. */

  if (!ok)
    {
    if (hname == sender_host_name) sender_host_name = NULL; else
      {
      uschar **a;                              /* Don't amalgamate - some */
      a = --aliases;                           /* compilers grumble */
      while (*a != NULL) { *a = a[1]; a++; }
      }
    }
  }

/* If sender_host_name == NULL, it means we didn't like the name. Replace
it with the first alias, if there is one. */

if (sender_host_name == NULL && *sender_host_aliases != NULL)
  sender_host_name = *sender_host_aliases++;

/* If we now have a main name, all is well. */

if (sender_host_name != NULL) return OK;

/* We have failed to find an address that matches. */

HDEBUG(D_host_lookup)
  debug_printf("%s does not match any IP address for %s\n",
    sender_host_address, save_hostname);

/* This message must be in permanent store */

old_pool = store_pool;
store_pool = POOL_PERM;
host_lookup_msg = string_sprintf(" (%s does not match any IP address for %s)",
  sender_host_address, save_hostname);
store_pool = old_pool;

host_lookup_failed = TRUE;
return FAIL;
}




/*************************************************
*    Find IP address(es) for host by name        *
*************************************************/

/* The input is a host_item structure with the name filled in and the address
field set to NULL. We use gethostbyname(). Of course, gethostbyname() may use
the DNS, but it doesn't do MX processing. If more than one address is given,
chain on additional host items, with other relevant fields copied.

The second argument provides a host list (usually an IP list) of hosts to
ignore. This makes it possible to ignore IPv6 link-local addresses or loopback
addresses in unreasonable places.

The lookup may result in a change of name. For compatibility with the dns
lookup, return this via fully_qualified_name as well as updating the host item.
The lookup may also yield more than one IP address, in which case chain on
subsequent host_item structures.

Arguments:
  host                   a host item with the name and MX filled in;
                           the address is to be filled in;
                           multiple IP addresses cause other host items to be
                             chained on.
  ignore_target_hosts    a list of hosts to ignore
  fully_qualified_name   if not NULL, set to point to host name for
                         compatibility with host_find_bydns
  local_host_check       TRUE if a check for the local host is wanted

Returns:                 HOST_FIND_FAILED  Failed to find the host or domain
                         HOST_FIND_AGAIN   Try again later
                         HOST_FOUND        Host found - data filled in
                         HOST_FOUND_LOCAL  Host found and is the local host
*/

int
host_find_byname(host_item *host, uschar *ignore_target_hosts,
  uschar **fully_qualified_name, BOOL local_host_check)
{
int i, yield, times;
uschar **addrlist;
host_item *last = NULL;
BOOL temp_error = FALSE;

/* In an IPv6 world, we need to scan for both kinds of address, so go round the
loop twice. Note that we have ensured that AF_INET6 is defined even in an IPv4
world, which makes for slightly tidier code. However, if dns_ipv4_lookup
matches the domain, we also just do IPv4 lookups here (except when testing
standalone). */

#if HAVE_IPV6
  int af;

  #ifndef STAND_ALONE
  if (dns_ipv4_lookup != NULL &&
        match_isinlist(host->name, &dns_ipv4_lookup, 0, NULL, NULL, MCL_DOMAIN,
          TRUE, NULL) == OK)
    { af = AF_INET; times = 1; }
  else
  #endif  /* STAND_ALONE */

    { af = AF_INET6; times = 2; }

/* No IPv6 support */

#else   /* HAVE_IPV6 */
  times = 1;
#endif  /* HAVE_IPV6 */

/* Initialize the flag that gets set for DNS syntax check errors, so that the
interface to this function can be similar to host_find_bydns. */

host_find_failed_syntax = FALSE;

/* Loop to look up both kinds of address in an IPv6 world */

for (i = 1; i <= times;
     #if HAVE_IPV6
       af = AF_INET,     /* If 2 passes, IPv4 on the second */
     #endif
     i++)
  {
  BOOL ipv4_addr;
  int error_num;
  struct hostent *hostdata;

  #if HAVE_IPV6
    #if HAVE_GETIPNODEBYNAME
    hostdata = getipnodebyname(CS host->name, af, 0, &error_num);
    #else
    hostdata = gethostbyname2(CS host->name, af);
    error_num = h_errno;
    #endif
  #else
  hostdata = gethostbyname(CS host->name);
  error_num = h_errno;
  #endif

  if (hostdata == NULL)
    {
    uschar *error;
    switch (error_num)
      {
      case HOST_NOT_FOUND: error = US"HOST_NOT_FOUND"; break;
      case TRY_AGAIN: error = US"TRY_AGAIN"; break;
      case NO_RECOVERY: error = US"NO_RECOVERY"; break;
      case NO_DATA: error = US"NO_DATA"; break;
      #if NO_DATA != NO_ADDRESS
      case NO_ADDRESS: error = US"NO_ADDRESS"; break;
      #endif
      default: error = US"?"; break;
      }

    DEBUG(D_host_lookup) debug_printf("%s returned %d (%s)\n",
      #if HAVE_IPV6
        #if HAVE_GETIPNODEBYNAME
        (af == AF_INET6)? "getipnodebyname(af=inet6)" : "getipnodebyname(af=inet)",
        #else
        (af == AF_INET6)? "gethostbyname2(af=inet6)" : "gethostbyname2(af=inet)",
        #endif
      #else
      "gethostbyname",
      #endif
      error_num, error);

    if (error_num == TRY_AGAIN || error_num == NO_RECOVERY) temp_error = TRUE;
    continue;
    }
  if ((hostdata->h_addr_list)[0] == NULL) continue;

  /* Replace the name with the fully qualified one if necessary, and fill in
  the fully_qualified_name pointer. */

  if (hostdata->h_name[0] != 0 &&
      Ustrcmp(host->name, hostdata->h_name) != 0)
    host->name = string_copy_dnsdomain((uschar *)hostdata->h_name);
  if (fully_qualified_name != NULL) *fully_qualified_name = host->name;

  /* Get the list of addresses. IPv4 and IPv6 addresses can be distinguished
  by their different lengths. Scan the list, ignoring any that are to be
  ignored, and build a chain from the rest. */

  ipv4_addr = hostdata->h_length == sizeof(struct in_addr);

  for (addrlist = USS hostdata->h_addr_list; *addrlist != NULL; addrlist++)
    {
    uschar *text_address =
      host_ntoa(ipv4_addr? AF_INET:AF_INET6, *addrlist, NULL, NULL);

    #ifndef STAND_ALONE
    if (ignore_target_hosts != NULL &&
        verify_check_this_host(&ignore_target_hosts, NULL, host->name,
          text_address, NULL) == OK)
      {
      DEBUG(D_host_lookup)
        debug_printf("ignored host %s [%s]\n", host->name, text_address);
      continue;
      }
    #endif

    /* If this is the first address, last == NULL and we put the data in the
    original block. */

    if (last == NULL)
      {
      host->address = text_address;
      host->port = PORT_NONE;
      host->status = hstatus_unknown;
      host->why = hwhy_unknown;
      last = host;
      }

    /* Else add further host item blocks for any other addresses, keeping
    the order. */

    else
      {
      host_item *next = store_get(sizeof(host_item));
      next->name = host->name;
      next->mx = host->mx;
      next->address = text_address;
      next->port = PORT_NONE;
      next->status = hstatus_unknown;
      next->why = hwhy_unknown;
      next->last_try = 0;
      next->next = last->next;
      last->next = next;
      last = next;
      }
    }
  }

/* If no hosts were found, the address field in the original host block will be
NULL. If temp_error is set, at least one of the lookups gave a temporary error,
so we pass that back. */

if (host->address == NULL)
  {
  uschar *msg =
    #ifndef STAND_ALONE
    (message_id[0] == 0 && smtp_in != NULL)?
      string_sprintf("no IP address found for host %s (during %s)", host->name,
          smtp_get_connection_info()) :
    #endif
    string_sprintf("no IP address found for host %s", host->name);

  HDEBUG(D_host_lookup) debug_printf("%s\n", msg);
  if (temp_error) return HOST_FIND_AGAIN;
  if (host_checking || !log_testing_mode)
    log_write(L_host_lookup_failed, LOG_MAIN, "%s", msg);
  return HOST_FIND_FAILED;
  }

/* Remove any duplicate IP addresses, then check to see if this is the local
host if required. */

host_remove_duplicates(host, &last);
yield = local_host_check?
  host_scan_for_local_hosts(host, &last, NULL) : HOST_FOUND;

/* When running in the test harness, sort into the order of addresses so as to
get repeatability. */

if (running_in_test_harness) sort_addresses(host, last);

HDEBUG(D_host_lookup)
  {
  host_item *h;
  if (fully_qualified_name != NULL)
    debug_printf("fully qualified name = %s\n", *fully_qualified_name);
  debug_printf("%s looked up these IP addresses:\n",
    #if HAVE_IPV6
      #if HAVE_GETIPNODEBYNAME
      "getipnodebyname"
      #else
      "gethostbyname2"
      #endif
    #else
    "gethostbyname"
    #endif
    );
  for (h = host; h != last->next; h = h->next)
    debug_printf("  name=%s address=%s\n", h->name,
      (h->address == NULL)? US"<null>" : h->address);
  }

/* Return the found status. */

return yield;
}



/*************************************************
*        Fill in a host address from the DNS     *
*************************************************/

/* Given a host item, with its name and mx fields set, and its address field
set to NULL, fill in its IP address from the DNS. If it is multi-homed, create
additional host items for the additional addresses, copying all the other
fields, and randomizing the order.

On IPv6 systems, A6 records are sought first (but only if support for A6 is
configured - they may never become mainstream), then AAAA records are sought,
and finally A records are sought as well.

The host name may be changed if the DNS returns a different name - e.g. fully
qualified or changed via CNAME. If fully_qualified_name is not NULL, dns_lookup
ensures that it points to the fully qualified name. However, this is the fully
qualified version of the original name; if a CNAME is involved, the actual
canonical host name may be different again, and so we get it directly from the
relevant RR. Note that we do NOT change the mx field of the host item in this
function as it may be called to set the addresses of hosts taken from MX
records.

Arguments:
  host                  points to the host item we're filling in
  lastptr               points to pointer to last host item in a chain of
                          host items (may be updated if host is last and gets
                          extended because multihomed)
  ignore_target_hosts   list of hosts to ignore
  allow_ip              if TRUE, recognize an IP address and return it
  fully_qualified_name  if not NULL, return fully qualified name here if
                          the contents are different (i.e. it must be preset
                          to something)

Returns:       HOST_FIND_FAILED     couldn't find A record
               HOST_FIND_AGAIN      try again later
               HOST_FOUND           found AAAA and/or A record(s)
               HOST_IGNORED         found, but all IPs ignored
*/

static int
set_address_from_dns(host_item *host, host_item **lastptr,
  uschar *ignore_target_hosts, BOOL allow_ip, uschar **fully_qualified_name)
{
dns_record *rr;
host_item *thishostlast = NULL;    /* Indicates not yet filled in anything */
BOOL v6_find_again = FALSE;
int i;

/* If allow_ip is set, a name which is an IP address returns that value
as its address. This is used for MX records when allow_mx_to_ip is set, for
those sites that feel they have to flaunt the RFC rules. */

if (allow_ip && string_is_ip_address(host->name, NULL) != 0)
  {
  #ifndef STAND_ALONE
  if (ignore_target_hosts != NULL &&
        verify_check_this_host(&ignore_target_hosts, NULL, host->name,
        host->name, NULL) == OK)
    return HOST_IGNORED;
  #endif

  host->address = host->name;
  host->port = PORT_NONE;
  return HOST_FOUND;
  }

/* On an IPv6 system, go round the loop up to three times, looking for A6 and
AAAA records the first two times. However, unless doing standalone testing, we
force an IPv4 lookup if the domain matches dns_ipv4_lookup is set. Since A6
records look like being abandoned, support them only if explicitly configured
to do so. On an IPv4 system, go round the loop once only, looking only for A
records. */

#if HAVE_IPV6

  #ifndef STAND_ALONE
    if (dns_ipv4_lookup != NULL &&
        match_isinlist(host->name, &dns_ipv4_lookup, 0, NULL, NULL, MCL_DOMAIN,
        TRUE, NULL) == OK)
      i = 0;    /* look up A records only */
    else
  #endif        /* STAND_ALONE */

  #ifdef SUPPORT_A6
  i = 2;        /* look up A6 and AAAA and A records */
  #else
  i = 1;        /* look up AAAA and A records */
  #endif        /* SUPPORT_A6 */

/* The IPv4 world */

#else           /* HAVE_IPV6 */
  i = 0;        /* look up A records only */
#endif          /* HAVE_IPV6 */

for (; i >= 0; i--)
  {
  static int types[] = { T_A, T_AAAA, T_A6 };
  int type = types[i];
  int randoffset = (i == 0)? 500 : 0;  /* Ensures v6 sorts before v4 */
  dns_answer dnsa;
  dns_scan dnss;

  int rc = dns_lookup(&dnsa, host->name, type, fully_qualified_name);

  /* We want to return HOST_FIND_AGAIN if one of the A, A6, or AAAA lookups
  fails or times out, but not if another one succeeds. (In the early
  IPv6 days there are name servers that always fail on AAAA, but are happy
  to give out an A record. We want to proceed with that A record.) */
  
  if (rc != DNS_SUCCEED)
    {
    if (i == 0)  /* Just tried for an A record, i.e. end of loop */
      {
      if (host->address != NULL) return HOST_FOUND;  /* A6 or AAAA was found */
      if (rc == DNS_AGAIN || rc == DNS_FAIL || v6_find_again)
        return HOST_FIND_AGAIN;
      return HOST_FIND_FAILED;    /* DNS_NOMATCH or DNS_NODATA */
      }

    /* Tried for an A6 or AAAA record: remember if this was a temporary
    error, and look for the next record type. */

    if (rc != DNS_NOMATCH && rc != DNS_NODATA) v6_find_again = TRUE;
    continue;
    }

  /* Lookup succeeded: fill in the given host item with the first non-ignored
  address found; create additional items for any others. A single A6 record
  may generate more than one address. */

  for (rr = dns_next_rr(&dnsa, &dnss, RESET_ANSWERS);
       rr != NULL;
       rr = dns_next_rr(&dnsa, &dnss, RESET_NEXT))
    {
    if (rr->type == type)
      {
      /* dns_address *da = dns_address_from_rr(&dnsa, rr); */

      dns_address *da;
      da = dns_address_from_rr(&dnsa, rr);

      DEBUG(D_host_lookup)
        {
        if (da == NULL)
          debug_printf("no addresses extracted from A6 RR for %s\n",
            host->name);
        }

      /* This loop runs only once for A and AAAA records, but may run
      several times for an A6 record that generated multiple addresses. */

      for (; da != NULL; da = da->next)
        {
        #ifndef STAND_ALONE
        if (ignore_target_hosts != NULL &&
              verify_check_this_host(&ignore_target_hosts, NULL,
                host->name, da->address, NULL) == OK)
          {
          DEBUG(D_host_lookup)
            debug_printf("ignored host %s [%s]\n", host->name, da->address);
          continue;
          }
        #endif

        /* If this is the first address, stick it in the given host block,
        and change the name if the returned RR has a different name. */

        if (thishostlast == NULL)
          {
          if (strcmpic(host->name, rr->name) != 0)
            host->name = string_copy_dnsdomain(rr->name);
          host->address = da->address;
          host->port = PORT_NONE;
          host->sort_key = host->mx * 1000 + random_number(500) + randoffset;
          host->status = hstatus_unknown;
          host->why = hwhy_unknown;
          thishostlast = host;
          }

        /* Not the first address. Check for, and ignore, duplicates. Then
        insert in the chain at a random point. */

        else
          {
          int new_sort_key;
          host_item *next;

          /* End of our local chain is specified by "thishostlast". */

          for (next = host;; next = next->next)
            {
            if (Ustrcmp(CS da->address, next->address) == 0) break;
            if (next == thishostlast) { next = NULL; break; }
            }
          if (next != NULL) continue;  /* With loop for next address */

          /* Not a duplicate */

          new_sort_key = host->mx * 1000 + random_number(500) + randoffset;
          next = store_get(sizeof(host_item));

          /* New address goes first: insert the new block after the first one
          (so as not to disturb the original pointer) but put the new address
          in the original block. */

          if (new_sort_key < host->sort_key)
            {
            *next = *host;
            host->next = next;
            host->address = da->address;
            host->port = PORT_NONE;
            host->sort_key = new_sort_key;
            if (thishostlast == host) thishostlast = next;  /* Local last */
            if (*lastptr == host) *lastptr = next;          /* Global last */
            }

          /* Otherwise scan down the addresses for this host to find the
          one to insert after. */

          else
            {
            host_item *h = host;
            while (h != thishostlast)
              {
              if (new_sort_key < h->next->sort_key) break;
              h = h->next;
              }
            *next = *h;
            h->next = next;
            next->address = da->address;
            next->port = PORT_NONE;
            next->sort_key = new_sort_key;
            if (h == thishostlast) thishostlast = next; /* Local last */
            if (h == *lastptr) *lastptr = next;         /* Global last */
            }
          }
        }
      }
    }
  }

/* Control gets here only if the third lookup (the A record) succeeded.
However, the address may not be filled in if it was ignored. */

return (host->address == NULL)? HOST_IGNORED : HOST_FOUND;
}




/*************************************************
*  Find IP addresses and names for host via DNS  *
*************************************************/

/* The input is a host_item structure with the name filled in and the address
field set to NULL. This may be in a chain of other host items. The lookup may
result in more than one IP address, in which case we must created new host
blocks for the additional addresses, and insert them into the chain. The
original name may not be fully qualified. Use the fully_qualified_name argument
to return the official name, as returned by the resolver.

Arguments:
  host                  point to initial host item
  ignore_target_hosts   a list of hosts to ignore
  whichrrs              flags indicating which RRs to look for:
                          HOST_FIND_BY_SRV  => look for SRV
                          HOST_FIND_BY_MX   => look for MX
                          HOST_FIND_BY_A    => look for A or AAAA
                        also flags indicating how the lookup is done
                          HOST_FIND_QUALIFY_SINGLE   ) passed to the
                          HOST_FIND_SEARCH_PARENTS   )   resolver
  srv_service           when SRV used, the service name
  srv_fail_domains      DNS errors for these domains => assume nonexist
  mx_fail_domains       DNS errors for these domains => assume nonexist
  fully_qualified_name  if not NULL, return fully-qualified name
  removed               set TRUE if local host was removed from the list

Returns:                HOST_FIND_FAILED  Failed to find the host or domain;
                                          if there was a syntax error,
                                          host_find_failed_syntax is set.
                        HOST_FIND_AGAIN   Could not resolve at this time
                        HOST_FOUND        Host found
                        HOST_FOUND_LOCAL  The lowest MX record points to this
                                          machine, if MX records were found, or
                                          an A record that was found contains
                                          an address of the local host
*/

int
host_find_bydns(host_item *host, uschar *ignore_target_hosts, int whichrrs,
  uschar *srv_service, uschar *srv_fail_domains, uschar *mx_fail_domains,
  uschar **fully_qualified_name, BOOL *removed)
{
host_item *h, *last;
dns_record *rr;
int rc = DNS_FAIL;
int ind_type = 0;
int yield;
dns_answer dnsa;
dns_scan dnss;

/* Set the default fully qualified name to the incoming name, initialize the
resolver if necessary, set up the relevant options, and initialize the flag
that gets set for DNS syntax check errors. */

if (fully_qualified_name != NULL) *fully_qualified_name = host->name;
dns_init((whichrrs & HOST_FIND_QUALIFY_SINGLE) != 0,
         (whichrrs & HOST_FIND_SEARCH_PARENTS) != 0);
host_find_failed_syntax = FALSE;

/* First, if requested, look for SRV records. The service name is given; we
assume TCP progocol. DNS domain names are constrained to a maximum of 256
characters, so the code below should be safe. */

if ((whichrrs & HOST_FIND_BY_SRV) != 0)
  {
  uschar buffer[300];
  uschar *temp_fully_qualified_name = buffer;
  int prefix_length;

  (void)sprintf(CS buffer, "_%s._tcp.%n%.256s", srv_service, &prefix_length,
    host->name);
  ind_type = T_SRV;

  /* Search for SRV records. If the fully qualified name is different to
  the input name, pass back the new original domain, without the prepended
  magic. */

  rc = dns_lookup(&dnsa, buffer, ind_type, &temp_fully_qualified_name);
  if (temp_fully_qualified_name != buffer && fully_qualified_name != NULL)
    *fully_qualified_name = temp_fully_qualified_name + prefix_length;

  /* On DNS failures, we give the "try again" error unless the domain is
  listed as one for which we continue. */

  if (rc == DNS_FAIL || rc == DNS_AGAIN)
    {
    if (match_isinlist(host->name, &srv_fail_domains, 0, NULL, NULL, MCL_DOMAIN,
        TRUE, NULL) != OK)
      return HOST_FIND_AGAIN;
    DEBUG(D_host_lookup) debug_printf("DNS_%s treated as DNS_NODATA "
      "(domain in srv_fail_domains)\n", (rc == DNS_FAIL)? "FAIL":"AGAIN");
    }
  }

/* If we did not find any SRV records, search the DNS for MX records, if
requested to do so. If the result is DNS_NOMATCH, it means there is no such
domain, and there's no point in going on to look for address records with the
same domain. The result will be DNS_NODATA if the domain exists but has no MX
records. On DNS failures, we give the "try again" error unless the domain is
listed as one for which we continue. */

if (rc != DNS_SUCCEED && (whichrrs & HOST_FIND_BY_MX) != 0)
  {
  ind_type = T_MX;
  rc = dns_lookup(&dnsa, host->name, ind_type, fully_qualified_name);
  if (rc == DNS_NOMATCH) return HOST_FIND_FAILED;
  if (rc == DNS_FAIL || rc == DNS_AGAIN)
    {
    if (match_isinlist(host->name, &mx_fail_domains, 0, NULL, NULL, MCL_DOMAIN,
        TRUE, NULL) != OK)
      return HOST_FIND_AGAIN;
    DEBUG(D_host_lookup) debug_printf("DNS_%s treated as DNS_NODATA "
      "(domain in mx_fail_domains)\n", (rc == DNS_FAIL)? "FAIL":"AGAIN");
    }
  }

/* If we haven't found anything yet, and we are requested to do so, try for an
A or AAAA record. If we find it (or them) check to see that it isn't the local
host. */

if (rc != DNS_SUCCEED)
  {
  if ((whichrrs & HOST_FIND_BY_A) == 0)
    {
    DEBUG(D_host_lookup) debug_printf("Address records are not being sought\n");
    return HOST_FIND_FAILED;
    }

  last = host;        /* End of local chainlet */
  host->mx = MX_NONE;
  host->port = PORT_NONE;
  rc = set_address_from_dns(host, &last, ignore_target_hosts, FALSE,
    fully_qualified_name);

  /* If one or more address records have been found, check that none of them
  are local. Since we know the host items all have their IP addresses
  inserted, host_scan_for_local_hosts() can only return HOST_FOUND or
  HOST_FOUND_LOCAL. We do not need to scan for duplicate IP addresses here,
  because set_address_from_dns() removes them. */

  if (rc == HOST_FOUND)
    rc = host_scan_for_local_hosts(host, &last, removed);
  else
    if (rc == HOST_IGNORED) rc = HOST_FIND_FAILED;  /* No special action */

  /* When running in the test harness, sort into the order of addresses so as
  to get repeatability. */
  
  if (running_in_test_harness) sort_addresses(host, last);

  DEBUG(D_host_lookup)
    {
    host_item *h;
    if (host->address != NULL)
      {
      if (fully_qualified_name != NULL)
        debug_printf("fully qualified name = %s\n", *fully_qualified_name);
      for (h = host; h != last->next; h = h->next)
        debug_printf("%s %s mx=%d sort=%d %s\n", h->name,
          (h->address == NULL)? US"<null>" : h->address, h->mx, h->sort_key,
          (h->status >= hstatus_unusable)? US"*" : US"");
      }
    }

  return rc;
  }

/* We have found one or more MX or SRV records. Sort them according to
precedence. Put the data for the first one into the existing host block, and
insert new host_item blocks into the chain for the remainder. For equal
precedences one is supposed to randomize the order. To make this happen, the
sorting is actually done on the MX value * 1000 + a random number. This is put
into a host field called sort_key.

In the case of hosts with both IPv6 and IPv4 addresses, we want to choose the
IPv6 address in preference. At this stage, we don't know what kind of address
the host has. We choose a random number < 500; if later we find an A record
first, we add 500 to the random number. Then for any other address records, we
use random numbers in the range 0-499 for AAAA records and 500-999 for A
records.

At this point we remove any duplicates that point to the same host, retaining
only the one with the lowest precedence. We cannot yet check for precedence
greater than that of the local host, because that test cannot be properly done
until the addresses have been found - an MX record may point to a name for this
host which is not the primary hostname. */

last = NULL;    /* Indicates that not even the first item is filled yet */

for (rr = dns_next_rr(&dnsa, &dnss, RESET_ANSWERS);
     rr != NULL;
     rr = dns_next_rr(&dnsa, &dnss, RESET_NEXT))
  {
  int precedence;
  int weight = 0;        /* For SRV records */
  int port = PORT_NONE;  /* For SRV records */
  uschar *s;             /* MUST be unsigned for GETSHORT */
  uschar data[256];

  if (rr->type != ind_type) continue;
  s = rr->data;
  GETSHORT(precedence, s);      /* Pointer s is advanced */

  /* For MX records, we use a random "weight" which causes multiple records of
  the same precedence to sort randomly. */

  if (ind_type == T_MX)
    {
    weight = random_number(500);
    }

  /* SRV records are specified with a port and a weight. The weight is used
  in a special algorithm. However, to start with, we just use it to order the
  records of equal priority (precedence). */

  else
    {
    GETSHORT(weight, s);
    GETSHORT(port, s);
    }

  /* Get the name of the host pointed to. */

  (void)dn_expand(dnsa.answer, dnsa.answer + dnsa.answerlen, s,
    (DN_EXPAND_ARG4_TYPE)data, sizeof(data));

  /* Check that we haven't already got this host on the chain; if we have,
  keep only the lower precedence. This situation shouldn't occur, but you
  never know what junk might get into the DNS (and this case has been seen on
  more than one occasion). */

  if (last != NULL)       /* This is not the first record */
    {
    host_item *prev = NULL;

    for (h = host; h != last->next; prev = h, h = h->next)
      {
      if (strcmpic(h->name, data) == 0)
        {
        DEBUG(D_host_lookup)
          debug_printf("discarded duplicate host %s (MX=%d)\n", data,
            (precedence > h->mx)? precedence : h->mx);
        if (precedence >= h->mx) goto NEXT_MX_RR; /* Skip greater precedence */
        if (h == host)                            /* Override first item */
          {
          h->mx = precedence;
          host->sort_key = precedence * 1000 + weight;
          goto NEXT_MX_RR;
          }

        /* Unwanted host item is not the first in the chain, so we can get
        get rid of it by cutting it out. */

        prev->next = h->next;
        if (h == last) last = prev;
        break;
        }
      }
    }

  /* If this is the first MX or SRV record, put the data into the existing host
  block. Otherwise, add a new block in the correct place; if it has to be
  before the first block, copy the first block's data to a new second block. */

  if (last == NULL)
    {
    host->name = string_copy_dnsdomain(data);
    host->address = NULL;
    host->port = port;
    host->mx = precedence;
    host->sort_key = precedence * 1000 + weight;
    host->status = hstatus_unknown;
    host->why = hwhy_unknown;
    last = host;
    }

  /* Make a new host item and seek the correct insertion place */

  else
    {
    int sort_key = precedence * 1000 + weight;
    host_item *next = store_get(sizeof(host_item));
    next->name = string_copy_dnsdomain(data);
    next->address = NULL;
    next->port = port;
    next->mx = precedence;
    next->sort_key = sort_key;
    next->status = hstatus_unknown;
    next->why = hwhy_unknown;
    next->last_try = 0;

    /* Handle the case when we have to insert before the first item. */

    if (sort_key < host->sort_key)
      {
      host_item htemp;
      htemp = *host;
      *host = *next;
      *next = htemp;
      host->next = next;
      if (last == host) last = next;
      }

    /* Else scan down the items we have inserted as part of this exercise;
    don't go further. */

    else
      {
      for (h = host; h != last; h = h->next)
        {
        if (sort_key < h->next->sort_key)
          {
          next->next = h->next;
          h->next = next;
          break;
          }
        }

      /* Join on after the last host item that's part of this
      processing if we haven't stopped sooner. */

      if (h == last)
        {
        next->next = last->next;
        last->next = next;
        last = next;
        }
      }
    }

  NEXT_MX_RR: continue;
  }

/* If the list of hosts was obtained from SRV records, there are two things to
do. First, if there is only one host, and it's name is ".", it means there is
no SMTP service at this domain. Otherwise, we have to sort the hosts of equal
priority according to their weights, using an algorithm that is defined in RFC
2782. The hosts are currently sorted by priority and weight. For each priority
group we have to pick off one host and put it first, and then repeat for any
remaining in the same priority group. */

if (ind_type == T_SRV)
  {
  host_item **pptr;

  if (host == last && host->name[0] == 0)
    {
    DEBUG(D_host_lookup) debug_printf("the single SRV record is \".\"\n");
    return HOST_FIND_FAILED;
    }

  DEBUG(D_host_lookup)
    {
    debug_printf("original ordering of hosts from SRV records:\n");
    for (h = host; h != last->next; h = h->next)
      debug_printf("  %s P=%d W=%d\n", h->name, h->mx, h->sort_key % 1000);
    }

  for (pptr = &host, h = host; h != last; pptr = &(h->next), h = h->next)
    {
    int sum = 0;
    host_item *hh;

    /* Find the last following host that has the same precedence. At the same
    time, compute the sum of the weights and the running totals. These can be
    stored in the sort_key field. */

    for (hh = h; hh != last; hh = hh->next)
      {
      int weight = hh->sort_key % 1000;   /* was precedence * 1000 + weight */
      sum += weight;
      hh->sort_key = sum;
      if (hh->mx != hh->next->mx) break;
      }

    /* If there's more than one host at this precedence (priority), we need to
    pick one to go first. */

    if (hh != h)
      {
      host_item *hhh;
      host_item **ppptr;
      int randomizer = random_number(sum + 1);

      for (ppptr = pptr, hhh = h;
           hhh != hh;
           ppptr = &(hhh->next), hhh = hhh->next)
        {
        if (hhh->sort_key >= randomizer) break;
        }

      /* hhh now points to the host that should go first; ppptr points to the
      place that points to it. Unfortunately, if the start of the minilist is
      the start of the entire list, we can't just swap the items over, because
      we must not change the value of host, since it is passed in from outside.
      One day, this could perhaps be changed.

      The special case is fudged by putting the new item *second* in the chain,
      and then transferring the data between the first and second items. We
      can't just swap the first and the chosen item, because that would mean
      that an item with zero weight might no longer be first. */

      if (hhh != h)
        {
        *ppptr = hhh->next;          /* Cuts it out of the chain */

        if (h == host)
          {
          host_item temp = *h;
          *h = *hhh;
          *hhh = temp;
          hhh->next = temp.next;
          h->next = hhh;
          }

        else
          {
          hhh->next = h;               /* The rest of the chain follows it */
          *pptr = hhh;                 /* It takes the place of h */
          h = hhh;                     /* It's now the start of this minilist */
          }
        }
      }

    /* A host has been chosen to be first at this priority and h now points
    to this host. There may be others at the same priority, or others at a
    different priority. Before we leave this host, we need to put back a sort
    key of the traditional MX kind, in case this host is multihomed, because
    the sort key is used for ordering the multiple IP addresses. We do not need
    to ensure that these new sort keys actually reflect the order of the hosts,
    however. */

    h->sort_key = h->mx * 1000 + random_number(500);
    }   /* Move on to the next host */
  }

/* Now we have to ensure addresses exist for all the hosts. We have ensured
above that the names in the host items are all unique. The addresses may have
been returned in the additional data section of the DNS query. Because it is
more expensive to scan the returned DNS records (because you have to expand the
names) we do a single scan over them, and multiple scans of the chain of host
items (which is typically only 3 or 4 long anyway.) Add extra host items for
multi-homed hosts. */

for (rr = dns_next_rr(&dnsa, &dnss, RESET_ADDITIONAL);
     rr != NULL;
     rr = dns_next_rr(&dnsa, &dnss, RESET_NEXT))
  {
  dns_address *da;
  int status = hstatus_unknown;
  int why = hwhy_unknown;
  int randoffset;

  if (rr->type != T_A
  #if HAVE_IPV6
    && rr->type != T_AAAA
    #ifdef SUPPORT_A6
    && rr->type != T_A6
    #endif
  #endif
    ) continue;

  /* Find the first host that matches this record's name. If there isn't
  one, move on to the next RR. */

  for (h = host; h != last->next; h = h->next)
    { if (strcmpic(h->name, rr->name) == 0) break; }
  if (h == last->next) continue;

  /* For IPv4 addresses, add 500 to the random part of the sort key, to ensure
  they sort after IPv6 addresses. */

  randoffset = (rr->type == T_A)? 500 : 0;

  /* Get the list of textual addresses for this RR. There may be more than one
  if it is an A6 RR. Then loop to handle multiple addresses from an A6 record.
  If there are none, nothing will get done - the record is ignored. */

  for (da = dns_address_from_rr(&dnsa, rr); da != NULL; da = da->next)
    {
    /* Set status for an ignorable host. */

    #ifndef STAND_ALONE
    if (ignore_target_hosts != NULL &&
          verify_check_this_host(&ignore_target_hosts, NULL, h->name,
            da->address, NULL) == OK)
      {
      DEBUG(D_host_lookup)
        debug_printf("ignored host %s [%s]\n", h->name, da->address);
      status = hstatus_unusable;
      why = hwhy_ignored;
      }
    #endif

    /* If the address is already set for this host, it may be that
    we just have a duplicate DNS record. Alternatively, this may be
    a multi-homed host. Search all items with the same host name
    (they will all be together) and if this address is found, skip
    to the next RR. */

    if (h->address != NULL)
      {
      int new_sort_key;
      host_item *thishostlast;
      host_item *hh = h;

      do
        {
        if (hh->address != NULL && Ustrcmp(CS da->address, hh->address) == 0)
          goto DNS_NEXT_RR;         /* Need goto to escape from inner loop */
        thishostlast = hh;
        hh = hh->next;
        }
      while (hh != last->next && strcmpic(hh->name, rr->name) == 0);

      /* We have a multi-homed host, since we have a new address for
      an existing name. Create a copy of the current item, and give it
      the new address. RRs can be in arbitrary order, but one is supposed
      to randomize the addresses of multi-homed hosts, so compute a new
      sorting key and do that. [Latest SMTP RFC says not to randomize multi-
      homed hosts, but to rely on the resolver. I'm not happy about that -
      caching in the resolver will not rotate as often as the name server
      does.] */

      new_sort_key = h->mx * 1000 + random_number(500) + randoffset;
      hh = store_get(sizeof(host_item));

      /* New address goes first: insert the new block after the first one
      (so as not to disturb the original pointer) but put the new address
      in the original block. */

      if (new_sort_key < h->sort_key)
        {
        *hh = *h;                       /* Note: copies the port */
        h->next = hh;
        h->address = da->address;
        h->sort_key = new_sort_key;
        h->status = status;
        h->why = why;
        }

      /* Otherwise scan down the addresses for this host to find the
      one to insert after. */

      else
        {
        while (h != thishostlast)
          {
          if (new_sort_key < h->next->sort_key) break;
          h = h->next;
          }
        *hh = *h;                       /* Note: copies the port */
        h->next = hh;
        hh->address = da->address;
        hh->sort_key = new_sort_key;
        hh->status = status;
        hh->why = why;
        }

      if (h == last) last = hh;         /* Inserted after last */
      }

    /* The existing item doesn't have its address set yet, so just set it.
    Ensure that an IPv4 address gets its sort key incremented in case an IPv6
    address is found later. */

    else
      {
      h->address = da->address;         /* Port should be set already */
      h->status = status;
      h->why = why;
      h->sort_key += randoffset;
      }
    }    /* Loop for addresses extracted from one RR */

  /* Carry on to the next RR. It would be nice to be able to be able to stop
  when every host on the list has an address, but we can't be sure there won't
  be an additional address for a multi-homed host further down the list, so
  we have to continue to the end. */

  DNS_NEXT_RR: continue;
  }

/* Set the default yield to failure */

yield = HOST_FIND_FAILED;

/* If we haven't found all the addresses in the additional section, we
need to search for A or AAAA records explicitly. The names shouldn't point to
CNAMES, but we use the general lookup function that handles them, just
in case. If any lookup gives a soft error, change the default yield.

For these DNS lookups, we must disable qualify_single and search_parents;
otherwise invalid host names obtained from MX or SRV records can cause trouble
if they happen to match something local. */

dns_init(FALSE, FALSE);

for (h = host; h != last->next; h = h->next)
  {
  if (h->address != NULL || h->status == hstatus_unusable) continue;
  rc = set_address_from_dns(h, &last, ignore_target_hosts, allow_mx_to_ip, NULL);
  if (rc != HOST_FOUND)
    {
    h->status = hstatus_unusable;
    if (rc == HOST_FIND_AGAIN)
      {
      yield = rc;
      h->why = hwhy_deferred;
      }
    else
      h->why = (rc == HOST_IGNORED)? hwhy_ignored : hwhy_failed;
    }
  }

/* Scan the list for any hosts that are marked unusable because they have
been explicitly ignored, and remove them from the list, as if they did not
exist. If we end up with just a single, ignored host, flatten its fields as if
nothing was found. */

if (ignore_target_hosts != NULL)
  {
  host_item *prev = NULL;
  for (h = host; h != last->next; h = h->next)
    {
    REDO:
    if (h->why != hwhy_ignored)        /* Non ignored host, just continue */
      prev = h;
    else if (prev == NULL)             /* First host is ignored */
      {
      if (h != last)                   /* First is not last */
        {
        if (h->next == last) last = h; /* Overwrite it with next */
        *h = *(h->next);               /* and reprocess it. */
        goto REDO;                     /* C should have redo, like Perl */
        }
      }
    else                               /* Ignored host is not first - */
      {                                /*   cut it out */
      prev->next = h->next;
      if (h == last) last = prev;
      }
    }

  if (host->why == hwhy_ignored) host->address = NULL;
  }

/* There is still one complication in the case of IPv6. Although the code above
arranges that IPv6 addresses take precedence over IPv4 addresses for multihomed
hosts, it doesn't do this for addresses that apply to different hosts with the
same MX precedence, because the sorting on MX precedence happens first. So we
have to make another pass to check for this case. We ensure that, within a
single MX preference value, IPv6 addresses come first. This can separate the
addresses of a multihomed host, but that should not matter. */

#if HAVE_IPV6
if (h != last)
  {
  for (h = host; h != last; h = h->next)
    {
    host_item temp;
    host_item *next = h->next;
    if (h->mx != next->mx ||                /* If next is different MX value */
        (h->sort_key % 1000) < 500 ||       /* OR this one is IPv6 */
        (next->sort_key % 1000) >= 500)     /* OR next is IPv4 */
      continue;                             /* move on to next */
    temp = *h;
    temp.next = next->next;
    *h = *next;
    h->next = next;
    *next = temp;
    }
  }
#endif

/* When running in the test harness, we want the hosts always to be in the same
order so that the debugging output is the same and can be compared. Having a
fixed set of "random" numbers doesn't actually achieve this, because the RRs
come back from the resolver in a random order, so the non-random random numbers
get used in a different order. We therefore have to sort the hosts that have
the same MX values. We chose do to this by their name and then by IP address.
The fact that the sort is slow matters not - this is testing only! */

if (running_in_test_harness)
  {
  BOOL done;
  do
    {
    done = TRUE;
    for (h = host; h != last; h = h->next)
      {
      int c = Ustrcmp(h->name, h->next->name);
      if (c == 0) c = Ustrcmp(h->address, h->next->address);
      if (h->mx == h->next->mx && c > 0)
        {
        host_item *next = h->next;
        host_item temp = *h;
        temp.next = next->next;
        *h = *next;
        h->next = next;
        *next = temp;
        done = FALSE;
        }
      }
    }
  while (!done);
  }

/* Remove any duplicate IP addresses and then scan the list of hosts for any
whose IP addresses are on the local host. If any are found, all hosts with the
same or higher MX values are removed. However, if the local host has the lowest
numbered MX, then HOST_FOUND_LOCAL is returned. Otherwise, if at least one host
with an IP address is on the list, HOST_FOUND is returned. Otherwise,
HOST_FIND_FAILED is returned, but in this case do not update the yield, as it
might have been set to HOST_FIND_AGAIN just above here. If not, it will already
be HOST_FIND_FAILED. */

host_remove_duplicates(host, &last);
rc = host_scan_for_local_hosts(host, &last, removed);
if (rc != HOST_FIND_FAILED) yield = rc;

DEBUG(D_host_lookup)
  {
  if (fully_qualified_name != NULL)
    debug_printf("fully qualified name = %s\n", *fully_qualified_name);
  debug_printf("host_find_bydns yield = %s (%d); returned hosts:\n",
    (yield == HOST_FOUND)? "HOST_FOUND" :
    (yield == HOST_FOUND_LOCAL)? "HOST_FOUND_LOCAL" :
    (yield == HOST_FIND_AGAIN)? "HOST_FIND_AGAIN" :
    (yield == HOST_FIND_FAILED)? "HOST_FIND_FAILED" : "?",
    yield);
  for (h = host; h != last->next; h = h->next)
    {
    debug_printf("  %s %s MX=%d ", h->name,
      (h->address == NULL)? US"<null>" : h->address, h->mx);
    if (h->port != PORT_NONE) debug_printf("port=%d ", h->port);
    if (h->status >= hstatus_unusable) debug_printf("*");
    debug_printf("\n");
    }
  }

return yield;
}




/*************************************************
**************************************************
*             Stand-alone test program           *
**************************************************
*************************************************/

#ifdef STAND_ALONE

BOOL alldigits(uschar *buffer)
{
if (!isdigit(*buffer)) return FALSE;
if (*buffer == '0' && buffer[1] == 'x')
  {
  buffer++;
  while (isxdigit(*(++buffer)));
  }
else while (isdigit(*(++buffer)));
return (*buffer == 0);
}

int main(int argc, char **cargv)
{
host_item h;
int whichrrs = HOST_FIND_BY_MX | HOST_FIND_BY_A;
BOOL byname = FALSE;
BOOL qualify_single = TRUE;
BOOL search_parents = FALSE;
uschar **argv = USS cargv;
uschar buffer[256];

primary_hostname = US"";
store_pool = POOL_MAIN;
debug_selector = D_host_lookup|D_interface;
debug_file = stdout;
debug_fd = fileno(debug_file);

printf("Exim stand-alone host functions test\n");

host_find_interfaces();
debug_selector = D_host_lookup | D_dns;

if (argc > 1) primary_hostname = argv[1];

/* So that debug level changes can be done first */

dns_init(qualify_single, search_parents);

printf("Testing host lookup\n");
printf("> ");
while (Ufgets(buffer, 256, stdin) != NULL)
  {
  int rc;
  int len = Ustrlen(buffer);
  uschar *fully_qualified_name;

  while (len > 0 && isspace(buffer[len-1])) len--;
  buffer[len] = 0;

  if (Ustrcmp(buffer, "q") == 0) break;

  if (Ustrcmp(buffer, "byname") == 0) byname = TRUE;
  else if (Ustrcmp(buffer, "no_byname") == 0) byname = FALSE;
  else if (Ustrcmp(buffer, "a_only") == 0) whichrrs = HOST_FIND_BY_A;
  else if (Ustrcmp(buffer, "mx_only") == 0) whichrrs = HOST_FIND_BY_MX;
  else if (Ustrcmp(buffer, "srv_only") == 0) whichrrs = HOST_FIND_BY_SRV;
  else if (Ustrcmp(buffer, "srv+a") == 0)
    whichrrs = HOST_FIND_BY_SRV | HOST_FIND_BY_A;
  else if (Ustrcmp(buffer, "srv+mx") == 0)
    whichrrs = HOST_FIND_BY_SRV | HOST_FIND_BY_MX;
  else if (Ustrcmp(buffer, "srv+mx+a") == 0)
    whichrrs = HOST_FIND_BY_SRV | HOST_FIND_BY_MX | HOST_FIND_BY_A;
  else if (Ustrcmp(buffer, "qualify_single") == 0) qualify_single = TRUE;
  else if (Ustrcmp(buffer, "no_qualify_single") == 0) qualify_single = FALSE;
  else if (Ustrcmp(buffer, "search_parents") == 0) search_parents = TRUE;
  else if (Ustrcmp(buffer, "no_search_parents") == 0) search_parents = FALSE;
  else if (Ustrncmp(buffer, "retrans", 7) == 0)
    {
    sscanf(CS(buffer+8), "%d", &dns_retrans);
    _res.retrans = dns_retrans;
    }
  else if (Ustrncmp(buffer, "retry", 5) == 0)
    {
    sscanf(CS(buffer+6), "%d", &dns_retry);
    _res.retry = dns_retry;
    }
  else if (alldigits(buffer))
    {
    debug_selector = Ustrtol(buffer, NULL, 0);
    _res.options &= ~RES_DEBUG;
    DEBUG(D_resolver) _res.options |= RES_DEBUG;
    }
  else
    {
    int flags = whichrrs;

    h.name = buffer;
    h.next = NULL;
    h.mx = MX_NONE;
    h.port = PORT_NONE;
    h.status = hstatus_unknown;
    h.why = hwhy_unknown;
    h.address = NULL;

    if (qualify_single) flags |= HOST_FIND_QUALIFY_SINGLE;
    if (search_parents) flags |= HOST_FIND_SEARCH_PARENTS;

    rc = byname?
      host_find_byname(&h, NULL, &fully_qualified_name, TRUE)
      :
      host_find_bydns(&h, NULL, flags, US"smtp", NULL, NULL,
        &fully_qualified_name, NULL);

    if (rc == HOST_FIND_FAILED) printf("Failed\n");
      else if (rc == HOST_FIND_AGAIN) printf("Again\n");
        else if (rc == HOST_FOUND_LOCAL) printf("Local\n");
    }

  printf("\n> ");
  }

printf("Testing host_aton\n");
printf("> ");
while (Ufgets(buffer, 256, stdin) != NULL)
  {
  int i;
  int x[4];
  int len = Ustrlen(buffer);

  while (len > 0 && isspace(buffer[len-1])) len--;
  buffer[len] = 0;

  if (Ustrcmp(buffer, "q") == 0) break;

  len = host_aton(buffer, x);
  printf("length = %d ", len);
  for (i = 0; i < len; i++)
    {
    printf("%04x ", (x[i] >> 16) & 0xffff);
    printf("%04x ", x[i] & 0xffff);
    }
  printf("\n> ");
  }

printf("\n");

printf("Testing host_name_lookup\n");
printf("> ");
while (Ufgets(buffer, 256, stdin) != NULL)
  {
  int len = Ustrlen(buffer);
  while (len > 0 && isspace(buffer[len-1])) len--;
  buffer[len] = 0;
  if (Ustrcmp(buffer, "q") == 0) break;
  sender_host_address = buffer;
  sender_host_name = NULL;
  sender_host_aliases = NULL;
  host_lookup_msg = US"";
  host_lookup_failed = FALSE;
  if (host_name_lookup() == FAIL)  /* Debug causes printing */
    printf("Lookup failed:%s\n", host_lookup_msg);
  printf("\n> ");
  }

printf("\n");

return 0;
}
#endif  /* STAND_ALONE */

/* End of host.c */
