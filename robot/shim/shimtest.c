// On-robot test for libmbotmic.so (run with LD_PRELOAD). Sends a fake mic packet through a
// socketpair, recv()s it, and checks: (a) a copy arrived on the listener, (b) silence mode zeroes it.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int main(void) {
    int listen_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/dev/socket/mbot_rawmic"); unlink(a.sun_path);
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    struct timeval tv = {1, 0}; setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int sp[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
    unsigned char pkt[1293], got[2048], copy[2048];
    memset(pkt, 0x11, sizeof pkt); pkt[0] = 0xf2;
    send(sp[0], pkt, sizeof pkt, 0);
    ssize_t n = recv(sp[1], got, sizeof got, 0);             // goes through the shim
    ssize_t c = recvfrom(listen_fd, copy, sizeof copy, 0, 0, 0);  // forwarded copy (recvfrom: not hooked)
    int fwd_ok = (c == 1293 && copy[0] == 0xf2 && copy[5] == 0x11);
    int zeroed = (got[1] == 0 && got[1280] == 0 && got[1281] == 0x11);
    int intact = (got[1] == 0x11);
    printf("recv=%zd forwarded=%zd fwd_ok=%d samples_zeroed=%d samples_intact=%d\n",
           n, c, fwd_ok, zeroed, intact);
    unlink(a.sun_path);
    return 0;
}
