
/*
 * $Id: delay_pools.c,v 1.40 2008/08/02 11:40:15 adrian Exp $
 *
 * DEBUG: section 77    Delay Pools
 * AUTHOR: David Luyer <david@luyer.net>
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */

#include "config.h"

#if DELAY_POOLS
#include "squid.h"

struct _class1DelayPool {
    int class;
    int aggregate;
    uint64_t aggregate_bytes;
};

#define IND_MAP_SZ 256

struct _class2DelayPool {
    int class;
    int aggregate;
    uint64_t aggregate_bytes;
    /* OK: -1 is terminator.  individual[255] is always host 255. */
    /* 255 entries + 1 terminator byte */
    unsigned char individual_map[IND_MAP_SZ];
    unsigned char individual_255_used;
    /* 256 entries */
    int individual[IND_MAP_SZ];
    uint64_t individual_bytes[IND_MAP_SZ];
};

#define NET_MAP_SZ 256
#define C3_IND_SZ (NET_MAP_SZ*IND_MAP_SZ)

struct _class3DelayPool {
    int class;
    int aggregate;
    uint64_t aggregate_bytes;
    /* OK: -1 is terminator.  network[255] is always host 255. */
    /* 255 entries + 1 terminator byte */
    unsigned char network_map[NET_MAP_SZ];
    unsigned char network_255_used;
    /* 256 entries */
    int network[256];
    uint64_t network_bytes[256];
    /* 256 sets of (255 entries + 1 terminator byte) */
    unsigned char individual_map[NET_MAP_SZ][IND_MAP_SZ];
    /* Pack this into one bit per net */
    unsigned char individual_255_used[32];
    /* largest entry = (255<<8)+255 = 65535 */
    int individual[C3_IND_SZ];
    uint64_t individual_bytes[C3_IND_SZ];
};

struct _class6DelayPoolFd {
    int fd;			/* just for sanity checking! */
    int aggregate;
    uint64_t aggregate_bytes;
};
typedef struct _class6DelayPoolFd class6DelayPoolFd;

struct _class6DelayPool {
    int class;
    int aggregate;
    uint64_t aggregate_bytes;
    int fds_size;		/* just for sanity checking */
    class6DelayPoolFd *fds;
};

typedef struct _class1DelayPool class1DelayPool;
typedef struct _class2DelayPool class2DelayPool;
typedef struct _class3DelayPool class3DelayPool;
typedef struct _class6DelayPool class6DelayPool;

union _delayPool {
    class1DelayPool *class1;
    class2DelayPool *class2;
    class3DelayPool *class3;
    class6DelayPool *class6;
};

typedef union _delayPool delayPool;

static delayPool *delay_data = NULL;
static char *delay_no_delay;
/*
 * This array is a hack. Explanation:
 * + the entry is 0 for no pool, or the pool id starting from 1 (rather than 0 like elsehwere)
 * + The entry is cleared on fd close
 * + The entry is -overwritten- if the fd changes pool; its not closed/reopened!
 */
static char *delay_class_6_fds;
static time_t delay_pools_last_update = 0;
static hash_table *delay_id_ptr_hash = NULL;
static long memory_used = 0;

static OBJH delayPoolStats;
static OBJH delayPoolStatsNew;

static unsigned int
delayIdPtrHash(const void *key, unsigned int n)
{
    /* Hashes actual POINTER VALUE.
     * Assumes <= 256 hash buckets & even hash size.
     * Assumes the most variation in pointers to inside
     * medium size objects occurs in the 2nd and 3rd
     * least significant bytes.
     */
    const char *ptr = (char *) &key;
#if SIZEOF_VOID_P == 4
    return (ptr[1] ^ ptr[2]) & (n - 1);
#elif SIZEOF_VOID_P == 8
#if WORDS_BIGENDIAN
    return (ptr[5] ^ ptr[6]) & (n - 1);
#else
    return (ptr[1] ^ ptr[2]) & (n - 1);
#endif
#else
#error What kind of a sick architecture are you on anyway?
#endif
}

static int
delayIdPtrHashCmp(const void *a, const void *b)
{
    /*
     * Compare POINTER VALUE.
     * Note, we can't subtract void pointers, but we don't need
     * to anyway.  All we need is a test for equality.
     */
    return a != b;
}

void
delayPoolsInit(void)
{
    delay_pools_last_update = getCurrentTime();
    delay_no_delay = xcalloc(1, Squid_MaxFD);
    delay_class_6_fds = xcalloc(1, Squid_MaxFD);
    cachemgrRegister("delay", "Delay Pool Levels", delayPoolStats, 0, 1);
    cachemgrRegister("delay2", "Delay Pool Statistics", delayPoolStatsNew, 0, 1);
    eventAdd("delayPoolsUpdate", delayPoolsUpdate, NULL, 1.0, 192);
}

void
delayInitDelayData(unsigned short pools)
{
    if (!pools)
	return;
    delay_data = xcalloc(pools, sizeof(*delay_data));
    memory_used += pools * sizeof(*delay_data);
    delay_id_ptr_hash = hash_create(delayIdPtrHashCmp, 256, delayIdPtrHash);
}

static void
delayIdZero(void *hlink)
{
    hash_link *h = hlink;
    delay_id *id = (delay_id *) h->key;
    *id = 0;
    xfree(h);
    memory_used -= sizeof(*h);
}

void
delayFreeDelayData(unsigned short pools)
{
    if (!delay_id_ptr_hash)
	return;
    /* XXX we should really ensure that the class6 fds array is cleared! */
    safe_free(delay_data);
    memory_used -= pools * sizeof(*delay_data);
    hashFreeItems(delay_id_ptr_hash, delayIdZero);
    hashFreeMemory(delay_id_ptr_hash);
    delay_id_ptr_hash = NULL;
}

void
delayRegisterDelayIdPtr(delay_id * loc)
{
    hash_link *lnk;
    if (!delay_id_ptr_hash)
	return;
    if (*loc == 0)
	return;
    lnk = xmalloc(sizeof(hash_link));
    memory_used += sizeof(hash_link);
    lnk->key = (char *) loc;
    hash_join(delay_id_ptr_hash, lnk);
}

void
delayUnregisterDelayIdPtr(delay_id * loc)
{
    hash_link *lnk;
    if (!delay_id_ptr_hash)
	return;
    /*
     * If we went through a reconfigure, then all the delay_id's
     * got set to zero, and they were removed from our hash
     * table.
     */
    if (*loc == 0)
	return;
    lnk = hash_lookup(delay_id_ptr_hash, loc);
    assert(lnk);
    hash_remove_link(delay_id_ptr_hash, lnk);
    xxfree(lnk);
    memory_used -= sizeof(*lnk);
}

void
delayCreateDelayPool(unsigned short pool, u_char class)
{
    switch (class) {
    case 1:
	delay_data[pool].class1 = xcalloc(1, sizeof(class1DelayPool));
	delay_data[pool].class1->class = 1;
	memory_used += sizeof(class1DelayPool);
	break;
    case 2:
	delay_data[pool].class2 = xcalloc(1, sizeof(class2DelayPool));
	delay_data[pool].class1->class = 2;
	memory_used += sizeof(class2DelayPool);
	break;
    case 3:
	delay_data[pool].class3 = xcalloc(1, sizeof(class3DelayPool));
	delay_data[pool].class1->class = 3;
	memory_used += sizeof(class3DelayPool);
	break;
    case 6:
	delay_data[pool].class6 = xcalloc(1, sizeof(class6DelayPool));
	delay_data[pool].class6->class = 5;
	delay_data[pool].class6->fds = xcalloc(Squid_MaxFD, sizeof(class6DelayPoolFd));
	delay_data[pool].class6->fds_size = Squid_MaxFD;
	memory_used += sizeof(class6DelayPool) + sizeof(class6DelayPoolFd) * Squid_MaxFD;
	break;
    default:
	assert(0);
    }
}

void
delayInitDelayPool(unsigned short pool, u_char class, delaySpecSet * rates)
{
    /* delaySetSpec may be pointer to partial structure so MUST pass by
     * reference.
     */
    switch (class) {
    case 1:
	delay_data[pool].class1->aggregate = (int) (((double) rates->aggregate.max_bytes *
		Config.Delay.initial) / 100);
	break;
    case 2:
	delay_data[pool].class2->aggregate = (int) (((double) rates->aggregate.max_bytes *
		Config.Delay.initial) / 100);
	delay_data[pool].class2->individual_map[0] = 255;
	delay_data[pool].class2->individual_255_used = 0;
	break;
    case 3:
	delay_data[pool].class3->aggregate = (int) (((double) rates->aggregate.max_bytes *
		Config.Delay.initial) / 100);
	delay_data[pool].class3->network_map[0] = 255;
	delay_data[pool].class3->network_255_used = 0;
	memset(&delay_data[pool].class3->individual_255_used, '\0',
	    sizeof(delay_data[pool].class3->individual_255_used));
	break;
    case 6:
	delay_data[pool].class6->aggregate = (((double) rates->aggregate.max_bytes * Config.Delay.initial) / 100);
	memset(delay_data[pool].class6->fds, '\0', delay_data[pool].class6->fds_size * sizeof(class6DelayPoolFd));
	break;
    default:
	assert(0);
    }
}

void
delayFreeDelayPool(unsigned short pool)
{
    /* this is a union - and all free() cares about is the pointer location */
    switch (delay_data[pool].class1->class) {
    case 1:
	memory_used -= sizeof(class1DelayPool);
	break;
    case 2:
	memory_used -= sizeof(class2DelayPool);
	break;
    case 3:
	memory_used -= sizeof(class3DelayPool);
	break;
    case 6:
	memory_used -= sizeof(class6DelayPool) + sizeof(class6DelayPoolFd) * delay_data[pool].class6->fds_size;
	safe_free(delay_data[pool].class6->fds);
    default:
	debug(77, 1) ("delayFreeDelayPool: bad class %d\n",
	    delay_data[pool].class1->class);
    }
    safe_free(delay_data[pool].class1);
}

void
delaySetNoDelay(int fd)
{
    delay_no_delay[fd] = 1;
}

void
delayClearNoDelay(int fd)
{
    delay_no_delay[fd] = 0;
}

int
delayIsNoDelay(int fd)
{
    return delay_no_delay[fd];
}

void
delayCloseFd(int fd)
{
    delay_no_delay[fd] = 0;	/* XXX ? */
    delay_class_6_fds[fd] = 0;
}

static delay_id
delayId(unsigned short pool, unsigned short position)
{
    return (pool << 16) | position;
}

/*
 * select a delay_id for the given clientHttpRequest -reply-.
 *
 * The -pool- part of delay_id is selected by the HttpReply (http->reply) information.
 * The -client- part of delay_id is selected from the client connection
 * source address.
 * The -fd- will always be the client-side connection (so class 5 pools won't work
 *   at all for server-side delay pools. Sorry!)
 */
delay_id
delayClientReply(clientHttpRequest * http, acl_access ** acl)
{
    request_t *r;
    aclCheck_t ch;
    ushort pool;
    assert(http);
    r = http->request;

    memset(&ch, '\0', sizeof(ch));
    ch.conn = http->conn;
    ch.reply = http->reply;
    ch.src_addr = r->client_addr;
    if (r->client_addr.s_addr == INADDR_BROADCAST) {
	debug(77, 2) ("delayClientReply: WARNING: Called with 'allones' address, ignoring\n");
	return delayId(0, 0);
    }
    for (pool = 0; pool < Config.Delay.pools; pool++) {
	//debug(1, 1) ("delayClientReply: checking pool %d list %p\n", pool, acl[pool]);
	if (acl[pool] && aclCheckFast(acl[pool], &ch))
	    break;
    }
    if (pool == Config.Delay.pools)
	return delayId(0, 0);
    return delayPoolClient(pool, http->conn->fd, ch.src_addr.s_addr);
}

/*
 * Select a delay_id for the given clientHttpRequest.
 *
 * The -pool- part of delay_id is selected by the request_t information.
 * The -client- part of delay_id is selected from the client connection
 *   source address.
 * The -fd- will always be the client-side connection (so class 5 pools won't work
 *   at all for server-side delay pools. Sorry!)
 */
delay_id
delayClientRequest(clientHttpRequest * http, acl_access ** acl)
{
    request_t *r;
    aclCheck_t ch;
    ushort pool;
    assert(http);
    r = http->request;

    memset(&ch, '\0', sizeof(ch));
    ch.conn = http->conn;
    ch.request = r;
    if (r->client_addr.s_addr == INADDR_BROADCAST) {
	debug(77, 2) ("delayClient: WARNING: Called with 'allones' address, ignoring\n");
	return delayId(0, 0);
    }
    for (pool = 0; pool < Config.Delay.pools; pool++) {
	if (acl[pool] && aclCheckFast(acl[pool], &ch))
	    break;
    }
    if (pool == Config.Delay.pools)
	return delayId(0, 0);
    return delayPoolClient(pool, http->conn->fd, ch.src_addr.s_addr);
}

delay_id
delayPoolClient(unsigned short pool, int fd, in_addr_t addr)
{
    int i;
    int j;
    unsigned int host;
    unsigned short position;
    unsigned char class, net;
    class = Config.Delay.class[pool];
    debug(77, 2) ("delayPoolClient: pool %u , class %u\n", pool, class);
    if (class == 0)
	return delayId(0, 0);
    if (class == 1)
	return delayId(pool + 1, 0);
    if (class == 2) {
	host = ntohl(addr) & 0xff;
	if (host == 255) {
	    if (!delay_data[pool].class2->individual_255_used) {
		delay_data[pool].class2->individual_255_used = 1;
		delay_data[pool].class2->individual[IND_MAP_SZ - 1] =
		    (int) (((double) Config.Delay.rates[pool]->individual.max_bytes *
			Config.Delay.initial) / 100);
	    }
	    return delayId(pool + 1, 255);
	}
	for (i = 0; i < IND_MAP_SZ; i++) {
	    if (delay_data[pool].class2->individual_map[i] == host)
		break;
	    if (delay_data[pool].class2->individual_map[i] == 255) {
		delay_data[pool].class2->individual_map[i] = host;
		assert(i < (IND_MAP_SZ - 1));
		delay_data[pool].class2->individual_map[i + 1] = 255;
		delay_data[pool].class2->individual[i] =
		    (int) (((double) Config.Delay.rates[pool]->individual.max_bytes *
			Config.Delay.initial) / 100);
		break;
	    }
	}
	return delayId(pool + 1, i);
    }
    if (class == 3) {
	host = ntohl(addr) & 0xffff;
	net = host >> 8;
	host &= 0xff;
	if (net == 255) {
	    i = 255;
	    if (!delay_data[pool].class3->network_255_used) {
		delay_data[pool].class3->network_255_used = 1;
		delay_data[pool].class3->network[255] =
		    (int) (((double) Config.Delay.rates[pool]->network.max_bytes *
			Config.Delay.initial) / 100);
		delay_data[pool].class3->individual_map[i][0] = 255;
	    }
	} else {
	    for (i = 0; i < NET_MAP_SZ; i++) {
		if (delay_data[pool].class3->network_map[i] == net)
		    break;
		if (delay_data[pool].class3->network_map[i] == 255) {
		    delay_data[pool].class3->network_map[i] = net;
		    delay_data[pool].class3->individual_map[i][0] = 255;
		    assert(i < (NET_MAP_SZ - 1));
		    delay_data[pool].class3->network_map[i + 1] = 255;
		    delay_data[pool].class3->network[i] =
			(int) (((double) Config.Delay.rates[pool]->network.max_bytes *
			    Config.Delay.initial) / 100);
		    break;
		}
	    }
	}
	position = i << 8;
	if (host == 255) {
	    position |= 255;
	    if (!(delay_data[pool].class3->individual_255_used[i / 8] & (1 << (i % 8)))) {
		delay_data[pool].class3->individual_255_used[i / 8] |= (1 << (i % 8));
		delay_data[pool].class3->individual[position] =
		    (int) (((double) Config.Delay.rates[pool]->individual.max_bytes *
			Config.Delay.initial) / 100);
	    }
	    return delayId(pool + 1, position);
	}
	assert(i < NET_MAP_SZ);
	for (j = 0; j < IND_MAP_SZ; j++) {
	    if (delay_data[pool].class3->individual_map[i][j] == host) {
		position |= j;
		break;
	    }
	    if (delay_data[pool].class3->individual_map[i][j] == 255) {
		delay_data[pool].class3->individual_map[i][j] = host;
		assert(j < (IND_MAP_SZ - 1));
		delay_data[pool].class3->individual_map[i][j + 1] = 255;
		position |= j;
		delay_data[pool].class3->individual[position] =
		    (int) (((double) Config.Delay.rates[pool]->individual.max_bytes *
			Config.Delay.initial) / 100);
		break;
	    }
	}
	return delayId(pool + 1, position);
    }
    /* class 6 delay pool - position is the fd number which we may not yet have! */
    if (class == 6) {
	if (fd < 0) {
	    debug(1, 1) ("delayClientClient: class 5 delay pool doesn't yet have access to the FD?!\n");
	    return delayId(0, 0);
	}
	assert(fd < delay_data[pool].class6->fds_size);
	position = fd;
	delay_data[pool].class6->fds[position].aggregate =
	    (int) (((double) Config.Delay.rates[pool]->individual.max_bytes * Config.Delay.initial) / 100);
	delay_class_6_fds[fd] = pool + 1;

	return delayId(pool + 1, position);
    }
    assert(0);
    return -1;
}

static void
delayUpdateClass1(class1DelayPool * class1, char pool, delaySpecSet * rates, int incr)
{
    /* delaySetSpec may be pointer to partial structure so MUST pass by
     * reference.
     */
    if (rates->aggregate.restore_bps != -1 &&
	(class1->aggregate += rates->aggregate.restore_bps * incr) >
	rates->aggregate.max_bytes)
	class1->aggregate = rates->aggregate.max_bytes;
}

static void
delayUpdateClass6(class6DelayPool * class6, char pool, delaySpecSet * rates, int incr)
{
    int restore_bytes;
    int fd;

    if (rates->aggregate.restore_bps != -1 &&
	(class6->aggregate += rates->aggregate.restore_bps * incr) >
	rates->aggregate.max_bytes)
	class6->aggregate = rates->aggregate.max_bytes;
    if ((restore_bytes = rates->individual.restore_bps) == -1)
	return;
    restore_bytes *= incr;

    /* Update per-FD allowances */
    assert(Biggest_FD < class6->fds_size);
    for (fd = 0; fd < Biggest_FD; fd++) {
	if (delay_class_6_fds[fd] != (pool + 1))
	    continue;
	class6->fds[fd].aggregate += restore_bytes;
	if (class6->fds[fd].aggregate > rates->individual.max_bytes)
	    class6->fds[fd].aggregate = rates->individual.max_bytes;
    }
}

static void
delayUpdateClass2(class2DelayPool * class2, char pool, delaySpecSet * rates, int incr)
{
    int restore_bytes;
    unsigned char i;		/* depends on 255 + 1 = 0 */
    /* delaySetSpec may be pointer to partial structure so MUST pass by
     * reference.
     */
    if (rates->aggregate.restore_bps != -1 &&
	(class2->aggregate += rates->aggregate.restore_bps * incr) >
	rates->aggregate.max_bytes)
	class2->aggregate = rates->aggregate.max_bytes;
    if ((restore_bytes = rates->individual.restore_bps) == -1)
	return;
    restore_bytes *= incr;
    /* i < IND_MAP_SZ is enforced by data type (unsigned chars are all < 256).
     * this loop starts at 0 or 255 and ends at 254 unless terminated earlier
     * by finding the end of the map.  note as above that 255 + 1 = 0.
     */
    for (i = (class2->individual_255_used ? 255 : 0);; i++) {
	if (i != 255 && class2->individual_map[i] == 255)
	    return;
	if (class2->individual[i] != rates->individual.max_bytes &&
	    (class2->individual[i] += restore_bytes) > rates->individual.max_bytes)
	    class2->individual[i] = rates->individual.max_bytes;
	if (i == 254)
	    return;
    }
}

static void
delayUpdateClass3(class3DelayPool * class3, char pool, delaySpecSet * rates, int incr)
{
    int individual_restore_bytes, network_restore_bytes;
    int mpos;
    unsigned char i, j;		/* depends on 255 + 1 = 0 */
    /* delaySetSpec may be pointer to partial structure so MUST pass by
     * reference.
     */
    if (rates->aggregate.restore_bps != -1 &&
	(class3->aggregate += rates->aggregate.restore_bps * incr) >
	rates->aggregate.max_bytes)
	class3->aggregate = rates->aggregate.max_bytes;
    /* the following line deliberately uses &, not &&, in an if statement
     * to avoid conditional execution
     */
    if (((network_restore_bytes = rates->network.restore_bps) == -1) &
	((individual_restore_bytes = rates->individual.restore_bps) == -1))
	return;
    individual_restore_bytes *= incr;
    network_restore_bytes *= incr;
    /* i < NET_MAP_SZ is enforced by data type (unsigned chars are all < 256).
     * this loop starts at 0 or 255 and ends at 254 unless terminated earlier
     * by finding the end of the map.  note as above that 255 + 1 = 0.
     */
    for (i = (class3->network_255_used ? 255 : 0);; ++i) {
	if (i != 255 && class3->network_map[i] == 255)
	    return;
	if (individual_restore_bytes != -incr) {
	    mpos = i << 8;
	    /* this is not as simple as the outer loop as mpos doesn't wrap like
	     * i and j do.  so the net 255 increment is done as a separate special
	     * case.  the alternative would be overlapping a union of two chars on
	     * top of a 16-bit unsigned int, but that wouldn't really be worth the
	     * effort.
	     */
	    for (j = 0;; ++j, ++mpos) {
		if (class3->individual_map[i][j] == 255)
		    break;
		assert(mpos < C3_IND_SZ);
		if (class3->individual[mpos] != rates->individual.max_bytes &&
		    (class3->individual[mpos] += individual_restore_bytes) >
		    rates->individual.max_bytes)
		    class3->individual[mpos] = rates->individual.max_bytes;
		if (j == 254)
		    break;
	    }
	    if (class3->individual_255_used[i / 8] & (1 << (i % 8))) {
		mpos |= 255;	/* this will set mpos to network 255 */
		assert(mpos < C3_IND_SZ);
		if (class3->individual[mpos] != rates->individual.max_bytes &&
		    (class3->individual[mpos] += individual_restore_bytes) >
		    rates->individual.max_bytes)
		    class3->individual[mpos] = rates->individual.max_bytes;
	    }
	}
	if (network_restore_bytes != -incr &&
	    class3->network[i] != rates->network.max_bytes &&
	    (class3->network[i] += network_restore_bytes) >
	    rates->network.max_bytes)
	    class3->network[i] = rates->network.max_bytes;
	if (i == 254)
	    return;
    }
}

void
delayPoolsUpdate(void *unused)
{
    int incr = squid_curtime - delay_pools_last_update;
    unsigned short i;
    unsigned char class;
    if (!Config.Delay.pools)
	return;
    eventAdd("delayPoolsUpdate", delayPoolsUpdate, NULL, 1.0, 192);
    delay_pools_last_update = squid_curtime;
    for (i = 0; i < Config.Delay.pools; i++) {
	class = Config.Delay.class[i];
	if (!class)
	    continue;
	switch (class) {
	case 1:
	    delayUpdateClass1(delay_data[i].class1, i, Config.Delay.rates[i], incr);
	    break;
	case 2:
	    delayUpdateClass2(delay_data[i].class2, i, Config.Delay.rates[i], incr);
	    break;
	case 3:
	    delayUpdateClass3(delay_data[i].class3, i, Config.Delay.rates[i], incr);
	    break;
	case 6:
	    delayUpdateClass6(delay_data[i].class6, i, Config.Delay.rates[i], incr);
	    break;
	default:
	    assert(0);
	}
    }
}

/*
 * this returns the number of bytes the client is permitted. it does not take
 * into account bytes already buffered - that is up to the caller.
 */
int
delayBytesWanted(delay_id d, int min, int max)
{
    unsigned short position = d & 0xFFFF;
    unsigned short pool = (d >> 16) - 1;
    unsigned char class = (pool == 0xFFFF) ? 0 : Config.Delay.class[pool];
    int nbytes = max;

    switch (class) {
    case 0:
	break;

    case 1:
	if (Config.Delay.rates[pool]->aggregate.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class1->aggregate);
	break;

    case 2:
	if (Config.Delay.rates[pool]->aggregate.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class2->aggregate);
	if (Config.Delay.rates[pool]->individual.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class2->individual[position]);
	break;

    case 3:
	if (Config.Delay.rates[pool]->aggregate.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class3->aggregate);
	if (Config.Delay.rates[pool]->individual.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class3->individual[position]);
	if (Config.Delay.rates[pool]->network.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class3->network[position >> 8]);
	break;

    case 6:
	if (Config.Delay.rates[pool]->aggregate.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class3->aggregate);
	assert(position < delay_data[pool].class6->fds_size);
	if (Config.Delay.rates[pool]->individual.restore_bps != -1)
	    nbytes = XMIN(nbytes, delay_data[pool].class6->fds[position].aggregate);
	break;
    default:
	fatalf("delayBytesWanted: Invalid class %d\n", class);
	break;
    }
    nbytes = XMAX(min, nbytes);
    return nbytes;
}

/*
 * this records actual bytes received.  always recorded, even if the
 * class is disabled - it's more efficient to just do it than to do all
 * the checks.
 */
void
delayBytesIn(delay_id d, int qty)
{
    unsigned short position = d & 0xFFFF;
    unsigned short pool = (d >> 16) - 1;
    unsigned char class;

    if (pool == 0xFFFF)
	return;
    class = Config.Delay.class[pool];
    switch (class) {
    case 1:
	delay_data[pool].class1->aggregate -= qty;

	delay_data[pool].class1->aggregate_bytes += qty;
	return;
    case 2:
	delay_data[pool].class2->aggregate -= qty;
	delay_data[pool].class2->individual[position] -= qty;

	delay_data[pool].class2->aggregate_bytes += qty;
	delay_data[pool].class2->individual_bytes[position] += qty;
	return;
    case 3:
	delay_data[pool].class3->aggregate -= qty;
	delay_data[pool].class3->network[position >> 8] -= qty;
	delay_data[pool].class3->individual[position] -= qty;

	delay_data[pool].class3->aggregate_bytes += qty;
	delay_data[pool].class3->network_bytes[position >> 8] += qty;
	delay_data[pool].class3->individual_bytes[position] += qty;
	return;

    case 6:
	delay_data[pool].class6->aggregate -= qty;
	assert(position < delay_data[pool].class6->fds_size);
	delay_data[pool].class6->fds[position].aggregate -= qty;
	return;
    }
    fatalf("delayBytesWanted: Invalid class %d\n", class);
    assert(0);
}

int
delayMostBytesWanted(const MemObject * mem, int max)
{
    int i = 0;
    int found = 0;
    store_client *sc;
    dlink_node *node;
    for (node = mem->clients.head; node; node = node->next) {
	sc = (store_client *) node->data;
	i = delayBytesWanted(sc->delay_id, i, max);
	found = 1;
    }
    return found ? i : max;
}

delay_id
delayMostBytesAllowed(const MemObject * mem, size_t * read_sz)
{
    int j;
    int jmax = -1;
    store_client *sc;
    dlink_node *node;
    delay_id d = 0;
    for (node = mem->clients.head; node; node = node->next) {
	sc = (store_client *) node->data;
	j = delayBytesWanted(sc->delay_id, 0, INT_MAX);
	if (j > jmax) {
	    jmax = j;
	    d = sc->delay_id;
	}
    }
    if (jmax >= 0 && jmax < (int) *read_sz) {
	if (jmax == 0)
	    jmax = 1;
	if (jmax > 1460)
	    jmax = 1460;
	*read_sz = (size_t) jmax;
    }
    return d;
}

void
delaySetStoreClient(store_client * sc, delay_id delay_id)
{
    assert(sc != NULL);
    sc->delay_id = delay_id;
    delayRegisterDelayIdPtr(&sc->delay_id);
}

static void
delayPoolStatsAg(StoreEntry * sentry, int pool, int type, delaySpecSet * rate, int ag, uint64_t bytes)
{
    /* note - always pass delaySpecSet's by reference as may be incomplete */
    if (rate->aggregate.restore_bps == -1) {
	if (type == 1)
	    storeAppendPrintf(sentry, "\tAggregate:\n\t\tDisabled.\n\n");
	return;
    }
    if (type == 1) {
	storeAppendPrintf(sentry, "\tAggregate:\n");
	storeAppendPrintf(sentry, "\t\tMax: %d\n", rate->aggregate.max_bytes);
	storeAppendPrintf(sentry, "\t\tRestore: %d\n", rate->aggregate.restore_bps);
	storeAppendPrintf(sentry, "\t\tCurrent: %d\n", ag);
    } else {
	storeAppendPrintf(sentry, "pools.pool.%d.max=%d\n", pool + 1, rate->aggregate.max_bytes);
	storeAppendPrintf(sentry, "pools.pool.%d.restore=%d\n", pool + 1, rate->aggregate.restore_bps);
	storeAppendPrintf(sentry, "pools.pool.%d.current=%d\n", pool + 1, ag);
	storeAppendPrintf(sentry, "pools.pool.%d.bytes=%" PRIu64 "\n", pool + 1, bytes);
    }
}

static void
delayPoolStats1(StoreEntry * sentry, int type, unsigned short pool)
{
    /* must be a reference only - partially malloc()d struct */
    delaySpecSet *rate = Config.Delay.rates[pool];

    if (type == 1) {
	storeAppendPrintf(sentry, "Pool: %d\n\tClass: 1\n\n", pool + 1);
    } else {
	storeAppendPrintf(sentry, "pools.pool.%d.class=1\n", pool + 1);
    }
    delayPoolStatsAg(sentry, pool, type, rate, delay_data[pool].class1->aggregate, delay_data[pool].class1->aggregate_bytes);
    storeAppendPrintf(sentry, "\n");
}

static void
delayPoolStats2Individual(StoreEntry * sentry, int type, unsigned short pool, int i)
{
    class2DelayPool *class2 = delay_data[pool].class2;

    if (type == 1) {
	storeAppendPrintf(sentry, "%d:%d ", class2->individual_map[i],
	    class2->individual[i]);
    } else {
	storeAppendPrintf(sentry, "pools.pool.%d.individuals.%d.rate=%d\n", pool + 1, class2->individual_map[i], class2->individual[i]);
	storeAppendPrintf(sentry, "pools.pool.%d.individuals.%d.bytes=%" PRIu64 "\n", pool + 1, class2->individual_map[i], class2->individual_bytes[i]);
    }
}

static void
delayPoolStats2(StoreEntry * sentry, int type, unsigned short pool)
{
    /* must be a reference only - partially malloc()d struct */
    delaySpecSet *rate = Config.Delay.rates[pool];
    class2DelayPool *class2 = delay_data[pool].class2;
    unsigned char shown = 0;
    unsigned int i;

    if (type == 1)
	storeAppendPrintf(sentry, "Pool: %d\n\tClass: 2\n\n", pool + 1);
    else
	storeAppendPrintf(sentry, "pools.pool.%d.class=2\n", pool + 1);

    delayPoolStatsAg(sentry, pool, type, rate, class2->aggregate, class2->aggregate_bytes);
    if (rate->individual.restore_bps == -1) {
	if (type == 1)
	    storeAppendPrintf(sentry, "\tIndividual:\n\t\tDisabled.\n\n");
	return;
    }
    if (type == 1) {
	storeAppendPrintf(sentry, "\tIndividual:\n");
	storeAppendPrintf(sentry, "\t\tMax: %d\n", rate->individual.max_bytes);
	storeAppendPrintf(sentry, "\t\tRate: %d\n", rate->individual.restore_bps);
	storeAppendPrintf(sentry, "\t\tCurrent: ");
    } else {
	storeAppendPrintf(sentry, "pools.pool.%d.individual.max=%d\n", pool + 1, rate->individual.max_bytes);
	storeAppendPrintf(sentry, "pools.pool.%d.individual.rate=%d\n", pool + 1, rate->individual.restore_bps);
    }
    for (i = 0; i < IND_MAP_SZ; i++) {
	if (class2->individual_map[i] == 255)
	    break;
	delayPoolStats2Individual(sentry, type, pool, i);
	shown = 1;
    }
    if (class2->individual_255_used) {
	delayPoolStats2Individual(sentry, type, pool, 255);
	shown = 1;
    }
    if (type == 1 && !shown)
	storeAppendPrintf(sentry, "Not used yet.");
    storeAppendPrintf(sentry, "\n\n");
}


static void
delayPoolStats3Network(StoreEntry * e, int type, unsigned short pool, int i)
{
    class3DelayPool *class3 = delay_data[pool].class3;

    if (type == 1) {
	storeAppendPrintf(e, "%d:%d ", class3->network_map[i],
	    class3->network[i]);
    } else {
	storeAppendPrintf(e, "pools.pool.%d.networks.%d.rate=%d\n", pool + 1, class3->network_map[i], class3->network[i]);;
	storeAppendPrintf(e, "pools.pool.%d.networks.%d.bytes=%" PRIu64 "\n", pool + 1, class3->network_map[i], class3->network_bytes[i]);
    }
}

static void
delayPoolStats3Individual(StoreEntry * e, int type, unsigned short pool, int i, int j)
{
    class3DelayPool *class3 = delay_data[pool].class3;

    if (type == 1) {
	storeAppendPrintf(e, "%d:%d ", class3->individual_map[i][j],
	    class3->individual[(i << 8) | j]);
    } else {
	storeAppendPrintf(e, "pools.pool.%d.networks.%d.individuals.%d.rate=%d\n", pool + 1, class3->network_map[i], class3->individual_map[i][j], class3->individual[(i << 8) | j]);
	storeAppendPrintf(e, "pools.pool.%d.networks.%d.individuals.%d.bytes=%" PRIu64 "\n", pool + 1, class3->network_map[i], class3->individual_map[i][j], class3->individual_bytes[(i << 8) | j]);
    }
}

static int
delayPoolStats3IndNetwork(StoreEntry * sentry, int type, unsigned short pool, int i)
{
    int j;
    int shown = 0;
    class3DelayPool *class3 = delay_data[pool].class3;

    if (type == 1)
	storeAppendPrintf(sentry, "\t\tCurrent [Network %d]: ", class3->network_map[i]);
    shown = 1;

    for (j = 0; j < IND_MAP_SZ; j++) {
	if (class3->individual_map[i][j] == 255)
	    break;
	delayPoolStats3Individual(sentry, type, pool, i, j);
	shown = 1;
    }
    if (class3->individual_255_used[i / 8] & (1 << (i % 8))) {
	delayPoolStats3Individual(sentry, type, pool, i, 255);
	shown = 1;
    }
    if (type == 1)
	storeAppendPrintf(sentry, "\n");
    return shown;
}

static void
delayPoolStats3(StoreEntry * sentry, int type, unsigned short pool)
{
    /* fully malloc()d struct in this case only */
    delaySpecSet *rate = Config.Delay.rates[pool];
    class3DelayPool *class3 = delay_data[pool].class3;
    unsigned char shown = 0;
    unsigned int i;

    if (type == 1)
	storeAppendPrintf(sentry, "Pool: %d\n\tClass: 3\n\n", pool + 1);
    else
	storeAppendPrintf(sentry, "pools.pool.%d.class=3\n", pool + 1);
    delayPoolStatsAg(sentry, pool, type, rate, class3->aggregate, class3->aggregate_bytes);
    if ((type == 1) && rate->network.restore_bps == -1) {
	storeAppendPrintf(sentry, "\tNetwork:\n\t\tDisabled.");
    } else {
	if (type == 1) {
	    storeAppendPrintf(sentry, "\tNetwork:\n");
	    storeAppendPrintf(sentry, "\t\tMax: %d\n", rate->network.max_bytes);
	    storeAppendPrintf(sentry, "\t\tRate: %d\n", rate->network.restore_bps);
	    storeAppendPrintf(sentry, "\t\tCurrent: ");
	} else {
	    storeAppendPrintf(sentry, "pools.pool.%d.network.max=%d\n", pool + 1, rate->network.max_bytes);
	    storeAppendPrintf(sentry, "pools.pool.%d.network.rate=%d\n", pool + 1, rate->network.restore_bps);
	}
	for (i = 0; i < NET_MAP_SZ; i++) {
	    if (class3->network_map[i] == 255)
		break;
	    delayPoolStats3Network(sentry, type, pool, i);
	    shown = 1;
	}
	if (class3->network_255_used) {
	    delayPoolStats3Network(sentry, type, pool, 255);
	    shown = 1;
	}
	if ((type == 1) && !shown)
	    storeAppendPrintf(sentry, "Not used yet.");
    }
    if (type == 1)
	storeAppendPrintf(sentry, "\n\n");
    shown = 0;
    if (rate->individual.restore_bps == -1) {
	if (type == 1)
	    storeAppendPrintf(sentry, "\tIndividual:\n\t\tDisabled.\n\n");
	return;
    }
    if (type == 1) {
	storeAppendPrintf(sentry, "\tIndividual:\n");
	storeAppendPrintf(sentry, "\t\tMax: %d\n", rate->individual.max_bytes);
	storeAppendPrintf(sentry, "\t\tRate: %d\n", rate->individual.restore_bps);
    } else {
	storeAppendPrintf(sentry, "pools.pool.%d.individual.max=%d\n", pool + 1, rate->individual.max_bytes);
	storeAppendPrintf(sentry, "pools.pool.%d.individual.rate=%d\n", pool + 1, rate->individual.restore_bps);
    }
    for (i = 0; i < NET_MAP_SZ; i++) {
	if (class3->network_map[i] == 255)
	    break;
	shown |= delayPoolStats3IndNetwork(sentry, type, pool, i);
    }
    if (class3->network_255_used)
	shown |= delayPoolStats3IndNetwork(sentry, type, pool, 255);

    if ((type == 1) && !shown)
	storeAppendPrintf(sentry, "\t\tCurrent [All networks]: Not used yet.\n");
    storeAppendPrintf(sentry, "\n");
}

static void
delayPoolStats5(StoreEntry * sentry, int type, unsigned short pool)
{
    delaySpecSet *rate = Config.Delay.rates[pool];
    class6DelayPool *class6 = delay_data[pool].class6;
    int i;

    if (type == 1)
	storeAppendPrintf(sentry, "Pool: %d\n\tClass: 5\n\n", pool + 1);
    else
	storeAppendPrintf(sentry, "pools.pool.%d.class=5\n", pool + 1);
    delayPoolStatsAg(sentry, pool, type, rate, class6->aggregate, class6->aggregate_bytes);

    /* Per-FD stats! */
    /* delay_class_6_fds[] contains the pool id starting from 1, not from 0 */
    for (i = 0; i < Biggest_FD; i++) {
	if (delay_class_6_fds[i] == (pool + 1)) {
	    if (type == 1) {
		storeAppendPrintf(sentry, "    FD: %d, aggregate %d\n", i, class6->fds[i].aggregate);
	    } else {
		storeAppendPrintf(sentry, "pools.pool.%d.fd.%d.aggregate=%d\n", pool + 1, i, class6->fds[i].aggregate);
	    }
	}
    }
}

static void
delayPoolStats(StoreEntry * sentry)
{
    unsigned short i;

    storeAppendPrintf(sentry, "Delay pools configured: %d\n\n", Config.Delay.pools);
    for (i = 0; i < Config.Delay.pools; i++) {
	switch (Config.Delay.class[i]) {
	case 0:
	    storeAppendPrintf(sentry, "Pool: %d\n\tClass: 0\n\n", i + 1);
	    storeAppendPrintf(sentry, "\tMisconfigured pool.\n\n");
	    break;
	case 1:
	    delayPoolStats1(sentry, 1, i);
	    break;
	case 2:
	    delayPoolStats2(sentry, 1, i);
	    break;
	case 3:
	    delayPoolStats3(sentry, 1, i);
	    break;
	case 6:
	    delayPoolStats5(sentry, 1, i);
	    break;
	default:
	    assert(0);
	}
    }
    storeAppendPrintf(sentry, "Memory Used: %d bytes\n", (int) memory_used);
}

static void
delayPoolStatsNew(StoreEntry * e)
{
    unsigned short i;

    storeAppendPrintf(e, "pools.npools=%d\n\n", Config.Delay.pools);
    for (i = 0; i < Config.Delay.pools; i++) {
	switch (Config.Delay.class[i]) {
	case 0:
	    storeAppendPrintf(e, "pools.pool.%d.class=0\n\n", i + 1);
	    break;
	case 1:
	    delayPoolStats1(e, 2, i);
	    break;
	case 2:
	    delayPoolStats2(e, 2, i);
	    break;
	case 3:
	    delayPoolStats3(e, 2, i);
	    break;
	case 6:
	    delayPoolStats5(e, 2, i);
	    break;
	default:
	    assert(0);
	}
    }
}

#endif
