#ifndef __NET_IP_TUNNELS_H
#define __NET_IP_TUNNELS_H 1

#include <linux/if_tunnel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/u64_stats_sync.h>
#include <linux/bitops.h>

#include <net/dsfield.h>
#include <net/gro_cells.h>
#include <net/inet_ecn.h>
#include <net/ip.h>
#include <net/rtnetlink.h>

#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6.h>
#include <net/ip6_fib.h>
#include <net/ip6_route.h>
#endif

/* Keep error state on tunnel for 30 sec */
#define IPTUNNEL_ERR_TIMEO	(30*HZ)

/* Used to memset ip_tunnel padding. */
#define IP_TUNNEL_KEY_SIZE					\
	(offsetof(struct ip_tunnel_key, tp_dst) +		\
	 FIELD_SIZEOF(struct ip_tunnel_key, tp_dst))

struct ip_tunnel_key {
	__be64			tun_id;
	__be32			ipv4_src;
	__be32			ipv4_dst;
	__be16			tun_flags;
	__u8			ipv4_tos;
	__u8			ipv4_ttl;
	__be16			tp_src;
	__be16			tp_dst;
} __packed __aligned(4); /* Minimize padding. */

/* Indicates whether the tunnel info structure represents receive
 * or transmit tunnel parameters.
 */
enum {
	IP_TUNNEL_INFO_RX,
	IP_TUNNEL_INFO_TX,
};

/* Maximum tunnel options length. */
#define IP_TUNNEL_OPTS_MAX					\
	GENMASK((FIELD_SIZEOF(struct ip_tunnel_info,		\
			      options_len) * BITS_PER_BYTE) - 1, 0)

struct ip_tunnel_info {
	struct ip_tunnel_key	key;
	const void		*options;
	u8			options_len;
	u8			mode;
};

/* 6rd prefix/relay information */
#ifdef CONFIG_IPV6_SIT_6RD
struct ip_tunnel_6rd_parm {
	struct in6_addr		prefix;
	__be32			relay_prefix;
	u16			prefixlen;
	u16			relay_prefixlen;
};
#endif

struct ip_tunnel_prl_entry {
	struct ip_tunnel_prl_entry __rcu *next;
	__be32				addr;
	u16				flags;
	struct rcu_head			rcu_head;
};

struct ip_tunnel {
	struct ip_tunnel __rcu	*next;
	struct hlist_node hash_node;
	struct net_device	*dev;

	int		err_count;	/* Number of arrived ICMP errors */
	unsigned long	err_time;	/* Time when the last ICMP error
					 * arrived */

	/* These four fields used only by GRE */
	__u32		i_seqno;	/* The last seen seqno	*/
	__u32		o_seqno;	/* The last output seqno */
	int		hlen;		/* Precalculated header length */
	int		mlink;

	struct ip_tunnel_parm parms;

	/* for SIT */
#ifdef CONFIG_IPV6_SIT_6RD
	struct ip_tunnel_6rd_parm ip6rd;
#endif
	struct ip_tunnel_prl_entry __rcu *prl;	/* potential router list */
	unsigned int		prl_count;	/* # of entries in PRL */
	int			ip_tnl_net_id;
	struct gro_cells	gro_cells;
};

#define TUNNEL_CSUM	__cpu_to_be16(0x01)
#define TUNNEL_ROUTING	__cpu_to_be16(0x02)
#define TUNNEL_KEY	__cpu_to_be16(0x04)
#define TUNNEL_SEQ	__cpu_to_be16(0x08)
#define TUNNEL_STRICT	__cpu_to_be16(0x10)
#define TUNNEL_REC	__cpu_to_be16(0x20)
#define TUNNEL_VERSION	__cpu_to_be16(0x40)
#define TUNNEL_NO_KEY	__cpu_to_be16(0x80)

struct tnl_ptk_info {
	__be16 flags;
	__be16 proto;
	__be32 key;
	__be32 seq;
};

#define PACKET_RCVD	0
#define PACKET_REJECT	1

#define IP_TNL_HASH_BITS   10
#define IP_TNL_HASH_SIZE   (1 << IP_TNL_HASH_BITS)

struct ip_tunnel_net {
	struct hlist_head *tunnels;
	struct net_device *fb_tunnel_dev;
};

static inline void __ip_tunnel_info_init(struct ip_tunnel_info *tun_info,
					 __be32 saddr, __be32 daddr,
					 u8 tos, u8 ttl,
					 __be16 tp_src, __be16 tp_dst,
					 __be64 tun_id, __be16 tun_flags,
					 const void *opts, u8 opts_len)
{
	tun_info->key.tun_id = tun_id;
	tun_info->key.ipv4_src = saddr;
	tun_info->key.ipv4_dst = daddr;
	tun_info->key.ipv4_tos = tos;
	tun_info->key.ipv4_ttl = ttl;
	tun_info->key.tun_flags = tun_flags;

	/* For the tunnel types on the top of IPsec, the tp_src and tp_dst of
	 * the upper tunnel are used.
	 * E.g: GRE over IPSEC, the tp_src and tp_port are zero.
	 */
	tun_info->key.tp_src = tp_src;
	tun_info->key.tp_dst = tp_dst;

	/* Clear struct padding. */
	if (sizeof(tun_info->key) != IP_TUNNEL_KEY_SIZE)
		memset((unsigned char *)&tun_info->key + IP_TUNNEL_KEY_SIZE,
		       0, sizeof(tun_info->key) - IP_TUNNEL_KEY_SIZE);

	tun_info->options = opts;
	tun_info->options_len = opts_len;
}

static inline void ip_tunnel_info_init(struct ip_tunnel_info *tun_info,
				       const struct iphdr *iph,
				       __be16 tp_src, __be16 tp_dst,
				       __be64 tun_id, __be16 tun_flags,
				       const void *opts, u8 opts_len)
{
	__ip_tunnel_info_init(tun_info, iph->saddr, iph->daddr,
			      iph->tos, iph->ttl, tp_src, tp_dst,
			      tun_id, tun_flags, opts, opts_len);
}

int ip_tunnel_init(struct net_device *dev);
void ip_tunnel_uninit(struct net_device *dev);
void  ip_tunnel_dellink(struct net_device *dev, struct list_head *head);
int ip_tunnel_init_net(struct net *net, int ip_tnl_net_id,
		       struct rtnl_link_ops *ops, char *devname);

void ip_tunnel_delete_net(struct ip_tunnel_net *itn);

void ip_tunnel_xmit(struct sk_buff *skb, struct net_device *dev,
		    const struct iphdr *tnl_params);
int ip_tunnel_ioctl(struct net_device *dev, struct ip_tunnel_parm *p, int cmd);
int ip_tunnel_change_mtu(struct net_device *dev, int new_mtu);

struct rtnl_link_stats64 *ip_tunnel_get_stats64(struct net_device *dev,
						struct rtnl_link_stats64 *tot);
struct ip_tunnel *ip_tunnel_lookup(struct ip_tunnel_net *itn,
				   int link, __be16 flags,
				   __be32 remote, __be32 local,
				   __be32 key);

int ip_tunnel_rcv(struct ip_tunnel *tunnel, struct sk_buff *skb,
		  const struct tnl_ptk_info *tpi, int hdr_len, bool log_ecn_error);
int ip_tunnel_changelink(struct net_device *dev, struct nlattr *tb[],
			 struct ip_tunnel_parm *p);
int ip_tunnel_newlink(struct net_device *dev, struct nlattr *tb[],
		      struct ip_tunnel_parm *p);
void ip_tunnel_setup(struct net_device *dev, int net_id);

/* Extract dsfield from inner protocol */
static inline u8 ip_tunnel_get_dsfield(const struct iphdr *iph,
				       const struct sk_buff *skb)
{
	if (skb->protocol == htons(ETH_P_IP))
		return iph->tos;
	else if (skb->protocol == htons(ETH_P_IPV6))
		return ipv6_get_dsfield((const struct ipv6hdr *)iph);
	else
		return 0;
}

/* Propogate ECN bits out */
static inline u8 ip_tunnel_ecn_encap(u8 tos, const struct iphdr *iph,
				     const struct sk_buff *skb)
{
	u8 inner = ip_tunnel_get_dsfield(iph, skb);

	return INET_ECN_encapsulate(tos, inner);
}

static inline void iptunnel_xmit(struct sk_buff *skb, struct net_device *dev)
{
	int err;
	int pkt_len = skb->len - skb_transport_offset(skb);
	struct pcpu_tstats *tstats = this_cpu_ptr(dev->tstats);

	nf_reset(skb);

	err = ip_local_out(skb);
	if (likely(net_xmit_eval(err) == 0)) {
		u64_stats_update_begin(&tstats->syncp);
		tstats->tx_bytes += pkt_len;
		tstats->tx_packets++;
		u64_stats_update_end(&tstats->syncp);
	} else {
		dev->stats.tx_errors++;
		dev->stats.tx_aborted_errors++;
	}
}
#endif /* __NET_IP_TUNNELS_H */
