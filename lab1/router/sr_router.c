/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

/*---------------------------------------------------------------------
	* Method: sr_init(void)
	* Scope:  Global
	*
	* Initialize the routing subsystem
	*
	*---------------------------------------------------------------------*/

void sr_init(struct sr_instance *sr)
{
	/* REQUIRES */
	assert(sr);

	/* Initialize cache and cache cleanup thread */
	sr_arpcache_init(&(sr->cache));

	pthread_attr_init(&(sr->attr));
	pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
	pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
	pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
	pthread_t thread;

	pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);

	/* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
	* Method: sr_handlepacket(uint8_t* p,char* interface)
	* Scope:  Global
	*
	* This method is called each time the router receives a packet on the
	* interface.  The packet buffer, the packet length and the receiving
	* interface are passed in as parameters. The packet is complete with
	* ethernet headers.
	*
	* Note: Both the packet buffer and the character's memory are handled
	* by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
	* packet instead if you intend to keep it around beyond the scope of
	* the method call.
	*
	*---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance *sr,
					 uint8_t *packet /* lent */,
					 unsigned int len,
					 char *interface /* lent */)
{
	/* REQUIRES */
	assert(sr);
	assert(packet);
	assert(interface);

	printf("*** -> Received packet of length %d \n", len);

	/* fill in code here */

	if (len < sizeof(sr_ethernet_hdr_t))
	{
		fprintf(stderr, "** Error: packet is wayy to short for ethernet header \n");
		return;
	}

	/* it is an IP packet */
	if (ethertype(packet) == ethertype_ip)
	{
		sr_handle_ip(sr, packet, len, interface);
	}
	/* it is a ARP packet */
	else if (ethertype(packet) == ethertype_arp)
	{
		sr_handle_arp(sr, packet, len, interface);
	}

} /* end sr_handlepacket*/

void sr_handle_ip(struct sr_instance *sr,
				  uint8_t *packet,
				  unsigned int len,
				  char *interface)
{
	/* check that the packet is large enough to hold an IP header */
	if (len - sizeof(sr_ethernet_hdr_t) < sizeof(sr_ip_hdr_t))
	{
		fprintf(stderr, "** Error: packet is wayy to short \n");
		return;
	}

	Debug("Sensed an ip frame, processing it\n");
	print_hdrs(packet, len);

	sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

	/* check that the packet has a correct checksum */
	uint16_t ip_sum = ip_hdr->ip_sum;
	ip_hdr->ip_sum = 0;
	if (ip_sum != cksum(ip_hdr, sizeof(sr_ip_hdr_t)))
	{
		fprintf(stderr, "** Error: packet has a wrong checksum \n");
		print_hdrs(packet, len);
		ip_hdr->ip_sum = ip_sum;
		return;
	}
	else
	{
		ip_hdr->ip_sum = ip_sum;
	}

	/* check if it is for me */
	uint32_t ip_dst = ip_hdr->ip_dst;
	struct sr_if *if_list = sr->if_list;
	int for_me = 0;
	while (if_list != NULL)
	{
		if (if_list->ip == ip_dst)
		{
			for_me = 1;
			break;
		}
		if_list = if_list->next;
	}

	/* it is for me */
	if (for_me)
	{
		uint8_t ip_p = ip_protocol(packet + sizeof(sr_ethernet_hdr_t));
		/* if it is ICMP echo req */
		if (ip_p == ip_protocol_icmp)
		{
			Debug("\tThe ip packet is for me, sending a icmp echo reply back.\n");
			/* send icmp echo reply (type 0, code 0) */
			sr_send_icmp(sr, packet, 0, 0, interface);
			return;
		}
		/* if it is TCP/UDP */
		else
		{
			Debug("\tTCP/UDP request received on iface %s, sending port unreachable\n", interface);
			/* send icmp port unreachable (type 3, code 3) */
			sr_send_icmp_t3(sr, packet, 3, 3, interface);
			return;
		}
	}
	/* it is not for me */
	else
	{
		Debug("\tGot a packet not destined to the router, forwarding it\n");

		/* decrement the TTL by 1 */
		ip_hdr->ip_ttl--;

		if (ip_hdr->ip_ttl == 0)
		{
			/* send icmp time exceeded (type 11, code 0) */
			sr_send_icmp_t3(sr, packet, 11, 0, interface);
			return;
		}

		/* recompute the packet checksum */
		ip_hdr->ip_sum = 0;
		ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));

		/* find out which entry in the routing table has the longest prefix match 
			 with the destination IP address */
		struct sr_rt *out_rt = sr_rt_for_dst(sr, ip_hdr->ip_dst);

		/* if ip address is not match in routing table */
		if (out_rt == NULL)
		{
			Debug("\tI don't have a routing table for that!\n");
			/* send icmp destination net unreachable (type 3, code 0)*/
			sr_send_icmp_t3(sr, packet, 3, 0, interface);
			return;
		}

		sr_print_routing_entry(out_rt);

		/* get the interface to send the packet */
		struct sr_if *if_entry = sr_get_interface(sr, out_rt->interface);

		/* check ARP cache for the next-hop MAC address*/
		struct sr_arpentry *entry = sr_arpcache_lookup(&(sr->cache), out_rt->gw.s_addr);

		if (entry)
		{
			Debug("Using next_hop_ip->mac mapping in entry to send the packet\n");
			sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet;
			memcpy(ethernet_hdr->ether_shost, if_entry->addr, ETHER_ADDR_LEN);
			memcpy(ethernet_hdr->ether_dhost, entry->mac, ETHER_ADDR_LEN);
			sr_send_packet(sr, packet, len, out_rt->interface);
		}
		else
		{
			Debug("\tNo entry found for receiver IP, queing packet and sending ARP req\n");
			/* sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet; */
			/* memcpy(ethernet_hdr->ether_shost, if_entry->addr, ETHER_ADDR_LEN); */
			/* memset(ethernet_hdr->ether_dhost, 0x00, ETHER_ADDR_LEN); */
			sr_arpcache_queuereq(&(sr->cache), out_rt->gw.s_addr, packet, len, out_rt->interface);
		}
	}
}

void sr_handle_arp(struct sr_instance *sr,
				   uint8_t *packet,
				   unsigned int len,
				   char *interface)
{
	/* check that the packet is large enough to hold an arp header */
	if (len - sizeof(sr_ethernet_hdr_t) < sizeof(sr_arp_hdr_t))
	{
		fprintf(stderr, "** Error: packet is wayy to short for arp header\n");
		return -1;
	}

	Debug("Sensed an ARP frame, processing it\n");
	print_hdrs(packet, len);

	sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
	unsigned short ar_op = ntohs(arp_hdr->ar_op);

	/* reply to me */
	if (ar_op == arp_op_reply)
	{
		sr_handle_arp_reply(sr, arp_hdr, interface);
	}
	/* request to me */
	else if (ar_op == arp_op_request)
	{
		sr_send_arp_reply(sr, packet, len, interface);
	}
	else
	{
		Debug("Didn't get an ARP frame I understood, quitting!\n");
		return;
	}
}

void sr_handle_arp_reply(struct sr_instance *sr,
						 sr_arp_hdr_t *arp_hdr,
						 char *interface)
{
	/* get the interface to send arp reply */
	struct sr_if *iface = sr_get_interface(sr, interface);
	if (iface->ip == arp_hdr->ar_tip)
	{
		Debug("\tGot ARP reply at interfce %s, caching it\n", iface->name);
		/* cache it */
		struct sr_arpcache *cache = &(sr->cache);
		pthread_mutex_lock(&(cache->lock));
		unsigned char mac[ETHER_ADDR_LEN];
		memcpy(mac, arp_hdr->ar_sha, ETHER_ADDR_LEN);
		uint32_t ip = arp_hdr->ar_sip;
		struct sr_arpreq *req = sr_arpcache_insert(cache, mac, ip);

		sr_arpcache_dump(cache);

		/* go through my request queue and send outstanding packets */
		if (req)
		{
			struct sr_packet *packet = req->packets;
			while (packet)
			{
				Debug("Forwarding ia packet that has been waiting for ARP reply\n");
				sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet->buf;
				memcpy(ethernet_hdr->ether_dhost, mac, ETHER_ADDR_LEN);
				memcpy(ethernet_hdr->ether_shost, iface->addr, ETHER_ADDR_LEN);
				sr_send_packet(sr, packet->buf, packet->len, iface->name);
				packet = packet->next;
			}
			sr_arpreq_destroy(&sr->cache, req);
		}
		pthread_mutex_unlock(&(cache->lock));
	}
}

struct sr_rt *sr_rt_for_dst(struct sr_instance *sr, uint32_t dst)
{
	struct sr_rt *rt_walker = sr->routing_table;
	struct sr_rt *best_rt = NULL;
	uint32_t longest_mask = 0;

	while (rt_walker)
	{
		uint32_t d1 = rt_walker->mask.s_addr & dst;

		if (d1 == rt_walker->dest.s_addr)
			if (longest_mask == 0 || rt_walker->mask.s_addr > longest_mask)
			{
				best_rt = rt_walker;
				longest_mask = rt_walker->mask.s_addr;
			}

		rt_walker = rt_walker->next;
	}
	return best_rt;
}

int sr_send_icmp(struct sr_instance *sr,
				 uint8_t *packet,
				 uint8_t icmp_type,
				 uint8_t icmp_code,
				 char *interface)
{
	/* get the ethernet header and ip header of input packet */
	sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet;
	sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
	sr_icmp_t3_hdr_t *icmp_t3_hdr = (sr_icmp_t3_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));

	/* get interface we should be sending the packet out on*/
	struct sr_if *rec_iface = sr_get_interface(sr, interface);

	/* allocate space for icmp packet */
	unsigned int len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
	uint8_t *icmp_packet = (uint8_t *)malloc(len);

	/* construct icmp header */
	sr_icmp_t3_hdr_t *icmp_hdr = (sr_icmp_t3_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
	icmp_hdr->icmp_type = icmp_type;
	icmp_hdr->icmp_code = icmp_code;
	icmp_hdr->unused = icmp_t3_hdr->unused;
	icmp_hdr->next_mtu = icmp_t3_hdr->next_mtu;
	memcpy(icmp_hdr->data, ip_hdr, ICMP_DATA_SIZE);
	icmp_hdr->icmp_sum = 0;
	icmp_hdr->icmp_sum = cksum(icmp_hdr, sizeof(sr_icmp_t3_hdr_t));

	/* construct ip header */
	sr_ip_hdr_t *ip_icmp_hdr = (sr_ip_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t));
	ip_icmp_hdr->ip_hl = 5;
	ip_icmp_hdr->ip_v = ip_hdr->ip_v;
	ip_icmp_hdr->ip_tos = ip_hdr->ip_tos;
	ip_icmp_hdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
	ip_icmp_hdr->ip_id = ip_hdr->ip_id;
	ip_icmp_hdr->ip_off = htons(IP_DF);
	ip_icmp_hdr->ip_dst = ip_hdr->ip_src;
	ip_icmp_hdr->ip_src = rec_iface->ip;
	ip_icmp_hdr->ip_p = ip_protocol_icmp;
	ip_icmp_hdr->ip_ttl = 60;
	ip_icmp_hdr->ip_sum = 0;
	ip_icmp_hdr->ip_sum = cksum(ip_icmp_hdr, sizeof(sr_ip_hdr_t));

	/* construct ethernet header */
	sr_ethernet_hdr_t *ethernet_icmp_hdr = (sr_ethernet_hdr_t *)icmp_packet;
	ethernet_icmp_hdr->ether_type = htons(ethertype_ip);
	memcpy(ethernet_icmp_hdr->ether_dhost, ethernet_hdr->ether_shost, ETHER_ADDR_LEN);
	memcpy(ethernet_icmp_hdr->ether_shost, rec_iface->addr, ETHER_ADDR_LEN);

	print_hdrs(icmp_packet, len);

	return sr_send_packet(sr, icmp_packet, len, interface);
}

int sr_send_icmp_t3(struct sr_instance *sr,
					uint8_t *packet,
					uint8_t icmp_type,
					uint8_t icmp_code,
					char *interface)
{
	/* get the ethernet header and ip header of input packet */
	sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet;
	sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
	sr_icmp_t3_hdr_t *rec_icmp_hdr = (sr_icmp_t3_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));

	/* allocate space for icmp packet */
	unsigned int len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
	uint8_t *icmp_packet = (uint8_t *)malloc(len);

	/* get the interface receive the packet */
	struct sr_if *rec_if = sr_get_interface(sr, interface);

	/* construct icmp header */
	sr_icmp_t3_hdr_t *icmp_hdr = (sr_icmp_t3_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
	icmp_hdr->icmp_type = icmp_type;
	icmp_hdr->icmp_code = icmp_code;
	icmp_hdr->unused = 0;
	icmp_hdr->next_mtu = htons(1500);
	memcpy(icmp_hdr->data, ip_hdr, ICMP_DATA_SIZE);
	icmp_hdr->icmp_sum = 0;
	icmp_hdr->icmp_sum = cksum(icmp_hdr, sizeof(sr_icmp_t3_hdr_t));

	/* construct ip header */
	sr_ip_hdr_t *ip_icmp_hdr = (sr_ip_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t));
	ip_icmp_hdr->ip_hl = 5;
	ip_icmp_hdr->ip_v = ip_hdr->ip_v;
	ip_icmp_hdr->ip_tos = ip_hdr->ip_tos;
	ip_icmp_hdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
	ip_icmp_hdr->ip_id = ip_hdr->ip_id;
	ip_icmp_hdr->ip_off = htons(IP_DF);
	ip_icmp_hdr->ip_dst = ip_hdr->ip_src;
	ip_icmp_hdr->ip_src = rec_if->ip;
	ip_icmp_hdr->ip_p = ip_protocol_icmp;
	ip_icmp_hdr->ip_ttl = 60;
	ip_icmp_hdr->ip_sum = 0;
	ip_icmp_hdr->ip_sum = cksum(ip_icmp_hdr, sizeof(sr_ip_hdr_t));

	/* construct ethernet header */
	sr_ethernet_hdr_t *ethernet_icmp_hdr = (sr_ethernet_hdr_t *)icmp_packet;
	ethernet_icmp_hdr->ether_type = htons(ethertype_ip);
	memcpy(ethernet_icmp_hdr->ether_dhost, ethernet_hdr->ether_shost, ETHER_ADDR_LEN);
	memcpy(ethernet_icmp_hdr->ether_shost, rec_if->addr, ETHER_ADDR_LEN);

	print_hdrs(icmp_packet, len);

	return sr_send_packet(sr, icmp_packet, len, interface);
}

int sr_send_arp_req(struct sr_instance *sr,
					struct sr_arpreq *req)
{
	/* allocate space for arp packet */
	unsigned int len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t);
	uint8_t *packet = (uint8_t *)malloc(len);

	/* get the interface to send the arp packet */
	struct sr_if *iface = sr_get_interface(sr, req->packets->iface);

	/* construct the arp header */
	sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
	arp_hdr->ar_hrd = htons(arp_hrd_ethernet);
	arp_hdr->ar_pro = htons(ethertype_ip);
	arp_hdr->ar_hln = ETHER_ADDR_LEN;
	arp_hdr->ar_pln = 4;
	arp_hdr->ar_op = htons(arp_op_request);
	memcpy(arp_hdr->ar_sha, iface->addr, ETHER_ADDR_LEN);
	arp_hdr->ar_sip = iface->ip;
	memset(arp_hdr->ar_tha, 0x00, ETHER_ADDR_LEN);
	arp_hdr->ar_tip = req->ip;

	/* construct the ethernet header */
	sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet;
	memcpy(ethernet_hdr->ether_shost, iface->addr, ETHER_ADDR_LEN);
	memset(ethernet_hdr->ether_dhost, 0xff, ETHER_ADDR_LEN);
	ethernet_hdr->ether_type = htons(ethertype_arp);

	print_hdrs(packet, len);

	return sr_send_packet(sr, packet, len, iface->name);
}

int sr_send_arp_reply(struct sr_instance *sr,
					  uint8_t *rec_packet,
					  unsigned int rec_len,
					  char *interface)
{
	Debug("\tGot ARP request at interfce %s, repling it\n", interface);
	/* get the ip and mac of own interface */
	struct sr_if *iface = sr_get_interface(sr, interface);
	uint32_t ip = iface->ip;
	unsigned char mac[ETHER_ADDR_LEN];
	memcpy(mac, iface->addr, ETHER_ADDR_LEN);

	/* insert the host to arp cache*/
	/* sr_arpcache_insert(&(sr->cache), mac, ip); */

	/* get the ethernet header and arp header of receive packet */
	sr_ethernet_hdr_t *rec_ethernet_hdr = (sr_ethernet_hdr_t *)rec_packet;
	sr_arp_hdr_t *rec_arp_hdr = (sr_arp_hdr_t *)(rec_packet + sizeof(sr_ethernet_hdr_t));

	/* allocate space for reply arp packet */
	unsigned int len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t);
	uint8_t *packet = (uint8_t *)malloc(len);

	/* construct the arp header */
	sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
	arp_hdr->ar_hrd = rec_arp_hdr->ar_hrd;
	arp_hdr->ar_pro = rec_arp_hdr->ar_pro;
	arp_hdr->ar_hln = rec_arp_hdr->ar_hln;
	arp_hdr->ar_pln = rec_arp_hdr->ar_pln;
	arp_hdr->ar_op = htons(arp_op_reply);
	memcpy(arp_hdr->ar_tha, rec_arp_hdr->ar_sha, ETHER_ADDR_LEN);
	arp_hdr->ar_tip = rec_arp_hdr->ar_sip;
	memcpy(arp_hdr->ar_sha, mac, ETHER_ADDR_LEN);
	arp_hdr->ar_sip = ip;

	/* construct the ethernet header */
	sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet;
	memcpy(ethernet_hdr->ether_dhost, rec_ethernet_hdr->ether_shost, ETHER_ADDR_LEN);
	memcpy(ethernet_hdr->ether_shost, mac, ETHER_ADDR_LEN);
	ethernet_hdr->ether_type = ntohs(ethertype_arp);

	print_hdrs(packet, len);

	return sr_send_packet(sr, packet, len, interface);
}
