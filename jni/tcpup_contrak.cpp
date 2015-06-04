#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <time.h>
#include <assert.h>

#include "tcpup/up.h"
#include "tcpup/ip.h"
#include "tcpup/fsm.h"
#include "tcpup/crypt.h"
#include "tcpup/contrak.h"

#define SEQ_GT(a,b)     ((int)((a)-(b)) > 0)

struct tcpup_info {
	int t_conv;
	int t_state;
	int x_state;
	int t_wscale; //linux default is 7
	time_t t_rcvtime;

	u_short s_port;
	unsigned int t_from;

	u_short d_port;
	unsigned int t_peer;

	tcp_seq snd_una;
	tcp_seq rcv_una;

	struct tcpup_info *next;
};

static int _tot_tcp = 0;
static struct tcpup_info *_tcpup_info_header = NULL;

static char const * const tcpstates[] = { 
	"CLOSED",       "LISTEN",       "SYN_SENT",     "SYN_RCVD",
	"ESTABLISHED",  "CLOSE_WAIT",   "FIN_WAIT_1",   "CLOSING",
	"LAST_ACK",     "FIN_WAIT_2",   "TIME_WAIT",
};

static void tcp_state_update(struct tcpup_info *upp, int state)
{
	fprintf(stderr, "%x/%-4d  %s\t -> %s\n",
			upp->t_conv, _tot_tcp, tcpstates[upp->t_state], tcpstates[state]);
	upp->t_state = state;
	upp->x_state = state;
	return;
}

static void tcp_state_preload(struct tcpup_info *upp, int state, tcp_seq ack_seq)
{
	if (upp->x_state != state) {
		fprintf(stderr, "%x/%-4d  %s\t <- %s\n",
				upp->t_conv, _tot_tcp, tcpstates[upp->t_state], tcpstates[state]);
		upp->rcv_una = ack_seq;
		upp->x_state = state;
	}

	return;
}

static int tcpup_state_send(struct tcpup_info *upp, struct tcpiphdr *tcp, size_t dlen)
{
	int xflags = 0;

	if (tcp->th_flags & TH_RST) {
		upp->t_state = TCPS_CLOSED;
		return 0;
	}

	if (upp->x_state != upp->t_state
			&& (tcp->th_flags & TH_ACK)
			&& SEQ_GT(htonl(tcp->th_ack), upp->rcv_una)) {
		tcp_state_update(upp, upp->x_state);
		upp->t_state = upp->x_state;
	}

	switch (upp->t_state) {
		case TCPS_CLOSED:
			xflags = TH_SYN| TH_ACK;
			if ((tcp->th_flags & xflags) == TH_SYN) {
				tcp_state_update(upp, TCPS_SYN_SENT);
				upp->snd_una = htonl(tcp->th_seq);
				return 0;
			}
			break;

		case TCPS_SYN_RECEIVED:
			assert((tcp->th_flags & TH_FIN) != TH_FIN);
			xflags = TH_SYN| TH_ACK;
			if ((tcp->th_flags & xflags) == TH_ACK
					&& SEQ_GT(htonl(tcp->th_seq), upp->snd_una)) {
				tcp_state_update(upp, upp->x_state);
				return 0;
			}
			break;

		case TCPS_ESTABLISHED:
			if ((tcp->th_flags & TH_FIN) == TH_FIN) {
				upp->snd_una = htonl(tcp->th_seq) + dlen;
				tcp_state_update(upp, TCPS_FIN_WAIT_1);
				return 0;
			}
			break;

		case TCPS_CLOSE_WAIT:
			if ((tcp->th_flags & TH_FIN) == TH_FIN) {
				upp->snd_una = htonl(tcp->th_seq) + dlen;
				tcp_state_update(upp, TCPS_LAST_ACK);
				return 0;
			}
			break;

		case TCPS_FIN_WAIT_1:
			xflags = TH_FIN| TH_ACK;
			if ((tcp->th_flags & xflags) == TH_ACK) {
				tcp_state_update(upp, upp->x_state);
				return 0;
			}
			break;
	}

	return 0;
}

static int tcpup_state_receive(struct tcpup_info *upp, struct tcpiphdr *tcp, size_t dlen)
{
	int xflags = 0;

	if (tcp->th_flags & TH_RST) {
		upp->t_state = TCPS_CLOSED;
		return 0;
	}

	switch (upp->t_state) {
		case TCPS_SYN_SENT:
			xflags = TH_SYN| TH_ACK;
			if ((tcp->th_flags & xflags) == TH_SYN) {
				assert((tcp->th_flags & TH_FIN) != TH_FIN);
				tcp_state_preload(upp, TCPS_SYN_RECEIVED, htonl(tcp->th_seq));
				return 0;
			}

			if ((tcp->th_flags & xflags) == xflags
					&& SEQ_GT(htonl(tcp->th_ack), upp->snd_una)) {
				assert((tcp->th_flags & TH_FIN) != TH_FIN);
				tcp_state_preload(upp, TCPS_ESTABLISHED, htonl(tcp->th_seq));
				return 0;
			}
			break;

		case TCPS_SYN_RECEIVED:
			if ((tcp->th_flags & TH_ACK) == TH_ACK
					&& SEQ_GT(htonl(tcp->th_ack), upp->snd_una)) {
				assert((tcp->th_flags & TH_FIN) != TH_FIN);
				tcp_state_preload(upp, TCPS_ESTABLISHED, htonl(tcp->th_seq));
				return 0;
			}
			break;

		case TCPS_ESTABLISHED:
			if ((tcp->th_flags & TH_FIN) == TH_FIN) {
				tcp_state_preload(upp, TCPS_CLOSE_WAIT, htonl(tcp->th_seq));
				return 0;
			}
			break;

		case TCPS_FIN_WAIT_1:
			xflags = TH_FIN| TH_ACK;
			if ((tcp->th_flags & xflags) == xflags
					&& SEQ_GT(htonl(tcp->th_ack), upp->snd_una)) {
				tcp_state_preload(upp, TCPS_TIME_WAIT, htonl(tcp->th_seq) + dlen);
				return 0;
			}

			if ((tcp->th_flags & TH_FIN) == TH_FIN) {
				tcp_state_preload(upp, TCPS_CLOSING, htonl(tcp->th_seq) + dlen);
				return 0;
			}

			if ((tcp->th_flags & TH_ACK) == TH_ACK
					&& SEQ_GT(htonl(tcp->th_ack), upp->snd_una)) {
				tcp_state_preload(upp, TCPS_FIN_WAIT_2, htonl(tcp->th_seq) + dlen);
				return 0;
			}
			break;

		case TCPS_FIN_WAIT_2:
			if ((tcp->th_flags & TH_FIN) == TH_FIN) {
				tcp_state_preload(upp, TCPS_TIME_WAIT, htonl(tcp->th_seq) + dlen);
				return 0;
			}
			break;

		case TCPS_CLOSING:
			if ((tcp->th_flags & TH_ACK) == TH_ACK
					&& SEQ_GT(htonl(tcp->th_ack), upp->snd_una)) {
				tcp_state_preload(upp, TCPS_TIME_WAIT, htonl(tcp->th_seq) + dlen);
				return 0;
			}
			break;

		case TCPS_LAST_ACK:
			if ((tcp->th_flags & TH_ACK) == TH_ACK
					&& SEQ_GT(htonl(tcp->th_ack), upp->snd_una)) {
				tcp_state_preload(upp, TCPS_CLOSED, htonl(tcp->th_seq) + dlen);
				return 0;
			}
			break;

		case TCPS_TIME_WAIT:
			fprintf(stderr, "before TIME_WAIT -> TIME_WAIT\n");
			break;
	}

	return 0;
}

static tcpup_info *tcpup_findcb(int src, int dst, u_short sport, u_short dport)
{
	struct tcpup_info *tp;

	for (tp = _tcpup_info_header; tp; tp = tp->next) {
		if (tp->s_port != sport ||
				tp->d_port != dport) {
			continue;
		}

		if (tp->t_from != src ||
				tp->t_peer != dst) {
			continue;
		}

		return tp;
	}

	return 0;
}

static tcpup_info *tcpup_lookup(uint32_t conv)
{
	time_t now;
	struct tcpup_info *tp;
	struct tcpup_info *tp_next;
	struct tcpup_info **tp_prev = &_tcpup_info_header;

	time(&now);
	for (tp = _tcpup_info_header; tp; tp = tp_next) {
		if (tp->t_conv == conv) {
			tp->t_rcvtime = now;
			return tp;
		}

		tp_next = tp->next;
		switch (tp->t_state) {
			case TCPS_CLOSED:
				*tp_prev = tp->next;
				_tot_tcp--;
				delete tp;
				continue;

			case TCPS_LAST_ACK:
			case TCPS_TIME_WAIT:
				if (tp->t_rcvtime + 6 <= now) {
					*tp_prev = tp->next;
					_tot_tcp--;
					delete tp;
					continue;
				}
				break;

			default:
				if (tp->t_rcvtime + 600 <= now) {
					*tp_prev = tp->next;
					_tot_tcp--;
					delete tp;
					continue;
				}
				break;
		}

		tp_prev = &tp->next;
	}

	return 0;
}

static tcpup_info *tcpup_newcb(int src, int dst, u_short sport, u_short dport)
{
	struct tcpup_info *up = new tcpup_info;
	assert(up != NULL);
	tcpup_lookup(-1);
	memset(up, 0, sizeof(*up));

	up->t_conv = (0xffffffff & (long)(up));

	up->t_from = src;
	up->t_peer = dst;
	up->s_port = sport;
	up->d_port = dport;
	up->t_state = TCPS_CLOSED;
	up->x_state = TCPS_CLOSED;
	up->t_wscale = 7;
	up->t_rcvtime = time(NULL);

	up->next = _tcpup_info_header;
	_tcpup_info_header = up;
	_tot_tcp++;

	return up;
}

static u_char _null_[8] = {0};
static u_char type_len_map[8] = {0x0, 0x04, 0x0, 0x0, 0x0, 0x10};

static int set_relay_info(u_char *target, int type, void *host, u_short port)
{
	int len;
	char *p, buf[60];

	p = (char *)target;
	*p++ = (type & 0xff);
	*p++ = 0;

	memcpy(p, &port, 2); 
	p += 2;

	len = type_len_map[type & 0x7];
	memcpy(p, host, len);
	p += len;

	return p - (char *)target;
}

static int translate_tcpip(struct tcpup_info *info, struct tcpuphdr *field, struct tcpiphdr *tcp, int length)
{
	int cnt;
	int offip, offup;
	u_char *dst, *src = 0;
	struct tcpupopt to = {0};

	field->th_seq = tcp->th_seq;
	field->th_ack = tcp->th_ack;
	field->th_magic = MAGIC_UDP_TCP;

	field->th_win   = tcp->th_win;
	field->th_flags = tcp->th_flags;

	cnt = (tcp->th_off << 2);
	src = (u_char *)(tcp + 1);
	dst = (u_char *)(field + 1);

	offip = tcpip_dooptions(&to, src, cnt - sizeof(*tcp));
	if (tcp->th_flags & TH_SYN) {
		to.to_flags |= TOF_DESTINATION;
		to.to_dslen  = set_relay_info(_null_, 0x01, &info->t_peer, info->d_port);
		to.to_dsaddr = _null_;

		if (to.to_flags & TOF_SCALE) {
			/* TODO: wscale will be not 7 */
			info->t_wscale = to.to_wscale;
		}
	}

	if (to.to_flags & TOF_TS) {
		field->th_tsecr = htonl(to.to_tsecr);
		field->th_tsval = htonl(to.to_tsval);
	}

	if (info->t_wscale != 7) {
		/* convert windows scale from old to new */
		unsigned int win = htons(tcp->th_win) << info->t_wscale;
		field->th_win = htons(win >> 7);
	}

	offup = tcpup_addoptions(&to, dst);
	field->th_opten = (offup >> 2);

	cnt = length - offip - sizeof(*tcp);
	assert(cnt >= 0);
	memcpy(dst + offup, src + offip, cnt);
	tcpup_state_send(info, tcp, cnt);

	return cnt + sizeof(*field) + offup;
}

static int translate_tcpup(struct tcpiphdr *tcp, struct tcpuphdr *field, int length)
{
	int cnt;
	int offip, offup;
	u_char *dst, *src = 0;
	struct tcpupopt to = {0};

	tcp->th_seq = field->th_seq;
	tcp->th_ack = field->th_ack;
	tcp->th_win  = field->th_win;
	tcp->th_flags  = field->th_flags;

	cnt = (field->th_opten << 2);
	src = (u_char *)(field + 1);
	dst = (u_char *)(tcp + 1);

	offup = tcpup_dooptions(&to, src, cnt);
	to.to_flags |= TOF_TS;
	to.to_tsval  = htonl(field->th_tsval);
	to.to_tsecr  = htonl(field->th_tsecr);

	if (tcp->th_flags & TH_SYN) {
		to.to_wscale = 7;
		to.to_flags |= TOF_SCALE;
		to.to_flags |= TOF_SACKPERM;
	}

	offip = tcpip_addoptions(&to, dst);
	tcp->th_off    = (sizeof(*tcp) + offip) >> 2;
	tcp->th_x2     = 0;
	tcp->th_urp    = 0;

	cnt = length - offup;
	assert(cnt >= 0);
	memcpy(dst + offip, ((char *)field) + offup, cnt);

	return cnt + sizeof(*tcp) + offip;
}

int translate_ip2up(unsigned char *buf, size_t size, unsigned char *packet, size_t length)
{
	int offset;

	struct iphdr *ip;
	struct tcpiphdr *tcp;
	struct tcpup_info *upp = NULL;

	ip = (struct iphdr *)packet;
	tcp = (struct tcpiphdr *)(ip + 1);

	if (ip->protocol != IPPROTO_TCP) {
		fprintf(stderr, "drop, protocol not support: %d\n", ip->protocol);
		return 0;
	}

	upp = tcpup_findcb(ip->saddr, ip->daddr, tcp->th_sport, tcp->th_dport);

	if (upp == NULL) {
		if (tcp->th_flags & TH_RST) {
			/* silent drop, ignore packet */
			fprintf(stderr, "silent drop, ignore packet\n");
			return 0;
		}

		if (tcp->th_flags & TH_ACK) {
			/* send back rst */
			fprintf(stderr, "send back rst, but ignore\n");
			return -1;
		}

		if (tcp->th_flags & TH_SYN) {
			fprintf(stderr, "tcpup connect context is created\n");
			upp = tcpup_newcb(ip->saddr, ip->daddr, tcp->th_sport, tcp->th_dport);
			assert(upp != NULL);
		} else {
			/* silent drop, ignore packet */
			fprintf(stderr, "silent drop, ignore packet\n");
			return 0;
		}
	}

	struct tcpuphdr *uphdr = (struct tcpuphdr *)buf;
	offset = translate_tcpip(upp, uphdr, tcp, length - sizeof(*ip));
	uphdr->th_conv = upp->t_conv;

	return offset;
}

int tcp_reset_fill(unsigned char *buf, unsigned char *packet, size_t length)
{
	struct iphdr *ip;
	struct iphdr *ip1;
	struct tcpiphdr *tcp;
	struct tcpiphdr *tcp1;
	struct in_addr saddr, daddr;

	ip = (struct iphdr *)buf;
	tcp = (struct tcpiphdr *)(ip + 1);

	ip1 = (struct iphdr *)packet;
	tcp1 = (struct tcpiphdr *)(ip1 + 1);

	ip->ihl = 5;
	ip->version = 4;
	ip->tos = 0;
	ip->tot_len = htons(sizeof(*tcp) + sizeof(*ip));
	ip->id  = (0xffff & (long)ip);
	ip->frag_off = htons(0x4000);
	ip->ttl = 8;
	ip->protocol = IPPROTO_TCP;
	ip->check = 0;

	ip->saddr = ip1->daddr;
	ip->daddr = ip1->saddr;
	tcp->th_dport = tcp1->th_sport;
	tcp->th_sport = tcp1->th_dport;
	tcp->th_sum   = 0;

	if (tcp1->th_flags & TH_SYN) {
		tcp->th_seq = 0;
		tcp->th_ack = htonl(htonl(tcp1->th_seq) + 1);
		tcp->th_win  = 0;
		tcp->th_flags  = (TH_ACK | TH_RST);
	} else if (tcp1->th_flags & TH_ACK) {
		tcp->th_seq = tcp1->th_ack;
		tcp->th_ack = 0;
		tcp->th_win  = 0;
		tcp->th_flags  = (TH_RST);
	}

	tcp->th_off    = (sizeof(*tcp) >> 2);
	tcp->th_x2     = 0;
	tcp->th_urp    = 0;

	saddr.s_addr = ip->saddr;
	daddr.s_addr = ip->daddr;
	tcp_checksum(&tcp->th_sum, &saddr, &daddr, tcp, sizeof(*tcp));

	ip_checksum(&ip->check, ip, sizeof(*ip));

	return sizeof(*ip) + sizeof(*tcp);
}

int tcpup_reset_fill(unsigned char *buf, unsigned char *packet, size_t length)
{
	struct tcpuphdr *tcp;
	struct tcpuphdr *tcp1;

	tcp = (struct tcpuphdr *)(buf);
	tcp1 = (struct tcpuphdr *)(packet);

	if (tcp1->th_flags & TH_SYN) {
		tcp->th_seq = 0;
		tcp->th_ack = htonl(htonl(tcp1->th_seq) + 1);
		tcp->th_win  = 0;
		tcp->th_flags  = (TH_ACK | TH_RST);
	} else if (tcp1->th_flags & TH_ACK) {
		tcp->th_seq = tcp1->th_ack;
		tcp->th_ack = 0;
		tcp->th_win  = 0;
		tcp->th_flags  = (TH_RST);
	}

	tcp->th_magic = MAGIC_UDP_TCP;
	tcp->th_opten = 0;
	return sizeof(*tcp);
}

int translate_up2ip(unsigned char *buf, size_t size, unsigned char *packet, size_t length)
{
	int offset;
	struct iphdr *ip;
	struct tcpiphdr *tcp;
	struct in_addr saddr, daddr;
	struct tcpup_info *upp = NULL;
	struct tcpuphdr  *field = (struct tcpuphdr *)buf;

	field = (struct tcpuphdr *)packet;
	upp = tcpup_lookup(field->th_conv);

	if (upp == NULL) {
		fprintf(stderr, "%x not find\n", field->th_conv);
		if (field->th_flags & TH_RST) {
			/* silent drop, ignore packet */
			return 0;
		}

		if (field->th_flags & TH_ACK) {
			/* send back rst */
			return -1;
		}

		/* !field->syn */
		/* silent drop, ignore packet */

		return 0;
	}

	ip = (struct iphdr *)buf;
	tcp = (struct tcpiphdr *)(ip + 1);

	offset = translate_tcpup(tcp, field, length);

	ip->ihl = 5;
	ip->version = 4;
	ip->tos = 0;
	ip->tot_len = htons(offset + sizeof(*ip));
	ip->id  = (0xffff & (long)ip);
	ip->frag_off = htons(0x4000);
	ip->ttl = 8;
	ip->protocol = IPPROTO_TCP;
	ip->check = 0;

	ip->saddr = upp->t_peer;
	ip->daddr = upp->t_from;
	tcp->th_dport = upp->s_port;
	tcp->th_sport = upp->d_port;
	tcp->th_sum   = 0;

	saddr.s_addr = ip->saddr;
	daddr.s_addr = ip->daddr;
	tcp_checksum(&tcp->th_sum, &saddr, &daddr, tcp, offset);

	ip_checksum(&ip->check, ip, sizeof(*ip));

	tcpup_state_receive(upp, tcp, offset - (tcp->th_off << 2));
	return offset + sizeof(*ip);
}