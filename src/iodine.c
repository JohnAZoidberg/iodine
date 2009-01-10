/*
 * Copyright (c) 2006-2009 Bjorn Andersson <flex@kryo.se>, Erik Ekman <yarrick@kryo.se>
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <err.h>
#include <grp.h>
#include <pwd.h>
#include <arpa/inet.h>
#include <zlib.h>
#include <arpa/nameser.h>
#ifdef DARWIN
#include <arpa/nameser8_compat.h>
#endif

#include "common.h"
#include "encoding.h"
#include "base32.h"
#include "base64.h"
#include "dns.h"
#include "login.h"
#include "tun.h"
#include "version.h"

static void send_ping(int fd);
static void send_chunk(int fd);
static int build_hostname(char *buf, size_t buflen, 
	const char *data, const size_t datalen, 
	const char *topdomain, struct encoder *encoder);

static int running = 1;
static char password[33];

static struct sockaddr_in nameserv;
static char *topdomain;

static uint16_t rand_seed;
static int downstream_seqno;
static int downstream_fragment;
static int down_ack_seqno;
static int down_ack_fragment;

static int max_downstream_frag_size;
static int autodetect_frag_size;

/* Current up/downstream IP packet */
static struct packet outpkt;
static struct packet inpkt;

/* My userid at the server */
static char userid;

/* DNS id for next packet */
static uint16_t chunkid;

/* Base32 encoder used for non-data packets */
static struct encoder *b32;

/* The encoder used for data packets
 * Defaults to Base32, can be changed after handshake */
static struct encoder *dataenc;

/* result of case preservation check done after login */
static int case_preserved;

#if !defined(BSD) && !defined(__GLIBC__)
static char *__progname;
#endif

static void
sighandler(int sig) 
{
	running = 0;
}

static void
send_query(int fd, char *hostname)
{
	char packet[4096];
	struct query q;
	size_t len;

	q.id = ++chunkid;
	q.type = T_NULL;

	len = dns_encode(packet, sizeof(packet), &q, QR_QUERY, hostname, strlen(hostname));

	sendto(fd, packet, len, 0, (struct sockaddr*)&nameserv, sizeof(nameserv));
}

static void
send_packet(int fd, char cmd, const char *data, const size_t datalen)
{
	char buf[4096];

	buf[0] = cmd;
	
	build_hostname(buf + 1, sizeof(buf) - 1, data, datalen, topdomain, b32);
	send_query(fd, buf);
}

static int
build_hostname(char *buf, size_t buflen, 
		const char *data, const size_t datalen, 
		const char *topdomain, struct encoder *encoder)
{
	int encsize;
	size_t space;
	char *b;

	space = MIN(0xFF, buflen) - strlen(topdomain) - 5;
	if (!encoder->places_dots())
		space -= (space / 57); /* space for dots */

	memset(buf, 0, buflen);
	
	encsize = encoder->encode(buf, &space, data, datalen);

	if (!encoder->places_dots())
		inline_dotify(buf, buflen);

	b = buf;
	b += strlen(buf);

	if (*b != '.') 
		*b++ = '.';

	strncpy(b, topdomain, strlen(topdomain)+1);

	return space;
}

static int
is_sending()
{
	return (outpkt.len != 0);
}

static int
read_dns(int fd, char *buf, int buflen)
{
	struct sockaddr_in from;
	char data[64*1024];
	socklen_t addrlen;
	struct query q;
	int rv;
	int r;

	addrlen = sizeof(struct sockaddr);
	if ((r = recvfrom(fd, data, sizeof(data), 0, 
			  (struct sockaddr*)&from, &addrlen)) == -1) {
		warn("recvfrom");
		return 0;
	}

	rv = dns_decode(buf, buflen, &q, QR_ANSWER, data, r);

	/* decode the data header, update seqno and frag before next request */
	if (rv >= 2) {
		downstream_seqno = (buf[1] >> 5) & 7;
		downstream_fragment = (buf[1] >> 1) & 15;
	}


	if (is_sending()) {
		if (chunkid == q.id) {
			/* Got ACK on sent packet */
			outpkt.offset += outpkt.sentlen;
			if (outpkt.offset == outpkt.len) {
				/* Packet completed */
				outpkt.offset = 0;
				outpkt.len = 0;
				outpkt.sentlen = 0;

				/* If the ack contains unacked frag number but no data, 
				 * send a ping to ack the frag number and get more data*/
				if (rv == 2 && (
					downstream_seqno != down_ack_seqno ||
					downstream_fragment != down_ack_fragment
					)) {
					
					send_ping(fd);
				}
			} else {
				/* More to send */
				send_chunk(fd);
			}

		}
	}
	return rv;
}


static int
tunnel_tun(int tun_fd, int dns_fd)
{
	unsigned long outlen;
	unsigned long inlen;
	char out[64*1024];
	char in[64*1024];
	size_t read;

	if ((read = read_tun(tun_fd, in, sizeof(in))) <= 0)
		return -1;

	outlen = sizeof(out);
	inlen = read;
	compress2((uint8_t*)out, &outlen, (uint8_t*)in, inlen, 9);

	memcpy(outpkt.data, out, MIN(outlen, sizeof(outpkt.data)));
	outpkt.sentlen = 0;
	outpkt.offset = 0;
	outpkt.len = outlen;
	outpkt.seqno++;
	outpkt.fragment = 0;

	send_chunk(dns_fd);

	return read;
}

static int
tunnel_dns(int tun_fd, int dns_fd)
{
	unsigned long datalen;
	char buf[64*1024];
	size_t read;

	if ((read = read_dns(dns_fd, buf, sizeof(buf))) <= 2) 
		return -1;

	if (downstream_seqno != inpkt.seqno) {
		/* New packet */
		inpkt.seqno = downstream_seqno;
		inpkt.fragment = downstream_fragment;
		inpkt.len = 0;
	} else if (downstream_fragment <= inpkt.fragment) {
		/* Duplicate fragment */
		return -1;
	}
	inpkt.fragment = downstream_fragment;

	datalen = MIN(read - 2, sizeof(inpkt.data) - inpkt.len);

	/* Skip 2 byte data header and append to packet */
	memcpy(&inpkt.data[inpkt.len], &buf[2], datalen);
	inpkt.len += datalen;

	if (buf[1] & 1) { /* If last fragment flag is set */
		/* Uncompress packet and send to tun */
		datalen = sizeof(buf);
		if (uncompress((uint8_t*)buf, &datalen, (uint8_t*) inpkt.data, inpkt.len) == Z_OK) {
			write_tun(tun_fd, buf, datalen);
		}
		inpkt.len = 0;
	}

	/* If we have nothing to send, send a ping to get more data */
	if (!is_sending()) 
		send_ping(dns_fd);
	
	return read;
}

static int
tunnel(int tun_fd, int dns_fd)
{
	struct timeval tv;
	fd_set fds;
	int rv;
	int i;

	rv = 0;

	while (running) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;


		FD_ZERO(&fds);
		if (!is_sending()) {
			FD_SET(tun_fd, &fds);
		}
		FD_SET(dns_fd, &fds);

		i = select(MAX(tun_fd, dns_fd) + 1, &fds, NULL, NULL, &tv);
		
		if (running == 0)
			break;

		if (i < 0) 
			err(1, "select");

		if (i == 0) /* timeout */
			send_ping(dns_fd);
		else {
			if (FD_ISSET(tun_fd, &fds)) {
				if (tunnel_tun(tun_fd, dns_fd) <= 0)
					continue;
			}
			if (FD_ISSET(dns_fd, &fds)) {
				if (tunnel_dns(tun_fd, dns_fd) <= 0)
					continue;
			} 
		}
	}

	return rv;
}

static void
send_chunk(int fd)
{
	char hex[] = "0123456789ABCDEF";
	char buf[4096];
	int avail;
	int code;
	char *p;

	p = outpkt.data;
	p += outpkt.offset;
	avail = outpkt.len - outpkt.offset;

	outpkt.sentlen = build_hostname(buf + 4, sizeof(buf) - 4, p, avail, topdomain, dataenc);

	/* Build upstream data header (see doc/proto_xxxxxxxx.txt) */

	buf[0] = hex[userid & 15]; /* First byte is 4 bits userid */

	code = ((outpkt.seqno & 7) << 2) | ((outpkt.fragment & 15) >> 2);
	buf[1] = b32_5to8(code); /* Second byte is 3 bits seqno, 2 upper bits fragment count */

	code = ((outpkt.fragment & 3) << 3) | (downstream_seqno & 7);
	buf[2] = b32_5to8(code); /* Third byte is 2 bits lower fragment count, 3 bits downstream packet seqno */

	code = ((downstream_fragment & 15) << 1) | (outpkt.sentlen == avail);
	buf[3] = b32_5to8(code); /* Fourth byte is 4 bits downstream fragment count, 1 bit last frag flag */

	down_ack_seqno = downstream_seqno;
	down_ack_fragment = downstream_fragment;

	outpkt.fragment++;
	send_query(fd, buf);
}

static void
send_login(int fd, char *login, int len)
{
	char data[19];

	memset(data, 0, sizeof(data));
	data[0] = userid;
	memcpy(&data[1], login, MIN(len, 16));

	data[17] = (rand_seed >> 8) & 0xff;
	data[18] = (rand_seed >> 0) & 0xff;
	
	rand_seed++;

	send_packet(fd, 'L', data, sizeof(data));
}

static void
send_ping(int fd)
{
	char data[4];
	
	if (is_sending()) {
		outpkt.sentlen = 0;
		outpkt.offset = 0;
		outpkt.len = 0;
	}

	data[0] = userid;
	data[1] = ((downstream_seqno & 7) << 4) | (downstream_fragment & 15);
	data[2] = (rand_seed >> 8) & 0xff;
	data[3] = (rand_seed >> 0) & 0xff;
	
	down_ack_seqno = downstream_seqno;
	down_ack_fragment = downstream_fragment;
	
	rand_seed++;

	send_packet(fd, 'P', data, sizeof(data));
}

static void
send_fragsize_probe(int fd, int fragsize)
{
	char probedata[256];
	char buf[4096];

	/* build a large query domain which is random and maximum size */
	memset(probedata, MIN(1, rand_seed & 0xff), sizeof(probedata));
	probedata[1] = MIN(1, (rand_seed >> 8) & 0xff);
	rand_seed++;
	build_hostname(buf + 4, sizeof(buf) - 4, probedata, sizeof(probedata), topdomain, dataenc);

	fragsize &= 2047;

	buf[0] = 'r'; /* Probe downstream fragsize packet */
	buf[1] = b32_5to8((userid << 1) | (fragsize & 1024));
	buf[2] = b32_5to8((fragsize >> 5) & 31);
	buf[3] = b32_5to8(fragsize & 31);

	send_query(fd, buf);
}

static void
send_set_downstream_fragsize(int fd, int fragsize)
{
	char data[5];
	
	data[0] = userid;
	data[1] = (fragsize & 0xff00) >> 8;
	data[2] = (fragsize & 0x00ff);
	data[3] = (rand_seed >> 8) & 0xff;
	data[4] = (rand_seed >> 0) & 0xff;
	
	rand_seed++;

	send_packet(fd, 'N', data, sizeof(data));
}

static void 
send_version(int fd, uint32_t version)
{
	char data[6];

	data[0] = (version >> 24) & 0xff;
	data[1] = (version >> 16) & 0xff;
	data[2] = (version >> 8) & 0xff;
	data[3] = (version >> 0) & 0xff;

	data[4] = (rand_seed >> 8) & 0xff;
	data[5] = (rand_seed >> 0) & 0xff;
	
	rand_seed++;

	send_packet(fd, 'V', data, sizeof(data));
}

static void
send_case_check(int fd)
{
	/* The '+' plus character is not allowed according to RFC. 
	 * Expect to get SERVFAIL or similar if it is rejected.
	 */
	char buf[512] = "zZ+-aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyY1234.";

	strncat(buf, topdomain, 512 - strlen(buf));
	send_query(fd, buf);
}

static void
send_codec_switch(int fd, int userid, int bits)
{
	char buf[512] = "S__.";
	buf[1] = b32_5to8(userid);
	buf[2] = b32_5to8(bits);
	
	strncat(buf, topdomain, 512 - strlen(buf));
	send_query(fd, buf);
}

static int
handshake(int dns_fd)
{
	struct timeval tv;
	uint32_t payload;
	char server[65];
	char client[65];
	char login[16];
	char in[4096];
	fd_set fds;
	int read;
	int mtu;
	int seed;
	int i;
	int r;

	for (i = 0; running && i < 5; i++) {
		tv.tv_sec = i + 1;
		tv.tv_usec = 0;

		send_version(dns_fd, VERSION);
		
		FD_ZERO(&fds);
		FD_SET(dns_fd, &fds);

		r = select(dns_fd + 1, &fds, NULL, NULL, &tv);

		if(r > 0) {
			read = read_dns(dns_fd, in, sizeof(in));
			
			if(read <= 0) {
				if (read == 0) {
					warn("handshake read");
				}
				/* if read < 0 then warning has been printed already */
				continue;
			}

			if (read >= 9) {
				payload =  (((in[4] & 0xff) << 24) |
						((in[5] & 0xff) << 16) |
						((in[6] & 0xff) << 8) |
						((in[7] & 0xff)));

				if (strncmp("VACK", in, 4) == 0) {
					seed = payload;
					userid = in[8];

					printf("Version ok, both using protocol v 0x%08x. You are user #%d\n", VERSION, userid);
					goto perform_login;
				} else if (strncmp("VNAK", in, 4) == 0) {
					errx(1, "You use protocol v 0x%08x, server uses v 0x%08x. Giving up", 
							VERSION, payload);
					/* NOTREACHED */
				} else if (strncmp("VFUL", in, 4) == 0) {
					errx(1, "Server full, all %d slots are taken. Try again later", payload);
					/* NOTREACHED */
				}
			} else 
				warnx("did not receive proper login challenge");
		}
		
		printf("Retrying version check...\n");
	}
	errx(1, "couldn't connect to server");
	/* NOTREACHED */
	
perform_login:
	login_calculate(login, 16, password, seed);
	
	for (i=0; running && i<5 ;i++) {
		tv.tv_sec = i + 1;
		tv.tv_usec = 0;

		send_login(dns_fd, login, 16);
		
		FD_ZERO(&fds);
		FD_SET(dns_fd, &fds);

		r = select(dns_fd + 1, &fds, NULL, NULL, &tv);

		if(r > 0) {
			read = read_dns(dns_fd, in, sizeof(in));
			
			if(read <= 0) {
				warn("read");
				continue;
			}

			if (read > 0) {
				int netmask;
				if (strncmp("LNAK", in, 4) == 0) {
					printf("Bad password\n");
					return 1;
				} else if (sscanf(in, "%64[^-]-%64[^-]-%d-%d", 
					server, client, &mtu, &netmask) == 4) {
					
					server[64] = 0;
					client[64] = 0;
					if (tun_setip(client, netmask) == 0 && 
						tun_setmtu(mtu) == 0) {
						goto perform_case_check;
					} else {
						warnx("Received handshake with bad data");
					}
				} else {
					printf("Received bad handshake\n");
				}
			}
		}

		printf("Retrying login...\n");
	}
	errx(1, "couldn't login to server");
	/* NOTREACHED */

perform_case_check:
	case_preserved = 0;
	for (i=0; running && i<5 ;i++) {
		tv.tv_sec = i + 1;
		tv.tv_usec = 0;

		send_case_check(dns_fd);
		
		FD_ZERO(&fds);
		FD_SET(dns_fd, &fds);

		r = select(dns_fd + 1, &fds, NULL, NULL, &tv);

		if(r > 0) {
			read = read_dns(dns_fd, in, sizeof(in));
			
			if (read > 0) {
				if (in[0] == 'z' || in[0] == 'Z') {
					if (read < (27 * 2)) {
						printf("Received short case check reply. Will use base32 encoder\n");
						goto switch_codec;
					} else {
						int k;

						/* TODO enhance this, base128 is probably also possible */
						case_preserved = 1;
						for (k = 0; k < 27 && case_preserved; k += 2) {
							if (in[k] == in[k+1]) {
								/* test string: zZ+-aAbBcCdDeE... */
								case_preserved = 0;
							}
						}
						goto switch_codec;
					}
				} else {
					printf("Received bad case check reply\n");
				}
			} else {
				printf("Got error on case check, will use base32\n");
				goto switch_codec;
			}
		}

		printf("Retrying case check...\n");
	}

	printf("No reply on case check, continuing\n");
switch_codec:
	if (!case_preserved)
		goto autodetect_max_fragsize;

	dataenc = get_base64_encoder();
	printf("Switching to %s codec\n", dataenc->name);
	/* Send to server that this user will use base64 from now on */
	for (i=0; running && i<5 ;i++) {
		int bits;
		tv.tv_sec = i + 1;
		tv.tv_usec = 0;

		bits = 6; /* base64 = 6 bits per byte */

		send_codec_switch(dns_fd, userid, bits);
		
		FD_ZERO(&fds);
		FD_SET(dns_fd, &fds);

		r = select(dns_fd + 1, &fds, NULL, NULL, &tv);

		if(r > 0) {
			read = read_dns(dns_fd, in, sizeof(in));
			
			if (read > 0) {
				if (strncmp("BADLEN", in, 6) == 0) {
					printf("Server got bad message length. ");
					goto codec_revert;
				} else if (strncmp("BADIP", in, 5) == 0) {
					printf("Server rejected sender IP address. ");
					goto codec_revert;
				} else if (strncmp("BADCODEC", in, 8) == 0) {
					printf("Server rejected the selected codec. ");
					goto codec_revert;
				}
				in[read] = 0; /* zero terminate */
				printf("Server switched to codec %s\n", in);
				goto autodetect_max_fragsize;
			}
		}
		printf("Retrying codec switch...\n");
	}
	printf("No reply from server on codec switch. ");
codec_revert: 
	printf("Falling back to base32\n");
	dataenc = get_base32_encoder();
autodetect_max_fragsize:
	if (autodetect_frag_size) {
		int proposed_fragsize = 768;
		int range = 768;
		max_downstream_frag_size = 0;
		printf("Autoprobing max downstream fragment size... (skip with -m fragsize)\n"); 
		while (range >= 8 || !max_downstream_frag_size) {
			for (i=0; running && i<3 ;i++) {
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				send_fragsize_probe(dns_fd, proposed_fragsize);

				FD_ZERO(&fds);
				FD_SET(dns_fd, &fds);

				r = select(dns_fd + 1, &fds, NULL, NULL, &tv);

				if(r > 0) {
					read = read_dns(dns_fd, in, sizeof(in));
					
					if (read > 0) {
						/* We got a reply */
						int acked_fragsize = ((in[0] & 0xff) << 8) | (in[1] & 0xff);
						if (acked_fragsize == proposed_fragsize) {
							printf("%d ok.. ", acked_fragsize);
							fflush(stdout);
							max_downstream_frag_size = acked_fragsize;
							range >>= 1;
							proposed_fragsize += range;
							continue;
						}
					}
				}
			}
			printf("%d not ok.. ", proposed_fragsize);
			fflush(stdout);
			range >>= 1;
			proposed_fragsize -= range;
		}
		printf("will use %d\n", max_downstream_frag_size);
	}
	printf("Setting downstream fragment size to max %d...\n", max_downstream_frag_size);
	for (i=0; running && i<5 ;i++) {
		tv.tv_sec = i + 1;
		tv.tv_usec = 0;

		send_set_downstream_fragsize(dns_fd, max_downstream_frag_size);
		
		FD_ZERO(&fds);
		FD_SET(dns_fd, &fds);

		r = select(dns_fd + 1, &fds, NULL, NULL, &tv);

		if(r > 0) {
			read = read_dns(dns_fd, in, sizeof(in));
			
			if (read > 0) {
				int accepted_fragsize;

				if (strncmp("BADFRAG", in, 7) == 0) {
					printf("Server rejected fragsize. Keeping default.");
					goto done;
				} else if (strncmp("BADIP", in, 5) == 0) {
					printf("Server rejected sender IP address.\n");
					goto done;
				}

				accepted_fragsize = ((in[0] & 0xff) << 8) | (in[1] & 0xff);
				goto done;
			}
		}
		printf("Retrying set fragsize...\n");
	}
	printf("No reply from server when setting fragsize. Keeping default.\n");
done:
	return 0;
}

static char *
get_resolvconf_addr()
{
	static char addr[16];
	char buf[80];
	char *rv;
	FILE *fp;
	
	rv = NULL;

	if ((fp = fopen("/etc/resolv.conf", "r")) == NULL) 
		err(1, "/etc/resolve.conf");
	
	while (feof(fp) == 0) {
		fgets(buf, sizeof(buf), fp);

		if (sscanf(buf, "nameserver %15s", addr) == 1) {
			rv = addr;
			break;
		}
	}
	
	fclose(fp);

	return rv;
}

static void
set_nameserver(const char *cp) 
{
	struct in_addr addr;

	if (inet_aton(cp, &addr) != 1)
		errx(1, "error parsing nameserver address: '%s'", cp);

	memset(&nameserv, 0, sizeof(nameserv));
	nameserv.sin_family = AF_INET;
	nameserv.sin_port = htons(53);
	nameserv.sin_addr = addr;
}

static void
usage() {
	extern char *__progname;

	printf("Usage: %s [-v] [-h] [-f] [-u user] [-t chrootdir] [-d device] "
			"[-P password] [-m maxfragsize] [nameserver] topdomain\n", __progname);
	exit(2);
}

static void
help() {
	extern char *__progname;

	printf("iodine IP over DNS tunneling client\n");
	printf("Usage: %s [-v] [-h] [-f] [-u user] [-t chrootdir] [-d device] "
			"[-P password] [-m maxfragsize] [nameserver] topdomain\n", __progname);
	printf("  -v to print version info and exit\n");
	printf("  -h to print this help and exit\n");
	printf("  -f to keep running in foreground\n");
	printf("  -u name to drop privileges and run as user 'name'\n");
	printf("  -t dir to chroot to directory dir\n");
	printf("  -d device to set tunnel device name\n");
	printf("  -P password used for authentication (max 32 chars will be used)\n");
	printf("  -m maxfragsize, to limit size of downstream packets\n");
	printf("nameserver is the IP number of the relaying nameserver, if absent /etc/resolv.conf is used\n");
	printf("topdomain is the FQDN that is delegated to the tunnel endpoint.\n");

	exit(0);
}

static void
version() {
	printf("iodine IP over DNS tunneling client\n");
	printf("version: 0.5.0-rc1 from 2009-01-10\n");

	exit(0);
}

int
main(int argc, char **argv)
{
	char *nameserv_addr;
	struct passwd *pw;
	char *username;
	int foreground;
	char *newroot;
	char *device;
	int choice;
	int tun_fd;
	int dns_fd;

	memset(password, 0, 33);
	username = NULL;
	foreground = 0;
	newroot = NULL;
	device = NULL;
	chunkid = 0;

	outpkt.seqno = 0;
	inpkt.len = 0;

	autodetect_frag_size = 1;
	max_downstream_frag_size = 3072;

	b32 = get_base32_encoder();
	dataenc = get_base32_encoder();
	
#if !defined(BSD) && !defined(__GLIBC__)
	__progname = strrchr(argv[0], '/');
	if (__progname == NULL)
		__progname = argv[0];
	else
		__progname++;
#endif

	while ((choice = getopt(argc, argv, "vfhu:t:d:P:m:")) != -1) {
		switch(choice) {
		case 'v':
			version();
			/* NOTREACHED */
			break;
		case 'f':
			foreground = 1;
			break;
		case 'h':
			help();
			/* NOTREACHED */
			break;
		case 'u':
			username = optarg;
			break;
		case 't':
			newroot = optarg;
			break;
		case 'd':
			device = optarg;
			break;
		case 'P':
			strncpy(password, optarg, sizeof(password));
			password[sizeof(password)-1] = 0;
			
			/* XXX: find better way of cleaning up ps(1) */
			memset(optarg, 0, strlen(optarg)); 
			break;
		case 'm':
			autodetect_frag_size = 0;
			max_downstream_frag_size = atoi(optarg);
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}
	
	if (geteuid() != 0) {
		warnx("Run as root and you'll be happy.\n");
		usage();
		/* NOTREACHED */
	}

	argc -= optind;
	argv += optind;

	switch (argc) {
	case 1:
		nameserv_addr = get_resolvconf_addr();
		topdomain = strdup(argv[0]);
		break;
	case 2:
		nameserv_addr = argv[0];
		topdomain = strdup(argv[1]);
		break;
	default:
		usage();
		/* NOTREACHED */
	}

	if (max_downstream_frag_size < 1 || max_downstream_frag_size > 0xffff) {
		warnx("Use a max frag size between 1 and 65535 bytes.\n");
		usage();
		/* NOTREACHED */
	}

	set_nameserver(nameserv_addr);

	if(strlen(topdomain) <= 128) {
		if(check_topdomain(topdomain)) {
			warnx("Topdomain contains invalid characters.\n");
			usage();
			/* NOTREACHED */
		}
	} else {
		warnx("Use a topdomain max 128 chars long.\n");
		usage();
		/* NOTREACHED */
	}

	if (username != NULL) {
		if ((pw = getpwnam(username)) == NULL) {
			warnx("User %s does not exist!\n", username);
			usage();
			/* NOTREACHED */
		}
	}
	
	if (strlen(password) == 0) 
		read_password(password, sizeof(password));

	if ((tun_fd = open_tun(device)) == -1)
		goto cleanup1;
	if ((dns_fd = open_dns(0, INADDR_ANY)) == -1)
		goto cleanup2;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	if(handshake(dns_fd))
		goto cleanup2;
	
	printf("Sending queries for %s to %s\n", topdomain, nameserv_addr);

	if (foreground == 0) 
		do_detach();

	if (newroot != NULL)
		do_chroot(newroot);
	
	if (username != NULL) {
		gid_t gids[1];
		gids[0] = pw->pw_gid;
		if (setgroups(1, gids) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
			warnx("Could not switch to user %s!\n", username);
			usage();
			/* NOTREACHED */
		}
	}
	
	downstream_seqno = 0;
	downstream_fragment = 0;
	down_ack_seqno = 0;
	down_ack_fragment = 0;
	
	tunnel(tun_fd, dns_fd);

cleanup2:
	close_dns(dns_fd);
	close_tun(tun_fd);
cleanup1:

	return 0;
}
