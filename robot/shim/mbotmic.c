// libmbotmic.so - LD_PRELOAD shim for Vector's vic-anim.
//
// vic-robot forwards the body board's raw 4-microphone audio to vic-anim as datagrams on
// /dev/socket/_anim_robot_client_0, read with recv(). Each mic packet is 1293 bytes:
//   [0]        CLAD tag 0xf2 (MicData)
//   [1..1280]  int16 LE samples, PLANAR: ch0[160] ch1[160] ch2[160] ch3[160] @ 15625 Hz
//   [1281..]   u32 timestamp_ms, u32 status flags, f32 head angle
// This shim (1) copies every mic packet to /dev/socket/mbot_rawmic (mictap streams it to MBot as
// constant audio), and (2) while /data/mbot/mic_silence exists, zeroes the samples before vic-anim
// sees them - the firmware then never hears "Hey Vector", so it never does its wake reaction.
// (3) From the RobotState packets on the same socket it keeps the LIVE battery voltage and writes it to
// /run/mbot_batt once a second. The firmware's own (SDK) battery voltage is a filter that only updates
// while syscon says the battery is "connected", and it has been seen frozen for a whole 50-minute run.
// It must never block or crash vic-anim: non-blocking sends, no allocation, no locks.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MIC_TAG      0xf2
#define MIC_PKT_LEN  1293
#define MIC_DATA_LEN 1280
#define OUT_PATH     "/dev/socket/mbot_rawmic"
#define SILENCE_FLAG "/data/mbot/mic_silence"
#define PKTS_PER_SEC 98
// RobotState (robot/clad robotStatusAndActions.clad), after the 1-byte CLAD tag:
#define STATE_TAG     0xf0
#define STATE_LEN     219
#define ST_BATT_OFF   173   // float batteryVoltage
#define ST_CHGR_OFF   177   // float chargerVoltage
#define ST_STATUS_OFF 181   // u32 RobotStatusFlag
#define ST_TEMP_OFF   217   // u8 battTemp_C
#define BATT_PATH     "/run/mbot_batt"

typedef ssize_t (*recv_fn)(int, void *, size_t, int);
static recv_fn real_recv;
static int out_fd = -1;
static struct sockaddr_un out_addr;
static unsigned pkt_count;
static unsigned send_backoff;   // packets to skip after a failed send (no listener)
static int silence;
static float batt_avg, batt_min_s, batt_last, chgr_last;  // avg: ~10 s EMA; min_s: lowest this second (sag under load)
static unsigned st_status, st_temp, st_n;
static time_t batt_written;

static void note_state(const unsigned char *b) {
    float v, c;
    unsigned st;
    memcpy(&v, b + ST_BATT_OFF, 4);
    memcpy(&c, b + ST_CHGR_OFF, 4);
    memcpy(&st, b + ST_STATUS_OFF, 4);
    if (!(v > 2.0f && v < 5.0f)) return;
    batt_last = v; chgr_last = c; st_status = st; st_temp = b[ST_TEMP_OFF];
    batt_avg = batt_avg > 0 ? batt_avg + (v - batt_avg) / 64.f : v;
    if (st_n++ == 0 || v < batt_min_s) batt_min_s = v;
    time_t now = time(0);
    if (now == batt_written) return;
    batt_written = now;
    char line[128];
    int n = snprintf(line, sizeof line, "%ld %.4f %.4f %.4f %.4f 0x%x %u %u\n", (long)now, batt_avg, batt_last,
                     batt_min_s, chgr_last, st_status, st_temp, st_n);
    st_n = 0;
    int fd = open(BATT_PATH ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NONBLOCK, 0644);
    if (fd < 0) return;
    if (n > 0 && write(fd, line, (size_t)n) == n) { close(fd); rename(BATT_PATH ".tmp", BATT_PATH); }
    else close(fd);
}

static void init_out(void) {
    out_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    memset(&out_addr, 0, sizeof out_addr);
    out_addr.sun_family = AF_UNIX;
    strncpy(out_addr.sun_path, OUT_PATH, sizeof out_addr.sun_path - 1);
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    if (!real_recv) real_recv = (recv_fn)dlsym(RTLD_NEXT, "recv");
    ssize_t n = real_recv ? real_recv(fd, buf, len, flags)
                          : (ssize_t)syscall(SYS_recvfrom, fd, buf, len, flags, 0, 0);
    if (n == STATE_LEN && buf && ((unsigned char *)buf)[0] == STATE_TAG) {
        int saved = errno;
        note_state((const unsigned char *)buf);
        errno = saved;
    } else if (n == MIC_PKT_LEN && buf && ((unsigned char *)buf)[0] == MIC_TAG) {
        int saved = errno;
        if (out_fd < 0) init_out();
        if (send_backoff) {
            send_backoff--;
        } else if (out_fd >= 0 &&
                   sendto(out_fd, buf, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL,
                          (struct sockaddr *)&out_addr, sizeof out_addr) < 0 &&
                   errno != EAGAIN) {
            send_backoff = PKTS_PER_SEC;  // nobody listening: retry in ~1 s
        }
        if (pkt_count++ % PKTS_PER_SEC == 0) silence = access(SILENCE_FLAG, F_OK) == 0;
        if (silence) memset((unsigned char *)buf + 1, 0, MIC_DATA_LEN);
        errno = saved;
    }
    return n;
}
