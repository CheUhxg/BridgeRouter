int init_xfrm_socket();
int send_xfrm_add_sa(int xfrm_socket, int keylen, void *keybuf);
int send_xfrm_get_sa(int xfrm_socket);