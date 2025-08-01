/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2001 Niels Provos <provos@citi.umich.edu>
 * Copyright 2011-2018 Alexander Bluhm <bluhm@openbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *	$OpenBSD: pf_norm.c,v 1.114 2009/01/29 14:11:45 henning Exp $
 */

#include <sys/cdefs.h>
#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_pf.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/refcount.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/vnet.h>
#include <net/pfvar.h>
#include <net/if_pflog.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/scope6_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_seq.h>
#include <netinet/sctp_constants.h>
#include <netinet/sctp_header.h>

#ifdef INET6
#include <netinet/ip6.h>
#endif /* INET6 */

struct pf_frent {
	TAILQ_ENTRY(pf_frent)	fr_next;
	struct mbuf	*fe_m;
	uint16_t	fe_hdrlen;	/* ipv4 header length with ip options
					   ipv6, extension, fragment header */
	uint16_t	fe_extoff;	/* last extension header offset or 0 */
	uint16_t	fe_len;		/* fragment length */
	uint16_t	fe_off;		/* fragment offset */
	uint16_t	fe_mff;		/* more fragment flag */
};

RB_HEAD(pf_frag_tree, pf_fragment);
struct pf_frnode {
	struct pf_addr		fn_src;		/* ip source address */
	struct pf_addr		fn_dst;		/* ip destination address */
	sa_family_t		fn_af;		/* address family */
	u_int8_t		fn_proto;	/* protocol for fragments in fn_tree */
	u_int32_t		fn_fragments;	/* number of entries in fn_tree */

	RB_ENTRY(pf_frnode)	fn_entry;
	struct pf_frag_tree	fn_tree;	/* matching fragments, lookup by id */
};

struct pf_fragment {
	uint32_t	fr_id;	/* fragment id for reassemble */

	/* pointers to queue element */
	struct pf_frent	*fr_firstoff[PF_FRAG_ENTRY_POINTS];
	/* count entries between pointers */
	uint8_t	fr_entries[PF_FRAG_ENTRY_POINTS];
	RB_ENTRY(pf_fragment) fr_entry;
	TAILQ_ENTRY(pf_fragment) frag_next;
	uint32_t	fr_timeout;
	TAILQ_HEAD(pf_fragq, pf_frent) fr_queue;
	uint16_t	fr_maxlen;	/* maximum length of single fragment */
	u_int16_t	fr_holes;	/* number of holes in the queue */
	struct pf_frnode *fr_node;	/* ip src/dst/proto/af for fragments */
};

VNET_DEFINE_STATIC(struct mtx, pf_frag_mtx);
#define V_pf_frag_mtx		VNET(pf_frag_mtx)
#define PF_FRAG_LOCK()		mtx_lock(&V_pf_frag_mtx)
#define PF_FRAG_UNLOCK()	mtx_unlock(&V_pf_frag_mtx)
#define PF_FRAG_ASSERT()	mtx_assert(&V_pf_frag_mtx, MA_OWNED)

VNET_DEFINE(uma_zone_t, pf_state_scrub_z);	/* XXX: shared with pfsync */

VNET_DEFINE_STATIC(uma_zone_t, pf_frent_z);
#define	V_pf_frent_z	VNET(pf_frent_z)
VNET_DEFINE_STATIC(uma_zone_t, pf_frnode_z);
#define	V_pf_frnode_z	VNET(pf_frnode_z)
VNET_DEFINE_STATIC(uma_zone_t, pf_frag_z);
#define	V_pf_frag_z	VNET(pf_frag_z)

TAILQ_HEAD(pf_fragqueue, pf_fragment);
TAILQ_HEAD(pf_cachequeue, pf_fragment);
RB_HEAD(pf_frnode_tree, pf_frnode);
VNET_DEFINE_STATIC(struct pf_fragqueue,	pf_fragqueue);
#define	V_pf_fragqueue			VNET(pf_fragqueue)
static __inline int	pf_frnode_compare(struct pf_frnode *,
			    struct pf_frnode *);
VNET_DEFINE_STATIC(struct pf_frnode_tree, pf_frnode_tree);
#define	V_pf_frnode_tree		VNET(pf_frnode_tree)
RB_PROTOTYPE(pf_frnode_tree, pf_frnode, fn_entry, pf_frnode_compare);
RB_GENERATE(pf_frnode_tree, pf_frnode, fn_entry, pf_frnode_compare);

static int		 pf_frag_compare(struct pf_fragment *,
			    struct pf_fragment *);
static RB_PROTOTYPE(pf_frag_tree, pf_fragment, fr_entry, pf_frag_compare);
static RB_GENERATE(pf_frag_tree, pf_fragment, fr_entry, pf_frag_compare);

static void	pf_flush_fragments(void);
static void	pf_free_fragment(struct pf_fragment *);

static struct pf_frent *pf_create_fragment(u_short *);
static int	pf_frent_holes(struct pf_frent *frent);
static struct pf_fragment	*pf_find_fragment(struct pf_frnode *, u_int32_t);
static inline int	pf_frent_index(struct pf_frent *);
static int	pf_frent_insert(struct pf_fragment *,
			    struct pf_frent *, struct pf_frent *);
void			pf_frent_remove(struct pf_fragment *,
			    struct pf_frent *);
struct pf_frent		*pf_frent_previous(struct pf_fragment *,
			    struct pf_frent *);
static struct pf_fragment *pf_fillup_fragment(struct pf_frnode *, u_int32_t,
		    struct pf_frent *, u_short *);
static struct mbuf *pf_join_fragment(struct pf_fragment *);
#ifdef INET
static int	pf_reassemble(struct mbuf **, u_short *);
#endif	/* INET */
#ifdef INET6
static int	pf_reassemble6(struct mbuf **,
		    struct ip6_frag *, uint16_t, uint16_t, u_short *);
#endif	/* INET6 */

#ifdef INET
static void
pf_ip2key(struct ip *ip, struct pf_frnode *key)
{

	key->fn_src.v4 = ip->ip_src;
	key->fn_dst.v4 = ip->ip_dst;
	key->fn_af = AF_INET;
	key->fn_proto = ip->ip_p;
}
#endif	/* INET */

void
pf_normalize_init(void)
{

	V_pf_frag_z = uma_zcreate("pf frags", sizeof(struct pf_fragment),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pf_frnode_z = uma_zcreate("pf fragment node",
	    sizeof(struct pf_frnode), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);
	V_pf_frent_z = uma_zcreate("pf frag entries", sizeof(struct pf_frent),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pf_state_scrub_z = uma_zcreate("pf state scrubs",
	    sizeof(struct pf_state_scrub),  NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);

	mtx_init(&V_pf_frag_mtx, "pf fragments", NULL, MTX_DEF);

	V_pf_limits[PF_LIMIT_FRAGS].zone = V_pf_frent_z;
	V_pf_limits[PF_LIMIT_FRAGS].limit = PFFRAG_FRENT_HIWAT;
	uma_zone_set_max(V_pf_frent_z, PFFRAG_FRENT_HIWAT);
	uma_zone_set_warning(V_pf_frent_z, "PF frag entries limit reached");

	TAILQ_INIT(&V_pf_fragqueue);
}

void
pf_normalize_cleanup(void)
{

	uma_zdestroy(V_pf_state_scrub_z);
	uma_zdestroy(V_pf_frent_z);
	uma_zdestroy(V_pf_frnode_z);
	uma_zdestroy(V_pf_frag_z);

	mtx_destroy(&V_pf_frag_mtx);
}

static int
pf_frnode_compare(struct pf_frnode *a, struct pf_frnode *b)
{
	int	diff;

	if ((diff = a->fn_proto - b->fn_proto) != 0)
		return (diff);
	if ((diff = a->fn_af - b->fn_af) != 0)
		return (diff);
	if ((diff = pf_addr_cmp(&a->fn_src, &b->fn_src, a->fn_af)) != 0)
		return (diff);
	if ((diff = pf_addr_cmp(&a->fn_dst, &b->fn_dst, a->fn_af)) != 0)
		return (diff);
	return (0);
}

static __inline int
pf_frag_compare(struct pf_fragment *a, struct pf_fragment *b)
{
	int	diff;

	if ((diff = a->fr_id - b->fr_id) != 0)
		return (diff);

	return (0);
}

void
pf_purge_expired_fragments(void)
{
	u_int32_t	expire = time_uptime -
			    V_pf_default_rule.timeout[PFTM_FRAG];

	pf_purge_fragments(expire);
}

void
pf_purge_fragments(uint32_t expire)
{
	struct pf_fragment	*frag;

	PF_FRAG_LOCK();
	while ((frag = TAILQ_LAST(&V_pf_fragqueue, pf_fragqueue)) != NULL) {
		if (frag->fr_timeout > expire)
			break;

		DPFPRINTF(PF_DEBUG_MISC, "expiring %d(%p)",
		    frag->fr_id, frag);
		pf_free_fragment(frag);
	}

	PF_FRAG_UNLOCK();
}

/*
 * Try to flush old fragments to make space for new ones
 */
static void
pf_flush_fragments(void)
{
	struct pf_fragment	*frag;
	int			 goal;

	PF_FRAG_ASSERT();

	goal = uma_zone_get_cur(V_pf_frent_z) * 9 / 10;
	DPFPRINTF(PF_DEBUG_MISC, "trying to free %d frag entriess", goal);
	while (goal < uma_zone_get_cur(V_pf_frent_z)) {
		frag = TAILQ_LAST(&V_pf_fragqueue, pf_fragqueue);
		if (frag)
			pf_free_fragment(frag);
		else
			break;
	}
}

/*
 * Remove a fragment from the fragment queue, free its fragment entries,
 * and free the fragment itself.
 */
static void
pf_free_fragment(struct pf_fragment *frag)
{
	struct pf_frent		*frent;
	struct pf_frnode	*frnode;

	PF_FRAG_ASSERT();

	frnode = frag->fr_node;
	RB_REMOVE(pf_frag_tree, &frnode->fn_tree, frag);
	MPASS(frnode->fn_fragments >= 1);
	frnode->fn_fragments--;
	if (frnode->fn_fragments == 0) {
		MPASS(RB_EMPTY(&frnode->fn_tree));
		RB_REMOVE(pf_frnode_tree, &V_pf_frnode_tree, frnode);
		uma_zfree(V_pf_frnode_z, frnode);
	}

	TAILQ_REMOVE(&V_pf_fragqueue, frag, frag_next);

	/* Free all fragment entries */
	while ((frent = TAILQ_FIRST(&frag->fr_queue)) != NULL) {
		TAILQ_REMOVE(&frag->fr_queue, frent, fr_next);

		m_freem(frent->fe_m);
		uma_zfree(V_pf_frent_z, frent);
	}

	uma_zfree(V_pf_frag_z, frag);
}

static struct pf_fragment *
pf_find_fragment(struct pf_frnode *key, uint32_t id)
{
	struct pf_fragment	*frag, idkey;
	struct pf_frnode	*frnode;

	PF_FRAG_ASSERT();

	frnode = RB_FIND(pf_frnode_tree, &V_pf_frnode_tree, key);
	if (frnode == NULL)
		return (NULL);
	MPASS(frnode->fn_fragments >= 1);
	idkey.fr_id = id;
	frag = RB_FIND(pf_frag_tree, &frnode->fn_tree, &idkey);
	if (frag == NULL)
		return (NULL);
	TAILQ_REMOVE(&V_pf_fragqueue, frag, frag_next);
	TAILQ_INSERT_HEAD(&V_pf_fragqueue, frag, frag_next);

	return (frag);
}

static struct pf_frent *
pf_create_fragment(u_short *reason)
{
	struct pf_frent *frent;

	PF_FRAG_ASSERT();

	frent = uma_zalloc(V_pf_frent_z, M_NOWAIT);
	if (frent == NULL) {
		pf_flush_fragments();
		frent = uma_zalloc(V_pf_frent_z, M_NOWAIT);
		if (frent == NULL) {
			REASON_SET(reason, PFRES_MEMORY);
			return (NULL);
		}
	}

	return (frent);
}

/*
 * Calculate the additional holes that were created in the fragment
 * queue by inserting this fragment.  A fragment in the middle
 * creates one more hole by splitting.  For each connected side,
 * it loses one hole.
 * Fragment entry must be in the queue when calling this function.
 */
static int
pf_frent_holes(struct pf_frent *frent)
{
	struct pf_frent *prev = TAILQ_PREV(frent, pf_fragq, fr_next);
	struct pf_frent *next = TAILQ_NEXT(frent, fr_next);
	int holes = 1;

	if (prev == NULL) {
		if (frent->fe_off == 0)
			holes--;
	} else {
		KASSERT(frent->fe_off != 0, ("frent->fe_off != 0"));
		if (frent->fe_off == prev->fe_off + prev->fe_len)
			holes--;
	}
	if (next == NULL) {
		if (!frent->fe_mff)
			holes--;
	} else {
		KASSERT(frent->fe_mff, ("frent->fe_mff"));
		if (next->fe_off == frent->fe_off + frent->fe_len)
			holes--;
	}
	return holes;
}

static inline int
pf_frent_index(struct pf_frent *frent)
{
	/*
	 * We have an array of 16 entry points to the queue.  A full size
	 * 65535 octet IP packet can have 8192 fragments.  So the queue
	 * traversal length is at most 512 and at most 16 entry points are
	 * checked.  We need 128 additional bytes on a 64 bit architecture.
	 */
	CTASSERT(((u_int16_t)0xffff &~ 7) / (0x10000 / PF_FRAG_ENTRY_POINTS) ==
	    16 - 1);
	CTASSERT(((u_int16_t)0xffff >> 3) / PF_FRAG_ENTRY_POINTS == 512 - 1);

	return frent->fe_off / (0x10000 / PF_FRAG_ENTRY_POINTS);
}

static int
pf_frent_insert(struct pf_fragment *frag, struct pf_frent *frent,
    struct pf_frent *prev)
{
	int index;

	CTASSERT(PF_FRAG_ENTRY_LIMIT <= 0xff);

	/*
	 * A packet has at most 65536 octets.  With 16 entry points, each one
	 * spawns 4096 octets.  We limit these to 64 fragments each, which
	 * means on average every fragment must have at least 64 octets.
	 */
	index = pf_frent_index(frent);
	if (frag->fr_entries[index] >= PF_FRAG_ENTRY_LIMIT)
		return ENOBUFS;
	frag->fr_entries[index]++;

	if (prev == NULL) {
		TAILQ_INSERT_HEAD(&frag->fr_queue, frent, fr_next);
	} else {
		KASSERT(prev->fe_off + prev->fe_len <= frent->fe_off,
		    ("overlapping fragment"));
		TAILQ_INSERT_AFTER(&frag->fr_queue, prev, frent, fr_next);
	}

	if (frag->fr_firstoff[index] == NULL) {
		KASSERT(prev == NULL || pf_frent_index(prev) < index,
		    ("prev == NULL || pf_frent_index(pref) < index"));
		frag->fr_firstoff[index] = frent;
	} else {
		if (frent->fe_off < frag->fr_firstoff[index]->fe_off) {
			KASSERT(prev == NULL || pf_frent_index(prev) < index,
			    ("prev == NULL || pf_frent_index(pref) < index"));
			frag->fr_firstoff[index] = frent;
		} else {
			KASSERT(prev != NULL, ("prev != NULL"));
			KASSERT(pf_frent_index(prev) == index,
			    ("pf_frent_index(prev) == index"));
		}
	}

	frag->fr_holes += pf_frent_holes(frent);

	return 0;
}

void
pf_frent_remove(struct pf_fragment *frag, struct pf_frent *frent)
{
#ifdef INVARIANTS
	struct pf_frent *prev = TAILQ_PREV(frent, pf_fragq, fr_next);
#endif /* INVARIANTS */
	struct pf_frent *next = TAILQ_NEXT(frent, fr_next);
	int index;

	frag->fr_holes -= pf_frent_holes(frent);

	index = pf_frent_index(frent);
	KASSERT(frag->fr_firstoff[index] != NULL, ("frent not found"));
	if (frag->fr_firstoff[index]->fe_off == frent->fe_off) {
		if (next == NULL) {
			frag->fr_firstoff[index] = NULL;
		} else {
			KASSERT(frent->fe_off + frent->fe_len <= next->fe_off,
			    ("overlapping fragment"));
			if (pf_frent_index(next) == index) {
				frag->fr_firstoff[index] = next;
			} else {
				frag->fr_firstoff[index] = NULL;
			}
		}
	} else {
		KASSERT(frag->fr_firstoff[index]->fe_off < frent->fe_off,
		    ("frag->fr_firstoff[index]->fe_off < frent->fe_off"));
		KASSERT(prev != NULL, ("prev != NULL"));
		KASSERT(prev->fe_off + prev->fe_len <= frent->fe_off,
		    ("overlapping fragment"));
		KASSERT(pf_frent_index(prev) == index,
		    ("pf_frent_index(prev) == index"));
	}

	TAILQ_REMOVE(&frag->fr_queue, frent, fr_next);

	KASSERT(frag->fr_entries[index] > 0, ("No fragments remaining"));
	frag->fr_entries[index]--;
}

struct pf_frent *
pf_frent_previous(struct pf_fragment *frag, struct pf_frent *frent)
{
	struct pf_frent *prev, *next;
	int index;

	/*
	 * If there are no fragments after frag, take the final one.  Assume
	 * that the global queue is not empty.
	 */
	prev = TAILQ_LAST(&frag->fr_queue, pf_fragq);
	KASSERT(prev != NULL, ("prev != NULL"));
	if (prev->fe_off <= frent->fe_off)
		return prev;
	/*
	 * We want to find a fragment entry that is before frag, but still
	 * close to it.  Find the first fragment entry that is in the same
	 * entry point or in the first entry point after that.  As we have
	 * already checked that there are entries behind frag, this will
	 * succeed.
	 */
	for (index = pf_frent_index(frent); index < PF_FRAG_ENTRY_POINTS;
	    index++) {
		prev = frag->fr_firstoff[index];
		if (prev != NULL)
			break;
	}
	KASSERT(prev != NULL, ("prev != NULL"));
	/*
	 * In prev we may have a fragment from the same entry point that is
	 * before frent, or one that is just one position behind frent.
	 * In the latter case, we go back one step and have the predecessor.
	 * There may be none if the new fragment will be the first one.
	 */
	if (prev->fe_off > frent->fe_off) {
		prev = TAILQ_PREV(prev, pf_fragq, fr_next);
		if (prev == NULL)
			return NULL;
		KASSERT(prev->fe_off <= frent->fe_off,
		    ("prev->fe_off <= frent->fe_off"));
		return prev;
	}
	/*
	 * In prev is the first fragment of the entry point.  The offset
	 * of frag is behind it.  Find the closest previous fragment.
	 */
	for (next = TAILQ_NEXT(prev, fr_next); next != NULL;
	    next = TAILQ_NEXT(next, fr_next)) {
		if (next->fe_off > frent->fe_off)
			break;
		prev = next;
	}
	return prev;
}

static struct pf_fragment *
pf_fillup_fragment(struct pf_frnode *key, uint32_t id,
    struct pf_frent *frent, u_short *reason)
{
	struct pf_frent		*after, *next, *prev;
	struct pf_fragment	*frag;
	struct pf_frnode	*frnode;
	uint16_t		total;

	PF_FRAG_ASSERT();

	/* No empty fragments. */
	if (frent->fe_len == 0) {
		DPFPRINTF(PF_DEBUG_MISC, "bad fragment: len 0");
		goto bad_fragment;
	}

	/* All fragments are 8 byte aligned. */
	if (frent->fe_mff && (frent->fe_len & 0x7)) {
		DPFPRINTF(PF_DEBUG_MISC, "bad fragment: mff and len %d",
		    frent->fe_len);
		goto bad_fragment;
	}

	/* Respect maximum length, IP_MAXPACKET == IPV6_MAXPACKET. */
	if (frent->fe_off + frent->fe_len > IP_MAXPACKET) {
		DPFPRINTF(PF_DEBUG_MISC, "bad fragment: max packet %d",
		    frent->fe_off + frent->fe_len);
		goto bad_fragment;
	}

	if (key->fn_af == AF_INET)
		DPFPRINTF(PF_DEBUG_MISC, "reass frag %d @ %d-%d\n",
		    id, frent->fe_off, frent->fe_off + frent->fe_len);
	else 
		DPFPRINTF(PF_DEBUG_MISC, "reass frag %#08x @ %d-%d",
		    id, frent->fe_off, frent->fe_off + frent->fe_len);

	/* Fully buffer all of the fragments in this fragment queue. */
	frag = pf_find_fragment(key, id);

	/* Create a new reassembly queue for this packet. */
	if (frag == NULL) {
		frag = uma_zalloc(V_pf_frag_z, M_NOWAIT);
		if (frag == NULL) {
			pf_flush_fragments();
			frag = uma_zalloc(V_pf_frag_z, M_NOWAIT);
			if (frag == NULL) {
				REASON_SET(reason, PFRES_MEMORY);
				goto drop_fragment;
			}
		}

		frnode = RB_FIND(pf_frnode_tree, &V_pf_frnode_tree, key);
		if (frnode == NULL) {
			frnode = uma_zalloc(V_pf_frnode_z, M_NOWAIT);
			if (frnode == NULL) {
				pf_flush_fragments();
				frnode = uma_zalloc(V_pf_frnode_z, M_NOWAIT);
				if (frnode == NULL) {
					REASON_SET(reason, PFRES_MEMORY);
					uma_zfree(V_pf_frag_z, frag);
					goto drop_fragment;
				}
			}
			*frnode = *key;
			RB_INIT(&frnode->fn_tree);
			frnode->fn_fragments = 0;
		}
		memset(frag->fr_firstoff, 0, sizeof(frag->fr_firstoff));
		memset(frag->fr_entries, 0, sizeof(frag->fr_entries));
		frag->fr_timeout = time_uptime;
		TAILQ_INIT(&frag->fr_queue);
		frag->fr_maxlen = frent->fe_len;
		frag->fr_holes = 1;

		frag->fr_id = id;
		frag->fr_node = frnode;
		/* RB_INSERT cannot fail as pf_find_fragment() found nothing */
		RB_INSERT(pf_frag_tree, &frnode->fn_tree, frag);
		frnode->fn_fragments++;
		if (frnode->fn_fragments == 1)
			RB_INSERT(pf_frnode_tree, &V_pf_frnode_tree, frnode);

		TAILQ_INSERT_HEAD(&V_pf_fragqueue, frag, frag_next);

		/* We do not have a previous fragment, cannot fail. */
		pf_frent_insert(frag, frent, NULL);

		return (frag);
	}

	KASSERT(!TAILQ_EMPTY(&frag->fr_queue), ("!TAILQ_EMPTY()->fr_queue"));
	MPASS(frag->fr_node);

	/* Remember maximum fragment len for refragmentation. */
	if (frent->fe_len > frag->fr_maxlen)
		frag->fr_maxlen = frent->fe_len;

	/* Maximum data we have seen already. */
	total = TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_off +
		TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_len;

	/* Non terminal fragments must have more fragments flag. */
	if (frent->fe_off + frent->fe_len < total && !frent->fe_mff)
		goto free_ipv6_fragment;

	/* Check if we saw the last fragment already. */
	if (!TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_mff) {
		if (frent->fe_off + frent->fe_len > total ||
		    (frent->fe_off + frent->fe_len == total && frent->fe_mff))
			goto free_ipv6_fragment;
	} else {
		if (frent->fe_off + frent->fe_len == total && !frent->fe_mff)
			goto free_ipv6_fragment;
	}

	/* Find neighbors for newly inserted fragment */
	prev = pf_frent_previous(frag, frent);
	if (prev == NULL) {
		after = TAILQ_FIRST(&frag->fr_queue);
		KASSERT(after != NULL, ("after != NULL"));
	} else {
		after = TAILQ_NEXT(prev, fr_next);
	}

	if (prev != NULL && prev->fe_off + prev->fe_len > frent->fe_off) {
		uint16_t precut;

		if (frag->fr_node->fn_af == AF_INET6)
			goto free_fragment;

		precut = prev->fe_off + prev->fe_len - frent->fe_off;
		if (precut >= frent->fe_len) {
			DPFPRINTF(PF_DEBUG_MISC, "new frag overlapped");
			goto drop_fragment;
		}
		DPFPRINTF(PF_DEBUG_MISC, "frag head overlap %d", precut);
		m_adj(frent->fe_m, precut);
		frent->fe_off += precut;
		frent->fe_len -= precut;
	}

	for (; after != NULL && frent->fe_off + frent->fe_len > after->fe_off;
	    after = next) {
		uint16_t aftercut;

		aftercut = frent->fe_off + frent->fe_len - after->fe_off;
		if (aftercut < after->fe_len) {
			DPFPRINTF(PF_DEBUG_MISC, "frag tail overlap %d",
			    aftercut);
			m_adj(after->fe_m, aftercut);
			/* Fragment may switch queue as fe_off changes */
			pf_frent_remove(frag, after);
			after->fe_off += aftercut;
			after->fe_len -= aftercut;
			/* Insert into correct queue */
			if (pf_frent_insert(frag, after, prev)) {
				DPFPRINTF(PF_DEBUG_MISC,
				    "fragment requeue limit exceeded");
				m_freem(after->fe_m);
				uma_zfree(V_pf_frent_z, after);
				/* There is not way to recover */
				goto free_fragment;
			}
			break;
		}

		/* This fragment is completely overlapped, lose it. */
		DPFPRINTF(PF_DEBUG_MISC, "old frag overlapped");
		next = TAILQ_NEXT(after, fr_next);
		pf_frent_remove(frag, after);
		m_freem(after->fe_m);
		uma_zfree(V_pf_frent_z, after);
	}

	/* If part of the queue gets too long, there is not way to recover. */
	if (pf_frent_insert(frag, frent, prev)) {
		DPFPRINTF(PF_DEBUG_MISC, "fragment queue limit exceeded");
		goto bad_fragment;
	}

	return (frag);

free_ipv6_fragment:
	if (frag->fr_node->fn_af == AF_INET)
		goto bad_fragment;
free_fragment:
	/*
	 * RFC 5722, Errata 3089:  When reassembling an IPv6 datagram, if one
	 * or more its constituent fragments is determined to be an overlapping
	 * fragment, the entire datagram (and any constituent fragments) MUST
	 * be silently discarded.
	 */
	DPFPRINTF(PF_DEBUG_MISC, "flush overlapping fragments");
	pf_free_fragment(frag);

bad_fragment:
	REASON_SET(reason, PFRES_FRAG);
drop_fragment:
	uma_zfree(V_pf_frent_z, frent);
	return (NULL);
}

static struct mbuf *
pf_join_fragment(struct pf_fragment *frag)
{
	struct mbuf *m, *m2;
	struct pf_frent	*frent;

	frent = TAILQ_FIRST(&frag->fr_queue);
	TAILQ_REMOVE(&frag->fr_queue, frent, fr_next);

	m = frent->fe_m;
	if ((frent->fe_hdrlen + frent->fe_len) < m->m_pkthdr.len)
		m_adj(m, (frent->fe_hdrlen + frent->fe_len) - m->m_pkthdr.len);
	uma_zfree(V_pf_frent_z, frent);
	while ((frent = TAILQ_FIRST(&frag->fr_queue)) != NULL) {
		TAILQ_REMOVE(&frag->fr_queue, frent, fr_next);

		m2 = frent->fe_m;
		/* Strip off ip header. */
		m_adj(m2, frent->fe_hdrlen);
		/* Strip off any trailing bytes. */
		if (frent->fe_len < m2->m_pkthdr.len)
			m_adj(m2, frent->fe_len - m2->m_pkthdr.len);

		uma_zfree(V_pf_frent_z, frent);
		m_cat(m, m2);
	}

	/* Remove from fragment queue. */
	pf_free_fragment(frag);

	return (m);
}

#ifdef INET
static int
pf_reassemble(struct mbuf **m0, u_short *reason)
{
	struct mbuf		*m = *m0;
	struct ip		*ip = mtod(m, struct ip *);
	struct pf_frent		*frent;
	struct pf_fragment	*frag;
	struct m_tag		*mtag;
	struct pf_fragment_tag	*ftag;
	struct pf_frnode	 key;
	uint16_t		 total, hdrlen;
	uint32_t		 frag_id;
	uint16_t		 maxlen;

	/* Get an entry for the fragment queue */
	if ((frent = pf_create_fragment(reason)) == NULL)
		return (PF_DROP);

	frent->fe_m = m;
	frent->fe_hdrlen = ip->ip_hl << 2;
	frent->fe_extoff = 0;
	frent->fe_len = ntohs(ip->ip_len) - (ip->ip_hl << 2);
	frent->fe_off = (ntohs(ip->ip_off) & IP_OFFMASK) << 3;
	frent->fe_mff = ntohs(ip->ip_off) & IP_MF;

	pf_ip2key(ip, &key);

	if ((frag = pf_fillup_fragment(&key, ip->ip_id, frent, reason)) == NULL)
		return (PF_DROP);

	/* The mbuf is part of the fragment entry, no direct free or access */
	m = *m0 = NULL;

	if (frag->fr_holes) {
		DPFPRINTF(PF_DEBUG_MISC, "frag %d, holes %d",
		    frag->fr_id, frag->fr_holes);
		return (PF_PASS);  /* drop because *m0 is NULL, no error */
	}

	/* We have all the data */
	frent = TAILQ_FIRST(&frag->fr_queue);
	KASSERT(frent != NULL, ("frent != NULL"));
	total = TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_off +
		TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_len;
	hdrlen = frent->fe_hdrlen;

	maxlen = frag->fr_maxlen;
	frag_id = frag->fr_id;
	m = *m0 = pf_join_fragment(frag);
	frag = NULL;

	if (m->m_flags & M_PKTHDR) {
		int plen = 0;
		for (m = *m0; m; m = m->m_next)
			plen += m->m_len;
		m = *m0;
		m->m_pkthdr.len = plen;
	}

	if ((mtag = m_tag_get(PACKET_TAG_PF_REASSEMBLED,
	    sizeof(struct pf_fragment_tag), M_NOWAIT)) == NULL) {
		REASON_SET(reason, PFRES_SHORT);
		/* PF_DROP requires a valid mbuf *m0 in pf_test() */
		return (PF_DROP);
	}
	ftag = (struct pf_fragment_tag *)(mtag + 1);
	ftag->ft_hdrlen = hdrlen;
	ftag->ft_extoff = 0;
	ftag->ft_maxlen = maxlen;
	ftag->ft_id = frag_id;
	m_tag_prepend(m, mtag);

	ip = mtod(m, struct ip *);
	ip->ip_sum = pf_cksum_fixup(ip->ip_sum, ip->ip_len,
	    htons(hdrlen + total), 0);
	ip->ip_len = htons(hdrlen + total);
	ip->ip_sum = pf_cksum_fixup(ip->ip_sum, ip->ip_off,
	    ip->ip_off & ~(IP_MF|IP_OFFMASK), 0);
	ip->ip_off &= ~(IP_MF|IP_OFFMASK);

	if (hdrlen + total > IP_MAXPACKET) {
		DPFPRINTF(PF_DEBUG_MISC, "drop: too big: %d", total);
		ip->ip_len = 0;
		REASON_SET(reason, PFRES_SHORT);
		/* PF_DROP requires a valid mbuf *m0 in pf_test() */
		return (PF_DROP);
	}

	DPFPRINTF(PF_DEBUG_MISC, "complete: %p(%d)", m, ntohs(ip->ip_len));
	return (PF_PASS);
}
#endif	/* INET */

#ifdef INET6
static int
pf_reassemble6(struct mbuf **m0, struct ip6_frag *fraghdr,
    uint16_t hdrlen, uint16_t extoff, u_short *reason)
{
	struct mbuf		*m = *m0;
	struct ip6_hdr		*ip6 = mtod(m, struct ip6_hdr *);
	struct pf_frent		*frent;
	struct pf_fragment	*frag;
	struct pf_frnode	 key;
	struct m_tag		*mtag;
	struct pf_fragment_tag	*ftag;
	int			 off;
	uint32_t		 frag_id;
	uint16_t		 total, maxlen;
	uint8_t			 proto;

	PF_FRAG_LOCK();

	/* Get an entry for the fragment queue. */
	if ((frent = pf_create_fragment(reason)) == NULL) {
		PF_FRAG_UNLOCK();
		return (PF_DROP);
	}

	frent->fe_m = m;
	frent->fe_hdrlen = hdrlen;
	frent->fe_extoff = extoff;
	frent->fe_len = sizeof(struct ip6_hdr) + ntohs(ip6->ip6_plen) - hdrlen;
	frent->fe_off = ntohs(fraghdr->ip6f_offlg & IP6F_OFF_MASK);
	frent->fe_mff = fraghdr->ip6f_offlg & IP6F_MORE_FRAG;

	key.fn_src.v6 = ip6->ip6_src;
	key.fn_dst.v6 = ip6->ip6_dst;
	key.fn_af = AF_INET6;
	/* Only the first fragment's protocol is relevant. */
	key.fn_proto = 0;

	if ((frag = pf_fillup_fragment(&key, fraghdr->ip6f_ident, frent, reason)) == NULL) {
		PF_FRAG_UNLOCK();
		return (PF_DROP);
	}

	/* The mbuf is part of the fragment entry, no direct free or access. */
	m = *m0 = NULL;

	if (frag->fr_holes) {
		DPFPRINTF(PF_DEBUG_MISC, "frag %d, holes %d", frag->fr_id,
		    frag->fr_holes);
		PF_FRAG_UNLOCK();
		return (PF_PASS);  /* Drop because *m0 is NULL, no error. */
	}

	/* We have all the data. */
	frent = TAILQ_FIRST(&frag->fr_queue);
	KASSERT(frent != NULL, ("frent != NULL"));
	extoff = frent->fe_extoff;
	maxlen = frag->fr_maxlen;
	frag_id = frag->fr_id;
	total = TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_off +
		TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_len;
	hdrlen = frent->fe_hdrlen - sizeof(struct ip6_frag);

	m = *m0 = pf_join_fragment(frag);
	frag = NULL;

	PF_FRAG_UNLOCK();

	/* Take protocol from first fragment header. */
	m = m_getptr(m, hdrlen + offsetof(struct ip6_frag, ip6f_nxt), &off);
	KASSERT(m, ("%s: short mbuf chain", __func__));
	proto = *(mtod(m, uint8_t *) + off);
	m = *m0;

	/* Delete frag6 header */
	if (ip6_deletefraghdr(m, hdrlen, M_NOWAIT) != 0)
		goto fail;

	if (m->m_flags & M_PKTHDR) {
		int plen = 0;
		for (m = *m0; m; m = m->m_next)
			plen += m->m_len;
		m = *m0;
		m->m_pkthdr.len = plen;
	}

	if ((mtag = m_tag_get(PACKET_TAG_PF_REASSEMBLED,
	    sizeof(struct pf_fragment_tag), M_NOWAIT)) == NULL)
		goto fail;
	ftag = (struct pf_fragment_tag *)(mtag + 1);
	ftag->ft_hdrlen = hdrlen;
	ftag->ft_extoff = extoff;
	ftag->ft_maxlen = maxlen;
	ftag->ft_id = frag_id;
	m_tag_prepend(m, mtag);

	ip6 = mtod(m, struct ip6_hdr *);
	ip6->ip6_plen = htons(hdrlen - sizeof(struct ip6_hdr) + total);
	if (extoff) {
		/* Write protocol into next field of last extension header. */
		m = m_getptr(m, extoff + offsetof(struct ip6_ext, ip6e_nxt),
		    &off);
		KASSERT(m, ("%s: short mbuf chain", __func__));
		*(mtod(m, char *) + off) = proto;
		m = *m0;
	} else
		ip6->ip6_nxt = proto;

	if (hdrlen - sizeof(struct ip6_hdr) + total > IPV6_MAXPACKET) {
		DPFPRINTF(PF_DEBUG_MISC, "drop: too big: %d", total);
		ip6->ip6_plen = 0;
		REASON_SET(reason, PFRES_SHORT);
		/* PF_DROP requires a valid mbuf *m0 in pf_test6(). */
		return (PF_DROP);
	}

	DPFPRINTF(PF_DEBUG_MISC, "complete: %p(%d)", m,
	    ntohs(ip6->ip6_plen));
	return (PF_PASS);

fail:
	REASON_SET(reason, PFRES_MEMORY);
	/* PF_DROP requires a valid mbuf *m0 in pf_test6(), will free later. */
	return (PF_DROP);
}
#endif	/* INET6 */

#ifdef INET6
int
pf_max_frag_size(struct mbuf *m)
{
	struct m_tag *tag;
	struct pf_fragment_tag *ftag;

	tag = m_tag_find(m, PACKET_TAG_PF_REASSEMBLED, NULL);
	if (tag == NULL)
		return (m->m_pkthdr.len);

	ftag = (struct pf_fragment_tag *)(tag + 1);

	return (ftag->ft_maxlen);
}

int
pf_refragment6(struct ifnet *ifp, struct mbuf **m0, struct m_tag *mtag,
    struct ifnet *rt, bool forward)
{
	struct mbuf		*m = *m0, *t;
	struct ip6_hdr		*hdr;
	struct pf_fragment_tag	*ftag = (struct pf_fragment_tag *)(mtag + 1);
	struct pf_pdesc		 pd;
	uint32_t		 frag_id;
	uint16_t		 hdrlen, extoff, maxlen;
	uint8_t			 proto;
	int			 error, action;

	hdrlen = ftag->ft_hdrlen;
	extoff = ftag->ft_extoff;
	maxlen = ftag->ft_maxlen;
	frag_id = ftag->ft_id;
	m_tag_delete(m, mtag);
	mtag = NULL;
	ftag = NULL;

	if (extoff) {
		int off;

		/* Use protocol from next field of last extension header */
		m = m_getptr(m, extoff + offsetof(struct ip6_ext, ip6e_nxt),
		    &off);
		KASSERT((m != NULL), ("pf_refragment6: short mbuf chain"));
		proto = *(mtod(m, uint8_t *) + off);
		*(mtod(m, char *) + off) = IPPROTO_FRAGMENT;
		m = *m0;
	} else {
		hdr = mtod(m, struct ip6_hdr *);
		proto = hdr->ip6_nxt;
		hdr->ip6_nxt = IPPROTO_FRAGMENT;
	}

	/* In case of link-local traffic we'll need a scope set. */
	hdr = mtod(m, struct ip6_hdr *);

	in6_setscope(&hdr->ip6_src, ifp, NULL);
	in6_setscope(&hdr->ip6_dst, ifp, NULL);

	/* The MTU must be a multiple of 8 bytes, or we risk doing the
	 * fragmentation wrong. */
	maxlen = maxlen & ~7;

	/*
	 * Maxlen may be less than 8 if there was only a single
	 * fragment.  As it was fragmented before, add a fragment
	 * header also for a single fragment.  If total or maxlen
	 * is less than 8, ip6_fragment() will return EMSGSIZE and
	 * we drop the packet.
	 */
	error = ip6_fragment(ifp, m, hdrlen, proto, maxlen, frag_id);
	m = (*m0)->m_nextpkt;
	(*m0)->m_nextpkt = NULL;
	if (error == 0) {
		/* The first mbuf contains the unfragmented packet. */
		m_freem(*m0);
		*m0 = NULL;
		action = PF_PASS;
	} else {
		/* Drop expects an mbuf to free. */
		DPFPRINTF(PF_DEBUG_MISC, "refragment error %d", error);
		action = PF_DROP;
	}
	for (; m; m = t) {
		t = m->m_nextpkt;
		m->m_nextpkt = NULL;
		m->m_flags |= M_SKIP_FIREWALL;
		memset(&pd, 0, sizeof(pd));
		pd.pf_mtag = pf_find_mtag(m);
		if (error != 0) {
			m_freem(m);
			continue;
		}
		if (rt != NULL) {
			struct sockaddr_in6	dst;
			hdr = mtod(m, struct ip6_hdr *);

			bzero(&dst, sizeof(dst));
			dst.sin6_family = AF_INET6;
			dst.sin6_len = sizeof(dst);
			dst.sin6_addr = hdr->ip6_dst;

			if (m->m_pkthdr.len <= if_getmtu(ifp)) {
				nd6_output_ifp(rt, rt, m, &dst, NULL);
			} else {
				in6_ifstat_inc(ifp, ifs6_in_toobig);
				icmp6_error(m, ICMP6_PACKET_TOO_BIG, 0,
				    if_getmtu(ifp));
			}
		} else if (forward) {
			MPASS(m->m_pkthdr.rcvif != NULL);
			ip6_forward(m, 0);
		} else {
			(void)ip6_output(m, NULL, NULL, 0, NULL, NULL,
			    NULL);
		}
	}

	return (action);
}
#endif /* INET6 */

#ifdef INET
int
pf_normalize_ip(u_short *reason, struct pf_pdesc *pd)
{
	struct pf_krule		*r;
	struct ip		*h = mtod(pd->m, struct ip *);
	int			 mff = (ntohs(h->ip_off) & IP_MF);
	int			 hlen = h->ip_hl << 2;
	u_int16_t		 fragoff = (ntohs(h->ip_off) & IP_OFFMASK) << 3;
	u_int16_t		 max;
	int			 ip_len;
	int			 tag = -1;
	int			 verdict;
	bool			 scrub_compat;

	PF_RULES_RASSERT();

	r = TAILQ_FIRST(pf_main_ruleset.rules[PF_RULESET_SCRUB].active.ptr);
	/*
	 * Check if there are any scrub rules, matching or not.
	 * Lack of scrub rules means:
	 *  - enforced packet normalization operation just like in OpenBSD
	 *  - fragment reassembly depends on V_pf_status.reass
	 * With scrub rules:
	 *  - packet normalization is performed if there is a matching scrub rule
	 *  - fragment reassembly is performed if the matching rule has no
	 *    PFRULE_FRAGMENT_NOREASS flag
	 */
	scrub_compat = (r != NULL);
	while (r != NULL) {
		pf_counter_u64_add(&r->evaluations, 1);
		if (pfi_kkif_match(r->kif, pd->kif) == r->ifnot)
			r = r->skip[PF_SKIP_IFP];
		else if (r->direction && r->direction != pd->dir)
			r = r->skip[PF_SKIP_DIR];
		else if (r->af && r->af != AF_INET)
			r = r->skip[PF_SKIP_AF];
		else if (r->proto && r->proto != h->ip_p)
			r = r->skip[PF_SKIP_PROTO];
		else if (PF_MISMATCHAW(&r->src.addr,
		    (struct pf_addr *)&h->ip_src.s_addr, AF_INET,
		    r->src.neg, pd->kif, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_SRC_ADDR];
		else if (PF_MISMATCHAW(&r->dst.addr,
		    (struct pf_addr *)&h->ip_dst.s_addr, AF_INET,
		    r->dst.neg, NULL, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_DST_ADDR];
		else if (r->match_tag && !pf_match_tag(pd->m, r, &tag,
		    pd->pf_mtag ? pd->pf_mtag->tag : 0))
			r = TAILQ_NEXT(r, entries);
		else
			break;
	}

	if (scrub_compat) {
		/* With scrub rules present IPv4 normalization happens only
		 * if one of rules has matched and it's not a "no scrub" rule */
		if (r == NULL || r->action == PF_NOSCRUB)
			return (PF_PASS);

		pf_counter_u64_critical_enter();
		pf_counter_u64_add_protected(&r->packets[pd->dir == PF_OUT], 1);
		pf_counter_u64_add_protected(&r->bytes[pd->dir == PF_OUT], pd->tot_len);
		pf_counter_u64_critical_exit();
		pf_rule_to_actions(r, &pd->act);
	}

	/* Check for illegal packets */
	if (hlen < (int)sizeof(struct ip)) {
		REASON_SET(reason, PFRES_NORM);
		goto drop;
	}

	if (hlen > ntohs(h->ip_len)) {
		REASON_SET(reason, PFRES_NORM);
		goto drop;
	}

	/* Clear IP_DF if the rule uses the no-df option or we're in no-df mode */
	if (((!scrub_compat && V_pf_status.reass & PF_REASS_NODF) ||
	    (r != NULL && r->rule_flag & PFRULE_NODF)) &&
	    (h->ip_off & htons(IP_DF))
	) {
		u_int16_t ip_off = h->ip_off;

		h->ip_off &= htons(~IP_DF);
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_off, h->ip_off, 0);
	}

	/* We will need other tests here */
	if (!fragoff && !mff)
		goto no_fragment;

	/* We're dealing with a fragment now. Don't allow fragments
	 * with IP_DF to enter the cache. If the flag was cleared by
	 * no-df above, fine. Otherwise drop it.
	 */
	if (h->ip_off & htons(IP_DF)) {
		DPFPRINTF(PF_DEBUG_MISC, "IP_DF");
		goto bad;
	}

	ip_len = ntohs(h->ip_len) - hlen;

	/* All fragments are 8 byte aligned */
	if (mff && (ip_len & 0x7)) {
		DPFPRINTF(PF_DEBUG_MISC, "mff and %d", ip_len);
		goto bad;
	}

	/* Respect maximum length */
	if (fragoff + ip_len > IP_MAXPACKET) {
		DPFPRINTF(PF_DEBUG_MISC, "max packet %d", fragoff + ip_len);
		goto bad;
	}

	if ((!scrub_compat && V_pf_status.reass) ||
	    (r != NULL && !(r->rule_flag & PFRULE_FRAGMENT_NOREASS))
	) {
		max = fragoff + ip_len;

		/* Fully buffer all of the fragments
		 * Might return a completely reassembled mbuf, or NULL */
		PF_FRAG_LOCK();
		DPFPRINTF(PF_DEBUG_MISC, "reass frag %d @ %d-%d",
		    h->ip_id, fragoff, max);
		verdict = pf_reassemble(&pd->m, reason);
		PF_FRAG_UNLOCK();

		if (verdict != PF_PASS)
			return (PF_DROP);

		if (pd->m == NULL)
			return (PF_DROP);

		h = mtod(pd->m, struct ip *);
		pd->tot_len = htons(h->ip_len);

 no_fragment:
		/* At this point, only IP_DF is allowed in ip_off */
		if (h->ip_off & ~htons(IP_DF)) {
			u_int16_t ip_off = h->ip_off;

			h->ip_off &= htons(IP_DF);
			h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_off, h->ip_off, 0);
		}
	}

	return (PF_PASS);

 bad:
	DPFPRINTF(PF_DEBUG_MISC, "dropping bad fragment");
	REASON_SET(reason, PFRES_FRAG);
 drop:
	if (r != NULL && r->log)
		PFLOG_PACKET(PF_DROP, *reason, r, NULL, NULL, pd, 1, NULL);

	return (PF_DROP);
}
#endif

#ifdef INET6
int
pf_normalize_ip6(int off, u_short *reason,
    struct pf_pdesc *pd)
{
	struct pf_krule		*r;
	struct ip6_hdr		*h;
	struct ip6_frag		 frag;
	bool			 scrub_compat;

	PF_RULES_RASSERT();

	r = TAILQ_FIRST(pf_main_ruleset.rules[PF_RULESET_SCRUB].active.ptr);
	/*
	 * Check if there are any scrub rules, matching or not.
	 * Lack of scrub rules means:
	 *  - enforced packet normalization operation just like in OpenBSD
	 * With scrub rules:
	 *  - packet normalization is performed if there is a matching scrub rule
	 * XXX: Fragment reassembly always performed for IPv6!
	 */
	scrub_compat = (r != NULL);
	while (r != NULL) {
		pf_counter_u64_add(&r->evaluations, 1);
		if (pfi_kkif_match(r->kif, pd->kif) == r->ifnot)
			r = r->skip[PF_SKIP_IFP];
		else if (r->direction && r->direction != pd->dir)
			r = r->skip[PF_SKIP_DIR];
		else if (r->af && r->af != AF_INET6)
			r = r->skip[PF_SKIP_AF];
		else if (r->proto && r->proto != pd->proto)
			r = r->skip[PF_SKIP_PROTO];
		else if (PF_MISMATCHAW(&r->src.addr,
		    (struct pf_addr *)&pd->src, AF_INET6,
		    r->src.neg, pd->kif, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_SRC_ADDR];
		else if (PF_MISMATCHAW(&r->dst.addr,
		    (struct pf_addr *)&pd->dst, AF_INET6,
		    r->dst.neg, NULL, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_DST_ADDR];
		else
			break;
	}

	if (scrub_compat) {
		/* With scrub rules present IPv6 normalization happens only
		 * if one of rules has matched and it's not a "no scrub" rule */
		if (r == NULL || r->action == PF_NOSCRUB)
			return (PF_PASS);

		pf_counter_u64_critical_enter();
		pf_counter_u64_add_protected(&r->packets[pd->dir == PF_OUT], 1);
		pf_counter_u64_add_protected(&r->bytes[pd->dir == PF_OUT], pd->tot_len);
		pf_counter_u64_critical_exit();
		pf_rule_to_actions(r, &pd->act);
	}

	if (!pf_pull_hdr(pd->m, off, &frag, sizeof(frag), NULL, reason, AF_INET6))
		return (PF_DROP);

	/* Offset now points to data portion. */
	off += sizeof(frag);

	if (pd->virtual_proto == PF_VPROTO_FRAGMENT) {
		/* Returns PF_DROP or *m0 is NULL or completely reassembled
		 * mbuf. */
		if (pf_reassemble6(&pd->m, &frag, off, pd->extoff, reason) != PF_PASS)
			return (PF_DROP);
		if (pd->m == NULL)
			return (PF_DROP);
		h = mtod(pd->m, struct ip6_hdr *);
		pd->tot_len = ntohs(h->ip6_plen) + sizeof(struct ip6_hdr);
	}

	return (PF_PASS);
}
#endif /* INET6 */

int
pf_normalize_tcp(struct pf_pdesc *pd)
{
	struct pf_krule	*r, *rm = NULL;
	struct tcphdr	*th = &pd->hdr.tcp;
	int		 rewrite = 0;
	u_short		 reason;
	u_int16_t	 flags;
	sa_family_t	 af = pd->af;
	int		 srs;

	PF_RULES_RASSERT();

	r = TAILQ_FIRST(pf_main_ruleset.rules[PF_RULESET_SCRUB].active.ptr);
	/* Check if there any scrub rules. Lack of scrub rules means enforced
	 * packet normalization operation just like in OpenBSD. */
	srs = (r != NULL);
	while (r != NULL) {
		pf_counter_u64_add(&r->evaluations, 1);
		if (pfi_kkif_match(r->kif, pd->kif) == r->ifnot)
			r = r->skip[PF_SKIP_IFP];
		else if (r->direction && r->direction != pd->dir)
			r = r->skip[PF_SKIP_DIR];
		else if (r->af && r->af != af)
			r = r->skip[PF_SKIP_AF];
		else if (r->proto && r->proto != pd->proto)
			r = r->skip[PF_SKIP_PROTO];
		else if (PF_MISMATCHAW(&r->src.addr, pd->src, af,
		    r->src.neg, pd->kif, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_SRC_ADDR];
		else if (r->src.port_op && !pf_match_port(r->src.port_op,
			    r->src.port[0], r->src.port[1], th->th_sport))
			r = r->skip[PF_SKIP_SRC_PORT];
		else if (PF_MISMATCHAW(&r->dst.addr, pd->dst, af,
		    r->dst.neg, NULL, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_DST_ADDR];
		else if (r->dst.port_op && !pf_match_port(r->dst.port_op,
			    r->dst.port[0], r->dst.port[1], th->th_dport))
			r = r->skip[PF_SKIP_DST_PORT];
		else if (r->os_fingerprint != PF_OSFP_ANY && !pf_osfp_match(
			    pf_osfp_fingerprint(pd, th),
			    r->os_fingerprint))
			r = TAILQ_NEXT(r, entries);
		else {
			rm = r;
			break;
		}
	}

	if (srs) {
		/* With scrub rules present TCP normalization happens only
		 * if one of rules has matched and it's not a "no scrub" rule */
		if (rm == NULL || rm->action == PF_NOSCRUB)
			return (PF_PASS);

		pf_counter_u64_critical_enter();
		pf_counter_u64_add_protected(&r->packets[pd->dir == PF_OUT], 1);
		pf_counter_u64_add_protected(&r->bytes[pd->dir == PF_OUT], pd->tot_len);
		pf_counter_u64_critical_exit();
		pf_rule_to_actions(rm, &pd->act);
	}

	if (rm && rm->rule_flag & PFRULE_REASSEMBLE_TCP)
		pd->flags |= PFDESC_TCP_NORM;

	flags = tcp_get_flags(th);
	if (flags & TH_SYN) {
		/* Illegal packet */
		if (flags & TH_RST)
			goto tcp_drop;

		if (flags & TH_FIN)
			goto tcp_drop;
	} else {
		/* Illegal packet */
		if (!(flags & (TH_ACK|TH_RST)))
			goto tcp_drop;
	}

	if (!(flags & TH_ACK)) {
		/* These flags are only valid if ACK is set */
		if ((flags & TH_FIN) || (flags & TH_PUSH) || (flags & TH_URG))
			goto tcp_drop;
	}

	/* Check for illegal header length */
	if (th->th_off < (sizeof(struct tcphdr) >> 2))
		goto tcp_drop;

	/* If flags changed, or reserved data set, then adjust */
	if (flags != tcp_get_flags(th) ||
	    (tcp_get_flags(th) & (TH_RES1|TH_RES2|TH_RES2)) != 0) {
		u_int16_t	ov, nv;

		ov = *(u_int16_t *)(&th->th_ack + 1);
		flags &= ~(TH_RES1 | TH_RES2 | TH_RES3);
		tcp_set_flags(th, flags);
		nv = *(u_int16_t *)(&th->th_ack + 1);

		th->th_sum = pf_proto_cksum_fixup(pd->m, th->th_sum, ov, nv, 0);
		rewrite = 1;
	}

	/* Remove urgent pointer, if TH_URG is not set */
	if (!(flags & TH_URG) && th->th_urp) {
		th->th_sum = pf_proto_cksum_fixup(pd->m, th->th_sum, th->th_urp,
		    0, 0);
		th->th_urp = 0;
		rewrite = 1;
	}

	/* copy back packet headers if we sanitized */
	if (rewrite)
		m_copyback(pd->m, pd->off, sizeof(*th), (caddr_t)th);

	return (PF_PASS);

 tcp_drop:
	REASON_SET(&reason, PFRES_NORM);
	if (rm != NULL && r->log)
		PFLOG_PACKET(PF_DROP, reason, r, NULL, NULL, pd, 1, NULL);
	return (PF_DROP);
}

int
pf_normalize_tcp_init(struct pf_pdesc *pd, struct tcphdr *th,
    struct pf_state_peer *src)
{
	u_int32_t tsval, tsecr;
	int		 olen;
	uint8_t		 opts[MAX_TCPOPTLEN], *opt;

	KASSERT((src->scrub == NULL),
	    ("pf_normalize_tcp_init: src->scrub != NULL"));

	src->scrub = uma_zalloc(V_pf_state_scrub_z, M_ZERO | M_NOWAIT);
	if (src->scrub == NULL)
		return (1);

	switch (pd->af) {
#ifdef INET
	case AF_INET: {
		struct ip *h = mtod(pd->m, struct ip *);
		src->scrub->pfss_ttl = h->ip_ttl;
		break;
	}
#endif /* INET */
#ifdef INET6
	case AF_INET6: {
		struct ip6_hdr *h = mtod(pd->m, struct ip6_hdr *);
		src->scrub->pfss_ttl = h->ip6_hlim;
		break;
	}
#endif /* INET6 */
	default:
		unhandled_af(pd->af);
	}

	/*
	 * All normalizations below are only begun if we see the start of
	 * the connections.  They must all set an enabled bit in pfss_flags
	 */
	if ((tcp_get_flags(th) & TH_SYN) == 0)
		return (0);

	olen = (th->th_off << 2) - sizeof(*th);
	if (olen < TCPOLEN_TIMESTAMP || !pf_pull_hdr(pd->m,
	    pd->off + sizeof(*th), opts, olen, NULL, NULL, pd->af))
		return (0);

	opt = opts;
	while ((opt = pf_find_tcpopt(opt, opts, olen,
	    TCPOPT_TIMESTAMP, TCPOLEN_TIMESTAMP)) != NULL) {
		src->scrub->pfss_flags |= PFSS_TIMESTAMP;
		src->scrub->pfss_ts_mod = arc4random();
		/* note PFSS_PAWS not set yet */
		memcpy(&tsval, &opt[2], sizeof(u_int32_t));
		memcpy(&tsecr, &opt[6], sizeof(u_int32_t));
		src->scrub->pfss_tsval0 = ntohl(tsval);
		src->scrub->pfss_tsval = ntohl(tsval);
		src->scrub->pfss_tsecr = ntohl(tsecr);
		getmicrouptime(&src->scrub->pfss_last);

		opt += opt[1];
	}

	return (0);
}

void
pf_normalize_tcp_cleanup(struct pf_kstate *state)
{
	/* XXX Note: this also cleans up SCTP. */
	uma_zfree(V_pf_state_scrub_z, state->src.scrub);
	uma_zfree(V_pf_state_scrub_z, state->dst.scrub);

	/* Someday... flush the TCP segment reassembly descriptors. */
}
int
pf_normalize_sctp_init(struct pf_pdesc *pd, struct pf_state_peer *src,
    struct pf_state_peer *dst)
{
	src->scrub = uma_zalloc(V_pf_state_scrub_z, M_ZERO | M_NOWAIT);
	if (src->scrub == NULL)
		return (1);

	dst->scrub = uma_zalloc(V_pf_state_scrub_z, M_ZERO | M_NOWAIT);
	if (dst->scrub == NULL) {
		uma_zfree(V_pf_state_scrub_z, src);
		return (1);
	}

	dst->scrub->pfss_v_tag = pd->sctp_initiate_tag;

	return (0);
}

int
pf_normalize_tcp_stateful(struct pf_pdesc *pd,
    u_short *reason, struct tcphdr *th, struct pf_kstate *state,
    struct pf_state_peer *src, struct pf_state_peer *dst, int *writeback)
{
	struct timeval uptime;
	u_int tsval_from_last;
	uint32_t tsval, tsecr;
	int copyback = 0;
	int got_ts = 0;
	int olen;
	uint8_t opts[MAX_TCPOPTLEN], *opt;

	KASSERT((src->scrub || dst->scrub),
	    ("%s: src->scrub && dst->scrub!", __func__));

	/*
	 * Enforce the minimum TTL seen for this connection.  Negate a common
	 * technique to evade an intrusion detection system and confuse
	 * firewall state code.
	 */
	switch (pd->af) {
#ifdef INET
	case AF_INET: {
		if (src->scrub) {
			struct ip *h = mtod(pd->m, struct ip *);
			if (h->ip_ttl > src->scrub->pfss_ttl)
				src->scrub->pfss_ttl = h->ip_ttl;
			h->ip_ttl = src->scrub->pfss_ttl;
		}
		break;
	}
#endif /* INET */
#ifdef INET6
	case AF_INET6: {
		if (src->scrub) {
			struct ip6_hdr *h = mtod(pd->m, struct ip6_hdr *);
			if (h->ip6_hlim > src->scrub->pfss_ttl)
				src->scrub->pfss_ttl = h->ip6_hlim;
			h->ip6_hlim = src->scrub->pfss_ttl;
		}
		break;
	}
#endif /* INET6 */
	default:
		unhandled_af(pd->af);
	}

	olen = (th->th_off << 2) - sizeof(*th);

	if (olen >= TCPOLEN_TIMESTAMP &&
	    ((src->scrub && (src->scrub->pfss_flags & PFSS_TIMESTAMP)) ||
	    (dst->scrub && (dst->scrub->pfss_flags & PFSS_TIMESTAMP))) &&
	    pf_pull_hdr(pd->m, pd->off + sizeof(*th), opts, olen, NULL, NULL, pd->af)) {
		/* Modulate the timestamps.  Can be used for NAT detection, OS
		 * uptime determination or reboot detection.
		 */
		opt = opts;
		while ((opt = pf_find_tcpopt(opt, opts, olen,
		    TCPOPT_TIMESTAMP, TCPOLEN_TIMESTAMP)) != NULL) {
			uint8_t *ts = opt + 2;
			uint8_t *tsr = opt + 6;

			if (got_ts) {
				/* Huh?  Multiple timestamps!? */
				if (V_pf_status.debug >= PF_DEBUG_MISC) {
					printf("pf: %s: multiple TS??", __func__);
					pf_print_state(state);
					printf("\n");
				}
				REASON_SET(reason, PFRES_TS);
				return (PF_DROP);
			}

			memcpy(&tsval, ts, sizeof(u_int32_t));
			memcpy(&tsecr, tsr, sizeof(u_int32_t));

			/* modulate TS */
			if (tsval && src->scrub &&
			    (src->scrub->pfss_flags & PFSS_TIMESTAMP)) {
				/* tsval used further on */
				tsval = ntohl(tsval);
				pf_patch_32(pd,
				    ts, htonl(tsval + src->scrub->pfss_ts_mod),
				    PF_ALGNMNT(ts - opts));
				copyback = 1;
			}

			/* modulate TS reply if any (!0) */
			if (tsecr && dst->scrub &&
			    (dst->scrub->pfss_flags & PFSS_TIMESTAMP)) {
				/* tsecr used further on */
				tsecr = ntohl(tsecr) - dst->scrub->pfss_ts_mod;
				pf_patch_32(pd, tsr, htonl(tsecr),
				    PF_ALGNMNT(tsr - opts));
				copyback = 1;
			}

			got_ts = 1;
			opt += opt[1];
		}

		if (copyback) {
			/* Copyback the options, caller copys back header */
			*writeback = 1;
			m_copyback(pd->m, pd->off + sizeof(*th), olen, opts);
		}
	}

	/*
	 * Must invalidate PAWS checks on connections idle for too long.
	 * The fastest allowed timestamp clock is 1ms.  That turns out to
	 * be about 24 days before it wraps.  XXX Right now our lowerbound
	 * TS echo check only works for the first 12 days of a connection
	 * when the TS has exhausted half its 32bit space
	 */
#define TS_MAX_IDLE	(24*24*60*60)
#define TS_MAX_CONN	(12*24*60*60)	/* XXX remove when better tsecr check */

	getmicrouptime(&uptime);
	if (src->scrub && (src->scrub->pfss_flags & PFSS_PAWS) &&
	    (uptime.tv_sec - src->scrub->pfss_last.tv_sec > TS_MAX_IDLE ||
	    time_uptime - (state->creation / 1000) > TS_MAX_CONN))  {
		if (V_pf_status.debug >= PF_DEBUG_MISC) {
			DPFPRINTF(PF_DEBUG_MISC, "src idled out of PAWS");
			pf_print_state(state);
			printf("\n");
		}
		src->scrub->pfss_flags = (src->scrub->pfss_flags & ~PFSS_PAWS)
		    | PFSS_PAWS_IDLED;
	}
	if (dst->scrub && (dst->scrub->pfss_flags & PFSS_PAWS) &&
	    uptime.tv_sec - dst->scrub->pfss_last.tv_sec > TS_MAX_IDLE) {
		if (V_pf_status.debug >= PF_DEBUG_MISC) {
			DPFPRINTF(PF_DEBUG_MISC, "dst idled out of PAWS");
			pf_print_state(state);
			printf("\n");
		}
		dst->scrub->pfss_flags = (dst->scrub->pfss_flags & ~PFSS_PAWS)
		    | PFSS_PAWS_IDLED;
	}

	if (got_ts && src->scrub && dst->scrub &&
	    (src->scrub->pfss_flags & PFSS_PAWS) &&
	    (dst->scrub->pfss_flags & PFSS_PAWS)) {
		/* Validate that the timestamps are "in-window".
		 * RFC1323 describes TCP Timestamp options that allow
		 * measurement of RTT (round trip time) and PAWS
		 * (protection against wrapped sequence numbers).  PAWS
		 * gives us a set of rules for rejecting packets on
		 * long fat pipes (packets that were somehow delayed
		 * in transit longer than the time it took to send the
		 * full TCP sequence space of 4Gb).  We can use these
		 * rules and infer a few others that will let us treat
		 * the 32bit timestamp and the 32bit echoed timestamp
		 * as sequence numbers to prevent a blind attacker from
		 * inserting packets into a connection.
		 *
		 * RFC1323 tells us:
		 *  - The timestamp on this packet must be greater than
		 *    or equal to the last value echoed by the other
		 *    endpoint.  The RFC says those will be discarded
		 *    since it is a dup that has already been acked.
		 *    This gives us a lowerbound on the timestamp.
		 *        timestamp >= other last echoed timestamp
		 *  - The timestamp will be less than or equal to
		 *    the last timestamp plus the time between the
		 *    last packet and now.  The RFC defines the max
		 *    clock rate as 1ms.  We will allow clocks to be
		 *    up to 10% fast and will allow a total difference
		 *    or 30 seconds due to a route change.  And this
		 *    gives us an upperbound on the timestamp.
		 *        timestamp <= last timestamp + max ticks
		 *    We have to be careful here.  Windows will send an
		 *    initial timestamp of zero and then initialize it
		 *    to a random value after the 3whs; presumably to
		 *    avoid a DoS by having to call an expensive RNG
		 *    during a SYN flood.  Proof MS has at least one
		 *    good security geek.
		 *
		 *  - The TCP timestamp option must also echo the other
		 *    endpoints timestamp.  The timestamp echoed is the
		 *    one carried on the earliest unacknowledged segment
		 *    on the left edge of the sequence window.  The RFC
		 *    states that the host will reject any echoed
		 *    timestamps that were larger than any ever sent.
		 *    This gives us an upperbound on the TS echo.
		 *        tescr <= largest_tsval
		 *  - The lowerbound on the TS echo is a little more
		 *    tricky to determine.  The other endpoint's echoed
		 *    values will not decrease.  But there may be
		 *    network conditions that re-order packets and
		 *    cause our view of them to decrease.  For now the
		 *    only lowerbound we can safely determine is that
		 *    the TS echo will never be less than the original
		 *    TS.  XXX There is probably a better lowerbound.
		 *    Remove TS_MAX_CONN with better lowerbound check.
		 *        tescr >= other original TS
		 *
		 * It is also important to note that the fastest
		 * timestamp clock of 1ms will wrap its 32bit space in
		 * 24 days.  So we just disable TS checking after 24
		 * days of idle time.  We actually must use a 12d
		 * connection limit until we can come up with a better
		 * lowerbound to the TS echo check.
		 */
		struct timeval delta_ts;
		int ts_fudge;

		/*
		 * PFTM_TS_DIFF is how many seconds of leeway to allow
		 * a host's timestamp.  This can happen if the previous
		 * packet got delayed in transit for much longer than
		 * this packet.
		 */
		if ((ts_fudge = state->rule->timeout[PFTM_TS_DIFF]) == 0)
			ts_fudge = V_pf_default_rule.timeout[PFTM_TS_DIFF];

		/* Calculate max ticks since the last timestamp */
#define TS_MAXFREQ	1100		/* RFC max TS freq of 1Khz + 10% skew */
#define TS_MICROSECS	1000000		/* microseconds per second */
		delta_ts = uptime;
		timevalsub(&delta_ts, &src->scrub->pfss_last);
		tsval_from_last = (delta_ts.tv_sec + ts_fudge) * TS_MAXFREQ;
		tsval_from_last += delta_ts.tv_usec / (TS_MICROSECS/TS_MAXFREQ);

		if ((src->state >= TCPS_ESTABLISHED &&
		    dst->state >= TCPS_ESTABLISHED) &&
		    (SEQ_LT(tsval, dst->scrub->pfss_tsecr) ||
		    SEQ_GT(tsval, src->scrub->pfss_tsval + tsval_from_last) ||
		    (tsecr && (SEQ_GT(tsecr, dst->scrub->pfss_tsval) ||
		    SEQ_LT(tsecr, dst->scrub->pfss_tsval0))))) {
			/* Bad RFC1323 implementation or an insertion attack.
			 *
			 * - Solaris 2.6 and 2.7 are known to send another ACK
			 *   after the FIN,FIN|ACK,ACK closing that carries
			 *   an old timestamp.
			 */

			DPFPRINTF(PF_DEBUG_MISC, "Timestamp failed %c%c%c%c",
			    SEQ_LT(tsval, dst->scrub->pfss_tsecr) ? '0' : ' ',
			    SEQ_GT(tsval, src->scrub->pfss_tsval +
			    tsval_from_last) ? '1' : ' ',
			    SEQ_GT(tsecr, dst->scrub->pfss_tsval) ? '2' : ' ',
			    SEQ_LT(tsecr, dst->scrub->pfss_tsval0)? '3' : ' ');
			DPFPRINTF(PF_DEBUG_MISC, " tsval: %u  tsecr: %u  +ticks: "
			    "%u  idle: %jus %lums",
			    tsval, tsecr, tsval_from_last,
			    (uintmax_t)delta_ts.tv_sec,
			    delta_ts.tv_usec / 1000);
			DPFPRINTF(PF_DEBUG_MISC, " src->tsval: %u  tsecr: %u",
			    src->scrub->pfss_tsval, src->scrub->pfss_tsecr);
			DPFPRINTF(PF_DEBUG_MISC, " dst->tsval: %u  tsecr: %u  "
			    "tsval0: %u", dst->scrub->pfss_tsval,
			    dst->scrub->pfss_tsecr, dst->scrub->pfss_tsval0);
			if (V_pf_status.debug >= PF_DEBUG_MISC) {
				pf_print_state(state);
				pf_print_flags(tcp_get_flags(th));
				printf("\n");
			}
			REASON_SET(reason, PFRES_TS);
			return (PF_DROP);
		}

		/* XXX I'd really like to require tsecr but it's optional */

	} else if (!got_ts && (tcp_get_flags(th) & TH_RST) == 0 &&
	    ((src->state == TCPS_ESTABLISHED && dst->state == TCPS_ESTABLISHED)
	    || pd->p_len > 0 || (tcp_get_flags(th) & TH_SYN)) &&
	    src->scrub && dst->scrub &&
	    (src->scrub->pfss_flags & PFSS_PAWS) &&
	    (dst->scrub->pfss_flags & PFSS_PAWS)) {
		/* Didn't send a timestamp.  Timestamps aren't really useful
		 * when:
		 *  - connection opening or closing (often not even sent).
		 *    but we must not let an attacker to put a FIN on a
		 *    data packet to sneak it through our ESTABLISHED check.
		 *  - on a TCP reset.  RFC suggests not even looking at TS.
		 *  - on an empty ACK.  The TS will not be echoed so it will
		 *    probably not help keep the RTT calculation in sync and
		 *    there isn't as much danger when the sequence numbers
		 *    got wrapped.  So some stacks don't include TS on empty
		 *    ACKs :-(
		 *
		 * To minimize the disruption to mostly RFC1323 conformant
		 * stacks, we will only require timestamps on data packets.
		 *
		 * And what do ya know, we cannot require timestamps on data
		 * packets.  There appear to be devices that do legitimate
		 * TCP connection hijacking.  There are HTTP devices that allow
		 * a 3whs (with timestamps) and then buffer the HTTP request.
		 * If the intermediate device has the HTTP response cache, it
		 * will spoof the response but not bother timestamping its
		 * packets.  So we can look for the presence of a timestamp in
		 * the first data packet and if there, require it in all future
		 * packets.
		 */

		if (pd->p_len > 0 && (src->scrub->pfss_flags & PFSS_DATA_TS)) {
			/*
			 * Hey!  Someone tried to sneak a packet in.  Or the
			 * stack changed its RFC1323 behavior?!?!
			 */
			if (V_pf_status.debug >= PF_DEBUG_MISC) {
				DPFPRINTF(PF_DEBUG_MISC, "Did not receive expected "
				    "RFC1323 timestamp");
				pf_print_state(state);
				pf_print_flags(tcp_get_flags(th));
				printf("\n");
			}
			REASON_SET(reason, PFRES_TS);
			return (PF_DROP);
		}
	}

	/*
	 * We will note if a host sends his data packets with or without
	 * timestamps.  And require all data packets to contain a timestamp
	 * if the first does.  PAWS implicitly requires that all data packets be
	 * timestamped.  But I think there are middle-man devices that hijack
	 * TCP streams immediately after the 3whs and don't timestamp their
	 * packets (seen in a WWW accelerator or cache).
	 */
	if (pd->p_len > 0 && src->scrub && (src->scrub->pfss_flags &
	    (PFSS_TIMESTAMP|PFSS_DATA_TS|PFSS_DATA_NOTS)) == PFSS_TIMESTAMP) {
		if (got_ts)
			src->scrub->pfss_flags |= PFSS_DATA_TS;
		else {
			src->scrub->pfss_flags |= PFSS_DATA_NOTS;
			if (V_pf_status.debug >= PF_DEBUG_MISC && dst->scrub &&
			    (dst->scrub->pfss_flags & PFSS_TIMESTAMP)) {
				/* Don't warn if other host rejected RFC1323 */
				DPFPRINTF(PF_DEBUG_MISC, "Broken RFC1323 stack did "
				    "not timestamp data packet. Disabled PAWS "
				    "security.");
				pf_print_state(state);
				pf_print_flags(tcp_get_flags(th));
				printf("\n");
			}
		}
	}

	/*
	 * Update PAWS values
	 */
	if (got_ts && src->scrub && PFSS_TIMESTAMP == (src->scrub->pfss_flags &
	    (PFSS_PAWS_IDLED|PFSS_TIMESTAMP))) {
		getmicrouptime(&src->scrub->pfss_last);
		if (SEQ_GEQ(tsval, src->scrub->pfss_tsval) ||
		    (src->scrub->pfss_flags & PFSS_PAWS) == 0)
			src->scrub->pfss_tsval = tsval;

		if (tsecr) {
			if (SEQ_GEQ(tsecr, src->scrub->pfss_tsecr) ||
			    (src->scrub->pfss_flags & PFSS_PAWS) == 0)
				src->scrub->pfss_tsecr = tsecr;

			if ((src->scrub->pfss_flags & PFSS_PAWS) == 0 &&
			    (SEQ_LT(tsval, src->scrub->pfss_tsval0) ||
			    src->scrub->pfss_tsval0 == 0)) {
				/* tsval0 MUST be the lowest timestamp */
				src->scrub->pfss_tsval0 = tsval;
			}

			/* Only fully initialized after a TS gets echoed */
			if ((src->scrub->pfss_flags & PFSS_PAWS) == 0)
				src->scrub->pfss_flags |= PFSS_PAWS;
		}
	}

	/* I have a dream....  TCP segment reassembly.... */
	return (0);
}

int
pf_normalize_mss(struct pf_pdesc *pd)
{
	int		 olen, optsoff;
	uint8_t		 opts[MAX_TCPOPTLEN], *opt;

	olen = (pd->hdr.tcp.th_off << 2) - sizeof(struct tcphdr);
	optsoff = pd->off + sizeof(struct tcphdr);
	if (olen < TCPOLEN_MAXSEG ||
	    !pf_pull_hdr(pd->m, optsoff, opts, olen, NULL, NULL, pd->af))
		return (0);

	opt = opts;
	while ((opt = pf_find_tcpopt(opt, opts, olen,
	    TCPOPT_MAXSEG, TCPOLEN_MAXSEG)) != NULL) {
		uint16_t	 mss;
		uint8_t		*mssp = opt + 2;
		memcpy(&mss, mssp, sizeof(mss));
		if (ntohs(mss) > pd->act.max_mss) {
			size_t mssoffopts = mssp - opts;
			pf_patch_16(pd, &mss,
			    htons(pd->act.max_mss), PF_ALGNMNT(mssoffopts));
			m_copyback(pd->m, optsoff + mssoffopts,
			    sizeof(mss), (caddr_t)&mss);
			m_copyback(pd->m, pd->off,
			    sizeof(struct tcphdr), (caddr_t)&pd->hdr.tcp);
		}

		opt += opt[1];
	}

	return (0);
}

int
pf_scan_sctp(struct pf_pdesc *pd)
{
	struct sctp_chunkhdr ch = { };
	int chunk_off = sizeof(struct sctphdr);
	int chunk_start;
	int ret;

	while (pd->off + chunk_off < pd->tot_len) {
		if (!pf_pull_hdr(pd->m, pd->off + chunk_off, &ch, sizeof(ch), NULL,
		    NULL, pd->af))
			return (PF_DROP);

		/* Length includes the header, this must be at least 4. */
		if (ntohs(ch.chunk_length) < 4)
			return (PF_DROP);

		chunk_start = chunk_off;
		chunk_off += roundup(ntohs(ch.chunk_length), 4);

		switch (ch.chunk_type) {
		case SCTP_INITIATION:
		case SCTP_INITIATION_ACK: {
			struct sctp_init_chunk init;

			if (!pf_pull_hdr(pd->m, pd->off + chunk_start, &init,
			    sizeof(init), NULL, NULL, pd->af))
				return (PF_DROP);

			/*
			 * RFC 9620, Section 3.3.2, "The Initiate Tag is allowed to have
			 * any value except 0."
			 */
			if (init.init.initiate_tag == 0)
				return (PF_DROP);
			if (init.init.num_inbound_streams == 0)
				return (PF_DROP);
			if (init.init.num_outbound_streams == 0)
				return (PF_DROP);
			if (ntohl(init.init.a_rwnd) < SCTP_MIN_RWND)
				return (PF_DROP);

			/*
			 * RFC 9260, Section 3.1, INIT chunks MUST have zero
			 * verification tag.
			 */
			if (ch.chunk_type == SCTP_INITIATION &&
			    pd->hdr.sctp.v_tag != 0)
				return (PF_DROP);

			pd->sctp_initiate_tag = init.init.initiate_tag;

			if (ch.chunk_type == SCTP_INITIATION)
				pd->sctp_flags |= PFDESC_SCTP_INIT;
			else
				pd->sctp_flags |= PFDESC_SCTP_INIT_ACK;

			ret = pf_multihome_scan_init(pd->off + chunk_start,
			    ntohs(init.ch.chunk_length), pd);
			if (ret != PF_PASS)
				return (ret);

			break;
		}
		case SCTP_ABORT_ASSOCIATION:
			pd->sctp_flags |= PFDESC_SCTP_ABORT;
			break;
		case SCTP_SHUTDOWN:
		case SCTP_SHUTDOWN_ACK:
			pd->sctp_flags |= PFDESC_SCTP_SHUTDOWN;
			break;
		case SCTP_SHUTDOWN_COMPLETE:
			pd->sctp_flags |= PFDESC_SCTP_SHUTDOWN_COMPLETE;
			break;
		case SCTP_COOKIE_ECHO:
			pd->sctp_flags |= PFDESC_SCTP_COOKIE;
			break;
		case SCTP_COOKIE_ACK:
			pd->sctp_flags |= PFDESC_SCTP_COOKIE_ACK;
			break;
		case SCTP_DATA:
			pd->sctp_flags |= PFDESC_SCTP_DATA;
			break;
		case SCTP_HEARTBEAT_REQUEST:
			pd->sctp_flags |= PFDESC_SCTP_HEARTBEAT;
			break;
		case SCTP_HEARTBEAT_ACK:
			pd->sctp_flags |= PFDESC_SCTP_HEARTBEAT_ACK;
			break;
		case SCTP_ASCONF:
			pd->sctp_flags |= PFDESC_SCTP_ASCONF;

			ret = pf_multihome_scan_asconf(pd->off + chunk_start,
			    ntohs(ch.chunk_length), pd);
			if (ret != PF_PASS)
				return (ret);
			break;
		default:
			pd->sctp_flags |= PFDESC_SCTP_OTHER;
			break;
		}
	}

	/* Validate chunk lengths vs. packet length. */
	if (pd->off + chunk_off != pd->tot_len)
		return (PF_DROP);

	/*
	 * INIT, INIT_ACK or SHUTDOWN_COMPLETE chunks must always be the only
	 * one in a packet.
	 */
	if ((pd->sctp_flags & PFDESC_SCTP_INIT) &&
	    (pd->sctp_flags & ~PFDESC_SCTP_INIT))
		return (PF_DROP);
	if ((pd->sctp_flags & PFDESC_SCTP_INIT_ACK) &&
	    (pd->sctp_flags & ~PFDESC_SCTP_INIT_ACK))
		return (PF_DROP);
	if ((pd->sctp_flags & PFDESC_SCTP_SHUTDOWN_COMPLETE) &&
	    (pd->sctp_flags & ~PFDESC_SCTP_SHUTDOWN_COMPLETE))
		return (PF_DROP);
	if ((pd->sctp_flags & PFDESC_SCTP_ABORT) &&
	    (pd->sctp_flags & PFDESC_SCTP_DATA)) {
		/*
		 * RFC4960 3.3.7: DATA chunks MUST NOT be
		 * bundled with ABORT.
		 */
		return (PF_DROP);
	}

	return (PF_PASS);
}

int
pf_normalize_sctp(struct pf_pdesc *pd)
{
	struct pf_krule	*r, *rm = NULL;
	struct sctphdr	*sh = &pd->hdr.sctp;
	u_short		 reason;
	sa_family_t	 af = pd->af;
	int		 srs;

	PF_RULES_RASSERT();

	r = TAILQ_FIRST(pf_main_ruleset.rules[PF_RULESET_SCRUB].active.ptr);
	/* Check if there any scrub rules. Lack of scrub rules means enforced
	 * packet normalization operation just like in OpenBSD. */
	srs = (r != NULL);
	while (r != NULL) {
		pf_counter_u64_add(&r->evaluations, 1);
		if (pfi_kkif_match(r->kif, pd->kif) == r->ifnot)
			r = r->skip[PF_SKIP_IFP];
		else if (r->direction && r->direction != pd->dir)
			r = r->skip[PF_SKIP_DIR];
		else if (r->af && r->af != af)
			r = r->skip[PF_SKIP_AF];
		else if (r->proto && r->proto != pd->proto)
			r = r->skip[PF_SKIP_PROTO];
		else if (PF_MISMATCHAW(&r->src.addr, pd->src, af,
		    r->src.neg, pd->kif, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_SRC_ADDR];
		else if (r->src.port_op && !pf_match_port(r->src.port_op,
			    r->src.port[0], r->src.port[1], sh->src_port))
			r = r->skip[PF_SKIP_SRC_PORT];
		else if (PF_MISMATCHAW(&r->dst.addr, pd->dst, af,
		    r->dst.neg, NULL, M_GETFIB(pd->m)))
			r = r->skip[PF_SKIP_DST_ADDR];
		else if (r->dst.port_op && !pf_match_port(r->dst.port_op,
			    r->dst.port[0], r->dst.port[1], sh->dest_port))
			r = r->skip[PF_SKIP_DST_PORT];
		else {
			rm = r;
			break;
		}
	}

	if (srs) {
		/* With scrub rules present SCTP normalization happens only
		 * if one of rules has matched and it's not a "no scrub" rule */
		if (rm == NULL || rm->action == PF_NOSCRUB)
			return (PF_PASS);

		pf_counter_u64_critical_enter();
		pf_counter_u64_add_protected(&r->packets[pd->dir == PF_OUT], 1);
		pf_counter_u64_add_protected(&r->bytes[pd->dir == PF_OUT], pd->tot_len);
		pf_counter_u64_critical_exit();
	}

	/* Verify we're a multiple of 4 bytes long */
	if ((pd->tot_len - pd->off - sizeof(struct sctphdr)) % 4)
		goto sctp_drop;

	/* INIT chunk needs to be the only chunk */
	if (pd->sctp_flags & PFDESC_SCTP_INIT)
		if (pd->sctp_flags & ~PFDESC_SCTP_INIT)
			goto sctp_drop;

	return (PF_PASS);

sctp_drop:
	REASON_SET(&reason, PFRES_NORM);
	if (rm != NULL && r->log)
		PFLOG_PACKET(PF_DROP, reason, r, NULL, NULL, pd,
		    1, NULL);

	return (PF_DROP);
}

#if defined(INET) || defined(INET6)
void
pf_scrub(struct pf_pdesc *pd)
{

	struct ip		*h = mtod(pd->m, struct ip *);
#ifdef INET6
	struct ip6_hdr		*h6 = mtod(pd->m, struct ip6_hdr *);
#endif /* INET6 */

	/* Clear IP_DF if no-df was requested */
	if (pd->af == AF_INET && pd->act.flags & PFSTATE_NODF &&
	    h->ip_off & htons(IP_DF))
	{
		u_int16_t ip_off = h->ip_off;

		h->ip_off &= htons(~IP_DF);
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_off, h->ip_off, 0);
	}

	/* Enforce a minimum ttl, may cause endless packet loops */
	if (pd->af == AF_INET && pd->act.min_ttl &&
	    h->ip_ttl < pd->act.min_ttl) {
		u_int16_t ip_ttl = h->ip_ttl;

		h->ip_ttl = pd->act.min_ttl;
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_ttl, h->ip_ttl, 0);
	}
#ifdef INET6
	/* Enforce a minimum ttl, may cause endless packet loops */
	if (pd->af == AF_INET6 && pd->act.min_ttl &&
	    h6->ip6_hlim < pd->act.min_ttl)
		h6->ip6_hlim = pd->act.min_ttl;
#endif /* INET6 */
	/* Enforce tos */
	if (pd->act.flags & PFSTATE_SETTOS) {
		switch (pd->af) {
		case AF_INET: {
			u_int16_t	ov, nv;

			ov = *(u_int16_t *)h;
			h->ip_tos = pd->act.set_tos | (h->ip_tos & IPTOS_ECN_MASK);
			nv = *(u_int16_t *)h;

			h->ip_sum = pf_cksum_fixup(h->ip_sum, ov, nv, 0);
			break;
		}
#ifdef INET6
		case AF_INET6:
			h6->ip6_flow &= IPV6_FLOWLABEL_MASK | IPV6_VERSION_MASK;
			h6->ip6_flow |= htonl((pd->act.set_tos | IPV6_ECN(h6)) << 20);
			break;
#endif /* INET6 */
		}
	}

	/* random-id, but not for fragments */
#ifdef INET
	if (pd->af == AF_INET &&
	    pd->act.flags & PFSTATE_RANDOMID && !(h->ip_off & ~htons(IP_DF))) {
		uint16_t ip_id = h->ip_id;

		ip_fillid(h, V_ip_random_id);
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_id, h->ip_id, 0);
	}
#endif /* INET */
}
#endif /* INET || INET6 */
