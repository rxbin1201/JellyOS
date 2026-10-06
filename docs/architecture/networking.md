# JellyOS Networking

**Code:** [`net/`](../../net/) (stack), [`drivers/network/`](../../drivers/network/) (NIC drivers), [`kernel/syscall/net_syscalls.c`](../../kernel/syscall/net_syscalls.c), [`userspace/services/network/`](../../userspace/services/network/), [`userspace/applications/network/`](../../userspace/applications/network/)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md) (version 4)

Phase 8 (milestone M7) connects JellyOS to the Internet over IPv4.

## Layers (README section 32)

```text
NIC driver            drivers/network/virtio_net.c     (loopback: net/link/loopback.c)
   ↓ netif_ops_t
Network interface     net/link/netif.c                 eth0, lo; IPv4 configuration, counters
   ↓
Ethernet              net/link/netif.c                 IPv4 and ARP frames; others dropped
   ↓
ARP                   net/arp/arp.c                    cache, request retries, packet queue
   ↓
IPv4 + ICMP           net/ipv4/                        routing, header checks, echo, unreachable
   ↓
UDP / TCP             net/udp/, net/tcp/
   ↓
Sockets               net/sockets/socket.c             kernel objects behind handles
   ↓
DNS resolver          net/dns/dns.c                    in the kernel, used through SYS_NET_RESOLVE
   ↓
Applications          libc <sys/socket.h>, networkd, ping, http, nc, ...
```

IPv6 is the next step in the README's implementation order. It is not part
of Phase 8; IPv6 frames are dropped.

## Execution model

- One mutex, `net_lock`, protects all protocol state (interfaces, ARP, TCP
  connections, socket queues).
- **Receiving:** a NIC interrupt only calls `netif_receive_ready()`. The
  **network thread** (`net/link/net.c`) then pulls frames from every
  interface (at most 256 per round), passes them up the stack and runs the
  protocol timers (ARP retries, TCP retransmission, TIME_WAIT). It sleeps
  until the next frame or timer deadline; without timers it does not wake up.
- **Sending** happens in the caller's context: a system call takes
  `net_lock`, and the segment goes down to the driver directly.
- **Blocking:** socket calls sleep on the socket object's wait queue with
  `net_lock` released. Timeouts (`JELLY_SO_RECEIVE_TIMEOUT`, `SEND_TIMEOUT`)
  and process kills (`INTERRUPTED`) end the wait.
- Packet buffers (`netbuf_t`) have 128 bytes of headroom, so every layer
  prepends its header without copying.

## Interfaces

| Interface | Index | Configuration |
|---|---|---|
| `lo` | 0 | 127.0.0.1/8, fixed. MTU 16 KiB |
| `eth0`, `eth1`, ... | 1, ... | none at boot; `networkd` sets it (DHCP or static) through `SYS_NET_CONFIGURE` |

An interface without an address accepts every IPv4 packet sent to it, so a
DHCP client can receive its offer. The DNS server address is stored with the
interface configuration.

### Routing

1. 127/8 and our own addresses go through `lo`.
2. 255.255.255.255 goes to the first Ethernet interface, or to the
   interface the socket is bound to (`JELLY_SO_INTERFACE`).
3. A destination on the subnet of a configured interface is sent directly.
4. Otherwise the packet goes to the first configured gateway.
5. Without a route the call fails with `UNREACHABLE`.

## Protocols

| Protocol | Implemented | Not (yet) |
|---|---|---|
| Ethernet | Ethernet II, padding to 60 bytes | VLANs, multicast filtering |
| ARP | Requests and replies, 32-entry cache (5 min), 3 retries at 1 s, up to 4 queued packets per destination, gratuitous ARP on configuration | ARP probes (address conflict detection) |
| IPv4 | Header checks, Don't Fragment on output, options skipped | Fragmentation and reassembly (fragments are dropped), forwarding |
| ICMP | Echo replies, ping sockets, destination unreachable both ways (port unreachable for UDP, errors to UDP and connecting TCP sockets) | Redirects, time exceeded handling |
| UDP | Checksums, ports, broadcast (with `JELLY_SO_BROADCAST`), connected sockets get ICMP errors | Multicast |
| TCP | See below | Window scaling, SACK, timestamps, out-of-order queue, urgent data |
| DNS | A records, recursion desired, 3 tries with 2 s timeout, 32-entry cache (TTL clamped to 5 s – 1 h, NXDOMAIN cached for 5 s), CNAME answers | AAAA, TCP fallback, search domains |

### TCP

- **Connections:** the full RFC 793 state machine with active and passive
  open, simultaneous open, half close (`SHUT_WR`), RST handling, and a
  challenge ACK for SYNs in the window (RFC 5961). TIME_WAIT lasts 10 s.
  `JELLY_SO_REUSE_ADDRESS` allows binding a port that is still in TIME_WAIT.
- **Closing a socket** leaves an orphaned connection that finishes the FIN
  exchange in the background (FIN_WAIT_2 gives up after 60 s). Closing with
  unread received data sends a reset instead (RFC 2525).
- **Buffers:** 64 KiB send and 64 KiB receive buffer per connection. The
  advertised window is the free receive space (at most 65535). A window
  update is sent when reading frees two segments' worth, or opens a zero
  window.
- **Sending:** segments up to the MSS (from the peer's MSS option, at most
  our MTU minus 40) within the peer's window and the congestion window:
  slow start, congestion avoidance, fast retransmit and fast recovery after
  three duplicate ACKs (RFC 5681). The initial window is 3 segments. Nagle's
  algorithm is not used.
- **Retransmission:** RTO from the smoothed RTT and its variance (RFC 6298,
  at least 200 ms, at most 60 s, Karn's rule), exponential backoff and go
  back N. A connection is reset after 10 retransmissions, a connection
  attempt after 5 SYN retries (about 31 s). Zero windows are probed with
  one byte and never time out.
- **Receiving:** in-order data only. Later segments are dropped and answered
  with a duplicate ACK, which makes the sender retransmit quickly. Every
  segment with data is acknowledged at once (no delayed ACKs).

### Sockets

| Type | Protocol | Notes |
|---|---|---|
| `JELLY_SOCK_STREAM` | TCP | `connect` blocks until established (or `WOULD_BLOCK` when non-blocking) |
| `JELLY_SOCK_DGRAM` | UDP | Datagrams up to 64 KiB minus headers; at most 256 KiB queued per socket. A short receive buffer truncates the datagram |
| `JELLY_SOCK_DGRAM` | ICMP | Ping socket: send an echo request (type 8), and the kernel sets the identifier to the socket's port and fills in the checksum. Matching echo replies are received whole |

A socket handle is waitable (signaled while readable, at end of stream or
on an error) and works with `SYS_FILE_READ` and `SYS_FILE_WRITE`, so
programs can use a connection like a file.

## Drivers

**virtio-net** (`drivers/network/virtio_net.c`, built-in module
`virtio_net`):

- VirtIO 1.x PCI with the features `MAC` and `STATUS`.
- 128 receive buffers of 2 KiB with an MSI-X interrupt. Received frames are
  copied out and the buffer is posted again.
- 128 transmit bounce buffers without an interrupt; finished ones are
  reclaimed on the next send.
- No offloads: checksums are computed by the stack. No mergeable buffers.

The e1000e (and real hardware NICs) follow in Phase 12, on the same
`netif_ops_t` interface.

## Configuration: networkd

`/sbin/networkd` (service `network` in `/etc/services.conf`) configures
every Ethernet interface. The default is DHCP; `/etc/network.conf` can choose
otherwise:

```ini
[interface eth0]
method=static          # dhcp (default) | static | off
address=192.168.1.20/24
gateway=192.168.1.1
dns=192.168.1.1
```

DHCP (RFC 2131): DISCOVER and REQUEST are broadcast from 0.0.0.0:68 over a
socket bound to the interface, with retries after 1, 2, 4 and 8 s. The ACK
supplies the address, netmask, router, DNS server and lease time. The lease
is renewed at half its time; when it expires without renewal, the address
is removed and discovery starts again.

## Tools

| Program | Use |
|---|---|
| `ifconfig [NAME [ADDRESS/PREFIX [gateway G] [dns D] \| down]]` | Show interfaces and counters, or configure statically (root) |
| `ping [-c N] [-i S] [-W S] HOST` | ICMP echo with round-trip times |
| `nslookup NAME...` | Resolve names |
| `http [-v] [-o FILE] URL` | HTTP/1.0 GET, follows redirects; there is no TLS, so `https://` is refused |
| `nc [-u] [-w S] HOST PORT`, `nc -l [-u] PORT` | Netcat: TCP or UDP, client or one-connection server |

## QEMU

`make run` attaches a virtio-net card on QEMU's user network (slirp; turn
it off with `NET=0`):

| Address | Meaning |
|---|---|
| 10.0.2.15 | JellyOS (from the built-in DHCP server) |
| 10.0.2.2 | Gateway; connections to it reach the host's 127.0.0.1 |
| 10.0.2.3 | DNS forwarder to the host's resolver |

The user network answers pings to the gateway. Pings to the Internet only
work when the host allows unprivileged ICMP sockets.
