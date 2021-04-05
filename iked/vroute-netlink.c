/*	$OpenBSD$	*/

/*
 * Copyright (c) 2021 Tobias Heider <tobhe@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>

#include <linux/ipv6.h>

#include <event.h>
#include <string.h>

#include <iked.h>

int vroute_setroute(struct iked *, uint8_t, struct sockaddr *, uint8_t,
    struct sockaddr *, int);
int vroute_doroute(struct iked *, uint8_t, struct sockaddr *,
    struct sockaddr *, struct sockaddr *, int);
int vroute_doaddr(struct iked *, int, struct sockaddr *, struct sockaddr *, int);

static int	nl_addattr(struct nlmsghdr *, int, void *, size_t);
static int	nl_dorule(struct iked *, int, uint32_t, int, int);

struct iked_vroute_sc {
	int	ivr_rtsock;
};

#define NL_BUFLEN	1024
#define IKED_RT_TABLE	210
#define IKED_RT_PRIO	210

void
vroute_init(struct iked *env)
{
	struct iked_vroute_sc	*ivr;

	ivr = calloc(1, sizeof(*ivr));
	if (ivr == NULL)
		fatal("%s: calloc.", __func__);

	if ((ivr->ivr_rtsock = socket(AF_NETLINK, SOCK_DGRAM,
	    NETLINK_ROUTE)) == -1)
		fatal("%s: failed to create netlink socket", __func__);

	env->sc_vroute = ivr;
	nl_dorule(env, IKED_RT_TABLE, IKED_RT_PRIO, AF_INET, 1);
	nl_dorule(env, IKED_RT_TABLE, IKED_RT_PRIO, AF_INET6, 1);
}

int
vroute_getaddr(struct iked *env, struct imsg *imsg)
{
	struct sockaddr		*addr, *mask;
	uint8_t			*ptr;
	size_t			 left;
	int			 af;
	unsigned int		 ifidx;

	ptr = imsg->data;
	left = IMSG_DATA_SIZE(imsg);

	if (left < sizeof(*addr))
		fatalx("bad length imsg received");

	addr = (struct sockaddr *) ptr;
	af = addr->sa_family;

	if (left < SA_LEN(addr))
		fatalx("bad length imsg received");
	ptr += SA_LEN(addr);
	left -= SA_LEN(addr);

	if (left < sizeof(*mask))
		fatalx("bad length imsg received");
	mask = (struct sockaddr *) ptr;
	if (mask->sa_family != af)
		return (-1);

	if (left < SA_LEN(mask))
		fatalx("bad length imsg received");
	ptr += SA_LEN(mask);
	left -= SA_LEN(mask);

	if (left != sizeof(ifidx))
		fatalx("bad length imsg received");
	memcpy(&ifidx, ptr, sizeof(ifidx));
	ptr += sizeof(ifidx);
	left -= sizeof(ifidx);

	return (vroute_doaddr(env, ifidx, addr, mask,
	    imsg->hdr.type == IMSG_IF_ADDADDR));
}

int
vroute_setaddroute(struct iked *env, uint8_t rdomain, struct sockaddr *dst,
    uint8_t mask, struct sockaddr *ifa)
{
	return (vroute_setroute(env, rdomain, dst, mask, ifa,
	    IMSG_VROUTE_ADD));
}

int
vroute_setcloneroute(struct iked *env, uint8_t rdomain, struct sockaddr *dst,
    uint8_t mask, struct sockaddr *addr)
{
	return (vroute_setroute(env, rdomain, dst, mask, addr,
	    IMSG_VROUTE_CLONE));
}

int
vroute_setdelroute(struct iked *env, uint8_t rdomain, struct sockaddr *dst,
    uint8_t mask, struct sockaddr *addr)
{
	return (vroute_setroute(env, rdomain, dst, mask, addr,
	    IMSG_VROUTE_DEL));
}

int
vroute_setroute(struct iked *env, uint8_t rdomain, struct sockaddr *dst,
    uint8_t mask, struct sockaddr *addr, int type)
{
	struct sockaddr_storage	 sa;
	struct sockaddr_in	*in;
	struct sockaddr_in6	*in6;
	struct iovec		 iov[5];
	int			 iovcnt = 0;
	uint8_t			 af;

	if (addr && dst->sa_family != addr->sa_family)
		return (-1);
	af = dst->sa_family;

	iov[iovcnt].iov_base = &rdomain;
	iov[iovcnt].iov_len = sizeof(rdomain);
	iovcnt++;

	iov[iovcnt].iov_base = dst;
	iov[iovcnt].iov_len = SA_LEN(dst);
	iovcnt++;

	if (type != IMSG_VROUTE_CLONE && addr) {
		bzero(&sa, sizeof(sa));
		switch(af) {
		case AF_INET:
			in = (struct sockaddr_in *)&sa;
			in->sin_addr.s_addr = prefixlen2mask(mask);
			in->sin_family = af;
			iov[iovcnt].iov_base = in;
			iov[iovcnt].iov_len = sizeof(*in);
			iovcnt++;
			break;
		case AF_INET6:
			in6 = (struct sockaddr_in6 *)&sa;
			prefixlen2mask6(mask,
			    (uint32_t *)in6->sin6_addr.s6_addr);
			in6->sin6_family = af;
			iov[iovcnt].iov_base = in6;
			iov[iovcnt].iov_len = sizeof(*in6);
			iovcnt++;
			break;
		}

		iov[iovcnt].iov_base = addr;
		iov[iovcnt].iov_len = SA_LEN(addr);
		iovcnt++;
	}

	return (proc_composev(&env->sc_ps, PROC_PARENT, type, iov, iovcnt));
}

int
vroute_getroute(struct iked *env, struct imsg *imsg)
{
	struct sockaddr		*dest, *mask = NULL, *gateway = NULL;
	uint8_t			*ptr;
	size_t			 left;
	int			 type = 0;
	uint8_t			 rdomain;

	ptr = (uint8_t *)imsg->data;
	left = IMSG_DATA_SIZE(imsg);

	if (left < sizeof(rdomain))
		return (-1);
	rdomain = *ptr;
	ptr += sizeof(rdomain);
	left -= sizeof(rdomain);

	if (left < sizeof(struct sockaddr))
		return (-1);
	dest = (struct sockaddr *)ptr;

	if (left < SA_LEN(dest))
		return (-1);
	socket_setport(dest, 0);
	ptr += SA_LEN(dest);
	left -= SA_LEN(dest);

	if (left != 0) {
		if (left < sizeof(*mask))
			return (-1);
		mask = (struct sockaddr *)ptr;
		if (left < SA_LEN(mask))
			return (-1);
		socket_setport(mask, 0);
		ptr += SA_LEN(mask);
		left -= SA_LEN(mask);

		if (left < sizeof(*gateway))
			return (-1);
		gateway = (struct sockaddr *)ptr;
		if (left < SA_LEN(gateway))
			return (-1);
		socket_setport(gateway, 0);
		ptr += SA_LEN(gateway);
		left -= SA_LEN(gateway);
	}

	switch(imsg->hdr.type) {
	case IMSG_VROUTE_ADD:
		type = RTM_NEWROUTE;
		break;
	case IMSG_VROUTE_DEL:
		type = RTM_DELROUTE;
		break;
	}

	return (vroute_doroute(env, type, dest, mask, gateway, 0));
}

int
vroute_getcloneroute(struct iked *env, struct imsg *imsg)
{
	struct sockaddr		*dst;
	struct sockaddr_storage	 mask;
	uint8_t			*ptr;
	size_t			 left;
	uint8_t			 rdomain;

	ptr = (uint8_t *)imsg->data;
	left = IMSG_DATA_SIZE(imsg);

	if (left < sizeof(rdomain))
		return (-1);
	rdomain = *ptr;
	ptr += sizeof(rdomain);
	left -= sizeof(rdomain);

	bzero(&mask, sizeof(mask));

	if (left < sizeof(struct sockaddr))
		return (-1);
	dst = (struct sockaddr *)ptr;
	if (left < SA_LEN(dst))
		return (-1);
	ptr += SA_LEN(dst);
	left -= SA_LEN(dst);

	/*
	 * With rtnetlink(7) the host route is not cloned. Instead, we
	 * add a RTN_THROW route for the host in the IKED routing table.
	 */
	return (vroute_doroute(env, RTM_NEWROUTE, dst, NULL, NULL, 1));
}

int
vroute_doroute(struct iked *env, uint8_t type, struct sockaddr *dest,
    struct sockaddr *mask, struct sockaddr *addr, int host)
{
	char			 dest_buf[NI_MAXHOST];
	char			 addr_buf[NI_MAXHOST];
	struct iked_vroute_sc	*ivr = env->sc_vroute;
	struct sockaddr_in	*in;
	struct sockaddr_in6	*in6;
	int			 af;
	struct {
		struct nlmsghdr		hdr;
		char			buf[NL_BUFLEN];
	} req;
	struct rtmsg		*rtm;

	if (dest == NULL)
		return (-1);

	bzero(&req, sizeof(req));

	af = dest->sa_family;
	req.hdr.nlmsg_type = type;
	req.hdr.nlmsg_flags = NLM_F_REQUEST;
	if (type == RTM_NEWROUTE)
		req.hdr.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
	req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));

	rtm = NLMSG_DATA(&req.hdr);
	rtm->rtm_family = af;
	rtm->rtm_protocol = RTPROT_STATIC;
	if (host)
		rtm->rtm_type = RTN_THROW;
	else
		rtm->rtm_type = RTN_UNICAST;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_table = IKED_RT_TABLE;

	bzero(addr_buf, sizeof(addr_buf));

	switch (af) {
	case AF_INET:
		rtm->rtm_dst_len = mask ? mask2prefixlen(mask) : 32;

		in = (struct sockaddr_in *) dest;
		nl_addattr(&req.hdr, RTA_DST, &in->sin_addr.s_addr, 4);
		inet_ntop(af, &((struct sockaddr_in *)dest)->sin_addr,
		    dest_buf, sizeof(dest_buf));

		if (type == RTM_DELROUTE)
			goto send;

		if (addr) {
			inet_ntop(af, &((struct sockaddr_in *)addr)->sin_addr,
			    addr_buf, sizeof(addr_buf));
			in = (struct sockaddr_in *) addr;
			nl_addattr(&req.hdr, RTA_GATEWAY, &in->sin_addr.s_addr, 4);
		}
		break;
	case AF_INET6:
		rtm->rtm_dst_len = mask ? mask2prefixlen6(mask) : 128;

		in6 = (struct sockaddr_in6 *) dest;
		nl_addattr(&req.hdr, RTA_DST, &in6->sin6_addr.s6_addr, 16);
		inet_ntop(af, &((struct sockaddr_in6 *)dest)->sin6_addr,
		    dest_buf, sizeof(dest_buf));

		if (type == RTM_DELROUTE)
			goto send;

		if (addr) {
			inet_ntop(af, &((struct sockaddr_in6 *)addr)->sin6_addr,
			    addr_buf, sizeof(addr_buf));
			in6 = (struct sockaddr_in6 *) dest;
			nl_addattr(&req.hdr, RTA_GATEWAY,
			    &in6->sin6_addr.s6_addr, 16);
		}
		break;
	default:
		return (-1);
	}
send:
	log_debug("%s: len: %u type: %s dst %s/%d gateway %s flags %x", __func__, req.hdr.nlmsg_len,
	    type == RTM_NEWROUTE ? "RTM_NEWROUTE" : type == RTM_DELROUTE ? "RTM_DELROUTE" :
	    type == RTM_GETROUTE ? "RTM_GETROUTE" : "unknown", dest_buf, rtm->rtm_dst_len, addr_buf,
	    req.hdr.nlmsg_flags);

	if (send(ivr->ivr_rtsock, &req, req.hdr.nlmsg_len, 0) == -1)
		log_warn("%s: send", __func__);

	return (0);
}

int
vroute_doaddr(struct iked *env, int ifidx, struct sockaddr *addr,
    struct sockaddr *mask, int add)
{
	struct iked_vroute_sc	*ivr = env->sc_vroute;
	struct sockaddr_in	*in;
	struct sockaddr_in6	*in6;
	int			 af;
	char			 addr_buf[NI_MAXHOST];
	char			 mask_buf[NI_MAXHOST];
	struct {
		struct nlmsghdr		hdr;
		struct ifaddrmsg	ifa;
		uint8_t			buf[NL_BUFLEN];
	} req;

	bzero(&req, sizeof(req));

	af = addr->sa_family;
	req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.hdr.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
	req.hdr.nlmsg_flags = NLM_F_REQUEST;
	if (add)
		req.hdr.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
	req.ifa.ifa_family = af;
	req.ifa.ifa_scope = RT_SCOPE_UNIVERSE;
	req.ifa.ifa_index = ifidx;

	switch(af) {
	case AF_INET:
		inet_ntop(af, &((struct sockaddr_in *)addr)->sin_addr,
		    addr_buf, sizeof(addr_buf));
		inet_ntop(af, &((struct sockaddr_in *)mask)->sin_addr,
		    mask_buf, sizeof(mask_buf));
		log_debug("%s: %s inet %s netmask %s", __func__,
		    add ? "add" : "del",addr_buf, mask_buf);

		req.ifa.ifa_prefixlen = mask2prefixlen(mask);
		in = (struct sockaddr_in *) addr;
		nl_addattr(&req.hdr, IFA_LOCAL, &in->sin_addr.s_addr, 4);
		break;
	case AF_INET6:
		inet_ntop(af, &((struct sockaddr_in6 *)addr)->sin6_addr,
		    addr_buf, sizeof(addr_buf));
		inet_ntop(af, &((struct sockaddr_in6 *)mask)->sin6_addr,
		    mask_buf, sizeof(mask_buf));
		log_debug("%s: %s inet6 %s netmask %s", __func__,
		    add ? "add" : "del",addr_buf, mask_buf);

		req.ifa.ifa_prefixlen = mask2prefixlen6(mask);
		in6 = (struct sockaddr_in6 *) addr;
		nl_addattr(&req.hdr, IFA_LOCAL, &in6->sin6_addr.s6_addr, 16);
		break;
	}

	if (send(ivr->ivr_rtsock, &req, req.hdr.nlmsg_len, 0) == -1)
		log_warn("%s: send", __func__);

	return (0);
}

static int
nl_dorule(struct iked *env, int table, uint32_t prio, int family, int add)
{
	struct iked_vroute_sc	*ivr = env->sc_vroute;
	struct {
		struct nlmsghdr		hdr;
		uint8_t			buf[NL_BUFLEN];
	} req;
	struct rtmsg		*rtm;

	bzero(&req, sizeof(req));

	req.hdr.nlmsg_flags = NLM_F_REQUEST;
	req.hdr.nlmsg_type = add ? RTM_NEWRULE : RTM_DELRULE;
	req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	if (add)
		req.hdr.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;

	rtm = NLMSG_DATA(&req.hdr);
	rtm->rtm_family = family;
	rtm->rtm_protocol = RTPROT_BOOT;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_type = RTN_UNICAST;
	rtm->rtm_table = IKED_RT_TABLE;

	nl_addattr(&req.hdr, RTA_PRIORITY, &prio, sizeof(prio));

	if (send(ivr->ivr_rtsock, &req, req.hdr.nlmsg_len, 0) == -1)
		log_warn("%s: send", __func__);

	return (0);
}

static int
nl_addattr(struct nlmsghdr *hdr, int type, void *data, size_t len)
{
	struct rtattr	*rta;

	if ((NLMSG_ALIGN(hdr->nlmsg_len) + RTA_LENGTH(len)) > NL_BUFLEN) {
		log_info("%s: buffer too small.", __func__);
		return (-1);
	}

	rta = (struct rtattr *)(((char *)hdr) +
	    NLMSG_ALIGN(hdr->nlmsg_len));
	rta->rta_type = type;
	rta->rta_len = RTA_LENGTH(len);
	memcpy(RTA_DATA(rta), data, len);
	hdr->nlmsg_len = NLMSG_ALIGN(hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
	return (0);
}
