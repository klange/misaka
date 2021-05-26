#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

struct ethernet_packet {
	uint8_t destination[6];
	uint8_t source[6];
	uint16_t type;
	uint8_t payload[];
} __attribute__((packed));

struct ipv4_packet {
	uint8_t  version_ihl;
	uint8_t  dscp_ecn;
	uint16_t length;
	uint16_t ident;
	uint16_t flags_fragment;
	uint8_t  ttl;
	uint8_t  protocol;
	uint16_t checksum;
	uint32_t source;
	uint32_t destination;
	uint8_t  payload[];
} __attribute__ ((packed));

struct udp_packet {
	uint16_t source_port;
	uint16_t destination_port;
	uint16_t length;
	uint16_t checksum;
	uint8_t  payload[];
} __attribute__ ((packed));

struct dhcp_packet {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;

	uint32_t xid;

	uint16_t secs;
	uint16_t flags;

	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;

	uint8_t  chaddr[16];

	uint8_t sname[64];
	uint8_t file[128];

	uint32_t magic;

	uint8_t  options[];
} __attribute__ ((packed));

struct dns_packet {
	uint16_t qid;
	uint16_t flags;
	uint16_t questions;
	uint16_t answers;
	uint16_t authorities;
	uint16_t additional;
	uint8_t data[];
} __attribute__ ((packed));

struct tcp_header {
	uint16_t source_port;
	uint16_t destination_port;

	uint32_t seq_number;
	uint32_t ack_number;

	uint16_t flags;
	uint16_t window_size;
	uint16_t checksum;
	uint16_t urgent;

	uint8_t  payload[];
} __attribute__((packed));

struct tcp_check_header {
	uint32_t source;
	uint32_t destination;
	uint8_t  zeros;
	uint8_t  protocol;
	uint16_t tcp_len;
	uint8_t  tcp_header[];
};

#define SOCK_STREAM 1
#define SOCK_DGRAM 2

// Note: Data offset is in upper 4 bits of flags field. Shift and subtract 5 since that is the min TCP size.
//       If the value is more than 5, multiply by 4 because this field is specified in number of words
#define TCP_OPTIONS_LENGTH(tcp) (((((tcp)->flags) >> 12) - 5) * 4)
#define TCP_HEADER_LENGTH(tcp) ((((tcp)->flags) >> 12) * 4)
#define TCP_HEADER_LENGTH_FLIPPED(tcp) (((htons((tcp)->flags)) >> 12) * 4)

#define htonl(l)  ( (((l) & 0xFF) << 24) | (((l) & 0xFF00) << 8) | (((l) & 0xFF0000) >> 8) | (((l) & 0xFF000000) >> 24))
#define htons(s)  ( (((s) & 0xFF) << 8) | (((s) & 0xFF00) >> 8) )
#define ntohl(l)  htonl((l))
#define ntohs(s)  htons((s))

#define BROADCAST_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define IPV4_PROT_UDP 17
#define IPV4_PROT_TCP 6
#define DHCP_MAGIC 0x63825363

#define TCP_FLAGS_FIN (1 << 0)
#define TCP_FLAGS_SYN (1 << 1)
#define TCP_FLAGS_RES (1 << 2)
#define TCP_FLAGS_PSH (1 << 3)
#define TCP_FLAGS_ACK (1 << 4)
#define TCP_FLAGS_URG (1 << 5)
#define TCP_FLAGS_ECE (1 << 6)
#define TCP_FLAGS_CWR (1 << 7)
#define TCP_FLAGS_NS  (1 << 8)
#define DATA_OFFSET_5 (0x5 << 12)

#define ETHERNET_TYPE_IPV4 0x0800
#define ETHERNET_TYPE_ARP  0x0806

struct payload {
	struct ethernet_packet eth_header;
	struct ipv4_packet     ip_header;
	struct udp_packet      udp_header;
	struct dhcp_packet     dhcp_header;
	uint8_t payload[32];
};

uint16_t calculate_ipv4_checksum(struct ipv4_packet * p) {
	uint32_t sum = 0;
	uint16_t * s = (uint16_t *)p;

	/* TODO: Checksums for options? */
	for (int i = 0; i < 10; ++i) {
		sum += ntohs(s[i]);
	}

	if (sum > 0xFFFF) {
		sum = (sum >> 16) + (sum & 0xFFFF);
	}

	return ~(sum & 0xFFFF) & 0xFFFF;
}

void ip_ntoa(uint32_t src_addr, char * out) {
	sprintf(out, "%d.%d.%d.%d",
		(src_addr & 0xFF000000) >> 24,
		(src_addr & 0xFF0000) >> 16,
		(src_addr & 0xFF00) >> 8,
		(src_addr & 0xFF));
}

uint8_t mac_addr[6];

void fill(struct payload *it, size_t payload_size) {

	it->eth_header.source[0] = mac_addr[0];
	it->eth_header.source[1] = mac_addr[1];
	it->eth_header.source[2] = mac_addr[2];
	it->eth_header.source[3] = mac_addr[3];
	it->eth_header.source[4] = mac_addr[4];
	it->eth_header.source[5] = mac_addr[5];

	it->eth_header.destination[0] = 0xFF;
	it->eth_header.destination[1] = 0xFF;
	it->eth_header.destination[2] = 0xFF;
	it->eth_header.destination[3] = 0xFF;
	it->eth_header.destination[4] = 0xFF;
	it->eth_header.destination[5] = 0xFF;

	it->eth_header.type = htons(0x0800);

	it->ip_header.version_ihl = ((0x4 << 4) | (0x5 << 0));
	it->ip_header.dscp_ecn    = 0;
	it->ip_header.length      = htons(sizeof(struct ipv4_packet) + sizeof(struct udp_packet) + sizeof(struct dhcp_packet) + payload_size);
	it->ip_header.ident       = htons(1);
	it->ip_header.flags_fragment = 0;
	it->ip_header.ttl         = 0x40;
	it->ip_header.protocol    = IPV4_PROT_UDP;
	it->ip_header.checksum    = 0;
	it->ip_header.source      = htonl(0);
	it->ip_header.destination = htonl(0xFFFFFFFF);

	it->ip_header.checksum = htons(calculate_ipv4_checksum(&it->ip_header));

	it->udp_header.source_port = htons(68);
	it->udp_header.destination_port = htons(67);
	it->udp_header.length = htons(sizeof(struct udp_packet) + sizeof(struct dhcp_packet) + payload_size);
	it->udp_header.checksum = 0; /* uh */

	it->dhcp_header.op = 1;
	it->dhcp_header.htype = 1;
	it->dhcp_header.hlen = 6;
	it->dhcp_header.hops = 0;
	it->dhcp_header.xid = htons(0x1337); /* transaction id... */
	it->dhcp_header.secs = 0;
	it->dhcp_header.flags = 0;

	it->dhcp_header.ciaddr = 0;
	it->dhcp_header.yiaddr = 0;
	it->dhcp_header.siaddr = 0;
	it->dhcp_header.giaddr = 0;
	it->dhcp_header.chaddr[0] = mac_addr[0];
	it->dhcp_header.chaddr[1] = mac_addr[1];
	it->dhcp_header.chaddr[2] = mac_addr[2];
	it->dhcp_header.chaddr[3] = mac_addr[3];
	it->dhcp_header.chaddr[4] = mac_addr[4];
	it->dhcp_header.chaddr[5] = mac_addr[5];

	it->dhcp_header.magic = htonl(DHCP_MAGIC);
}

int main(int argc, char * argv[]) {

	/* Let's make a socket. */
	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	//if (sockfd < 0) { perror("socket"); return 1; }

	/* Bind the socket to the requested device. */
	//if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, 

	char * if_name = "enp0s4";
	char if_path[100];

	if (argc > 1) {
		if_name = argv[1];
	}

	snprintf(if_path, 100, "/dev/net/%s", if_name);

	int netdev = open(if_path, O_RDWR);

	fprintf(stderr, "Configuring %s\n", if_name);

	if (ioctl(netdev, 0x12340001, &mac_addr)) {
		fprintf(stderr, "could not get mac address\n");
		return 1;
	}

	fprintf(stderr, "mac address: %02x:%02x:%02x:%02x:%02x:%02x\n",
		mac_addr[0], mac_addr[1], mac_addr[2],
		mac_addr[3], mac_addr[4], mac_addr[5]);

	/* Try to frob the whatsit */
	{
		struct payload thething = {
			.payload = {53,1,1,55,2,3,6,255,0}
		};

		fill(&thething, 8);

		write(netdev, &thething, sizeof(struct payload));
	}

	uint32_t yiaddr;
	uint32_t siaddr;

	do {
		char buf[8092] = {0};
		ssize_t rsize = read(netdev, &buf, 8092);

		if (rsize <= 0) {
			printf("bad size? %zd\n", rsize);
			continue;
		}

		struct payload * response = (void*)buf;

		if (ntohs(response->udp_header.destination_port) != 68) {
			printf("not what I was expecting\n");
			continue;
		}

		yiaddr = response->dhcp_header.yiaddr;
		siaddr = response->dhcp_header.siaddr;
		char yiaddr_ip[16];
		ip_ntoa(ntohl(yiaddr), yiaddr_ip);

		printf("Response from DHCP Discover: %s\n", yiaddr_ip);

		break;
	} while (1);

	{
		printf("Writing request\n");
		struct payload thething = {
			.payload = {53,1,3,50,4,
				(yiaddr) & 0xFF,
				(yiaddr >> 8) & 0xFF,
				(yiaddr >> 16) & 0xFF,
				(yiaddr >> 24) & 0xFF,
				55,2,3,6,255,0}
		};

		fill(&thething, 14);

		thething.dhcp_header.ciaddr = yiaddr;
		thething.dhcp_header.siaddr = siaddr;

		write(netdev, &thething, sizeof(struct payload));
	}

	do {
		char buf[8092] = {0};
		ssize_t rsize = read(netdev, &buf, 8092);

		if (rsize <= 0) {
			printf("bad size? %zd\n", rsize);
			continue;
		}

		struct payload * response = (void*)buf;

		if (ntohs(response->udp_header.destination_port) != 68) {
			printf("not what I was expecting\n");
			continue;
		}

		yiaddr = response->dhcp_header.yiaddr;
		siaddr = response->dhcp_header.siaddr;
		char yiaddr_ip[16];
		ip_ntoa(ntohl(yiaddr), yiaddr_ip);

		printf("ACK returns: %s\n", yiaddr_ip);

		break;
	} while (1);

	return 0;
}
