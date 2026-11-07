/* StratumOS - the network subsystem's lock.
 *
 * One lock covers the whole stack: the interface configuration, the ARP
 * cache, the IPv4 reassembly state, the UDP port table, TCP's connection
 * table and retransmission timers, the socket table, and the DNS and DHCP
 * clients. As with the filesystem, the layers are not separable in any
 * useful way - one received frame walks all of them - and a lock per layer
 * would buy nothing but more orderings to get wrong.
 *
 * It sits at LOCK_LEVEL_NET, above the filesystem's and below the heap's. A
 * socket is a file, so a read through a descriptor takes the filesystem's
 * lock at the boundary and then this one; nothing in the filesystem is ever
 * reached from in here, which is what makes that order safe to fix.
 *
 * Unlike every other lock in this kernel, this one counts its depth.
 *
 * That is a deliberate exception and the reason is the shape of the stack's
 * boundary. There are some ninety entry points into it - the shell drives
 * ARP, ICMP, UDP, TCP, DNS and HTTP directly, and the socket layer drives
 * the same functions from a system call - and they call each other freely:
 * socket_write() calls tcp_send(), which the shell also calls. Splitting
 * each of them into a public form and a `_locked` form, the way the heap and
 * the filesystem are split, would mean ninety duplicated functions and
 * ninety chances to call the wrong one. Counting the depth means an entry
 * point can be called from inside or outside the stack and be correct either
 * way.
 *
 * What that costs is the thing a non-recursive lock buys: the nesting is no
 * longer visible. It is an acceptable trade *here* and nowhere else, because
 * the ordering guarantee that matters - that this lock is taken between the
 * filesystem's and the heap's - is enforced on the outermost acquisition and
 * is unaffected by the depth. The memory stack and the filesystem stay
 * strictly non-recursive, so this is the exception and not the pattern.
 */
#ifndef STRATUM_NET_NETLOCK_H
#define STRATUM_NET_NETLOCK_H

#include <kernel/types.h>

void net_lock(void);
void net_unlock(void);

/* True if this processor holds the network lock, at any depth. */
bool net_lock_held(void);

/* Release the lock around something that blocks, and take it again after.
 *
 * The blocking socket calls poll the stack and then sleep, waiting for a
 * segment that another processor - or this one, from the network thread -
 * has to deliver. Sleeping with this lock held would make that delivery
 * impossible, which is a deadlock dressed as a timeout, and task_sleep_ms()
 * takes the scheduler's lock, which is below this one.
 *
 * The whole nest goes down, not one level of it, and net_sleep_begin()
 * returns the depth so that net_sleep_end() can restore it. Anything read
 * from the stack before the window must be re-read after it: the point of
 * the window is that other processors change that state while it is open.
 *
 * A caller that also holds the filesystem lock drops that too, and in the
 * other order - this one first, the filesystem's second, because a release
 * unwinds innermost-first and the reacquisition has to run back up the
 * hierarchy.
 */
u32 net_sleep_begin(void);
void net_sleep_end(u32 depth);

#endif /* STRATUM_NET_NETLOCK_H */
