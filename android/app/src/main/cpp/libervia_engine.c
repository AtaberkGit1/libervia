#define _POSIX_C_SOURCE 200809L

#include <jni.h>
#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "LiberViaNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_TCP_SESSIONS 1024
#define MAX_UDP_SESSIONS 1024
#define MAX_EPOLL_EVENTS 64
#define TUN_BUF_SIZE 4096
#define OUT_BUF_SIZE 16384
#define SESSION_IDLE_TIMEOUT_SEC 60
#define UDP_IDLE_TIMEOUT_SEC 60
#define MTU_PAYLOAD_CHUNK 1360

typedef enum {
    TAG_TUN = 1,
    TAG_EVENT = 2,
    TAG_TIMER = 3,
    TAG_TCP = 4,
    TAG_UDP = 5
} epoll_tag_t;

typedef struct {
    epoll_tag_t tag;
    int index;
} epoll_ctx_t;

typedef enum {
    TCP_STATE_CONNECTING = 1,
    TCP_STATE_ESTABLISHED = 2,
    TCP_STATE_FIN_WAIT = 3
} tcp_state_t;

typedef struct {
    tcp_state_t state;
    int sock_fd;
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint32_t client_seq;
    uint32_t server_seq;
    uint16_t client_win;
    bool dpi_applied;
    bool client_fin;
    bool server_fin;
    bool epoll_want_out;
    uint8_t out_buf[OUT_BUF_SIZE];
    size_t out_len;
    time_t last_active;
} tcp_session_t;

typedef struct {
    int sock_fd;
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint32_t forward_ip;
    time_t last_active;
    bool is_dns;
} udp_session_t;

typedef struct {
    int split_bytes;
    int disorder;
    int fake_ttl;
} engine_cfg_t;

static JavaVM *g_jvm = NULL;
static pthread_mutex_t g_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker_thread;
static bool g_thread_started = false;
static atomic_bool g_running = false;

static int g_tun_fd = -1;
static int g_epoll_fd = -1;
static int g_event_fd = -1;
static int g_timer_fd = -1;
static jobject g_service_ref = NULL;
static jmethodID g_protect_mid = NULL;

static uint32_t g_dns_ip_be = 0;
static engine_cfg_t g_cfg = {1, 1, 3};

static atomic_uint_fast64_t g_rx_bytes = 0;
static atomic_uint_fast64_t g_tx_bytes = 0;

static tcp_session_t *g_tcp[MAX_TCP_SESSIONS];
static udp_session_t *g_udp[MAX_UDP_SESSIONS];
static int g_active_sessions = 0;
static bool g_timer_armed = false;

static epoll_ctx_t g_tun_ctx = {TAG_TUN, 0};
static epoll_ctx_t g_evt_ctx = {TAG_EVENT, 0};
static epoll_ctx_t g_tmr_ctx = {TAG_TIMER, 0};
static epoll_ctx_t g_tcp_ctxs[MAX_TCP_SESSIONS];
static epoll_ctx_t g_udp_ctxs[MAX_UDP_SESSIONS];

static time_t now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void micro_pause(long usec) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = usec * 1000L;
    nanosleep(&ts, NULL);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void sync_idle_timer(void) {
    if (g_timer_fd < 0) return;
    bool should_arm = (g_active_sessions > 0);
    if (should_arm == g_timer_armed) return;

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (should_arm) {
        its.it_value.tv_sec = 60;
        its.it_interval.tv_sec = 60;
    }
    if (timerfd_settime(g_timer_fd, 0, &its, NULL) == 0) {
        g_timer_armed = should_arm;
    }
}

static uint16_t csum_add(uint32_t sum, const uint8_t *data, size_t len) {
    while (len > 1) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += ((uint32_t)data[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFU);
}

static uint16_t ip_checksum(const uint8_t *hdr, size_t hdr_len) {
    return csum_add(0, hdr, hdr_len);
}

static uint16_t transport_checksum(uint32_t src_ip_be, uint32_t dst_ip_be,
                                   uint8_t proto, const uint8_t *seg, size_t seg_len) {
    uint32_t sum = 0;
    uint32_t s = ntohl(src_ip_be);
    uint32_t d = ntohl(dst_ip_be);
    sum += (s >> 16) & 0xFFFFU;
    sum += s & 0xFFFFU;
    sum += (d >> 16) & 0xFFFFU;
    sum += d & 0xFFFFU;
    sum += (uint32_t)proto;
    sum += (uint32_t)seg_len;
    return csum_add(sum, seg, seg_len);
}

static bool protect_fd(JNIEnv *env, int fd) {
    if (!env || !g_service_ref || !g_protect_mid || fd < 0) return false;
    jboolean ok = (*env)->CallBooleanMethod(env, g_service_ref, g_protect_mid, (jint)fd);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return false;
    }
    return ok == JNI_TRUE;
}

static void parse_flags(const char *flags_str, engine_cfg_t *cfg) {
    cfg->split_bytes = 1;
    cfg->disorder = 1;
    cfg->fake_ttl = 3;
    if (!flags_str || !*flags_str) return;
    cfg->disorder = 0;
    cfg->fake_ttl = 0;

    char *copy = strdup(flags_str);
    if (!copy) return;

    char *saveptr = NULL;
    char *tok = strtok_r(copy, " \t\r\n", &saveptr);
    while (tok) {
        if (strcmp(tok, "-s") == 0) {
            char *arg = strtok_r(NULL, " \t\r\n", &saveptr);
            if (arg) {
                int v = atoi(arg);
                if (v < 1) v = 1;
                if (v > 256) v = 256;
                cfg->split_bytes = v;
            }
        } else if (strcmp(tok, "-d") == 0) {
            cfg->disorder = 1;
            char *peek = strtok_r(NULL, " \t\r\n", &saveptr);
            if (peek) {
                if (peek[0] >= '0' && peek[0] <= '9') {
                    cfg->disorder = atoi(peek) > 0 ? 1 : 0;
                } else {
                    tok = peek;
                    continue;
                }
            }
        } else if (strcmp(tok, "-t") == 0) {
            char *arg = strtok_r(NULL, " \t\r\n", &saveptr);
            if (arg) {
                int v = atoi(arg);
                if (v < 0) v = 0;
                if (v > 64) v = 64;
                cfg->fake_ttl = v;
            }
        }
        tok = strtok_r(NULL, " \t\r\n", &saveptr);
    }
    free(copy);
}

static int find_tls_sni_offset(const uint8_t *buf, size_t len) {
    if (len < 44) return -1;
    if (buf[0] != 0x16 || buf[1] != 0x03) return -1;
    if (buf[5] != 0x01) return -1;

    size_t pos = 43;
    if (pos + 1 > len) return -1;
    uint8_t sess_len = buf[pos];
    pos += 1 + sess_len;

    if (pos + 2 > len) return -1;
    uint16_t cs_len = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
    pos += 2 + cs_len;

    if (pos + 1 > len) return -1;
    uint8_t comp_len = buf[pos];
    pos += 1 + comp_len;

    if (pos + 2 > len) return -1;
    uint16_t exts_len = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
    pos += 2;

    size_t exts_end = pos + exts_len;
    if (exts_end > len) exts_end = len;

    while (pos + 4 <= exts_end) {
        uint16_t ext_type = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        uint16_t ext_len = ((uint16_t)buf[pos + 2] << 8) | buf[pos + 3];
        pos += 4;
        if (pos + ext_len > exts_end) break;
        if (ext_type == 0x0000 && ext_len >= 5) {
            if (buf[pos + 2] == 0x00) {
                return (int)(pos + 5);
            }
        }
        pos += ext_len;
    }
    return -1;
}

static bool is_http_payload(const uint8_t *buf, size_t len) {
    if (len < 12) return false;
    static const char *methods[] = {
        "GET ", "POST ", "HEAD ", "PUT ", "DELETE ", "OPTIONS ", "CONNECT ", "PATCH "
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        size_t mlen = strlen(methods[i]);
        if (len > mlen && memcmp(buf, methods[i], mlen) == 0) {
            return true;
        }
    }
    return false;
}

static size_t apply_http_host_mutation(uint8_t *buf, size_t len) {
    size_t split_at = 0;
    for (size_t i = 0; i + 7 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            if (split_at == 0 && i > 4) {
                split_at = i;
            }
            if ((buf[i + 2] == 'H' || buf[i + 2] == 'h') &&
                (buf[i + 3] == 'O' || buf[i + 3] == 'o') &&
                (buf[i + 4] == 'S' || buf[i + 4] == 's') &&
                (buf[i + 5] == 'T' || buf[i + 5] == 't') &&
                buf[i + 6] == ':') {
                buf[i + 2] = 'h';
                buf[i + 3] = 'O';
                buf[i + 4] = 's';
                buf[i + 5] = 'T';
                if (i + 8 < len) {
                    split_at = i + 8;
                }
                break;
            }
        }
    }
    return split_at;
}

static bool write_tun_packet(const uint8_t *pkt, size_t len) {
    if (g_tun_fd < 0 || len == 0) return false;
    ssize_t w = write(g_tun_fd, pkt, len);
    return w == (ssize_t)len;
}

static void send_ipv6_rst_to_tun(const uint8_t *pkt, size_t pkt_len) {
    if (pkt_len < 40 + 20) return;
    const uint8_t *tcp = pkt + 40;
    uint8_t flags = tcp[13];
    if (flags & 0x04 /* RST */) return;

    uint8_t rst_pkt[60];
    memset(rst_pkt, 0, sizeof(rst_pkt));

    rst_pkt[0] = 0x60; /* Version 6 */
    rst_pkt[4] = 0x00; /* Payload length: 20 bytes */
    rst_pkt[5] = 0x14;
    rst_pkt[6] = IPPROTO_TCP; /* Next header: TCP */
    rst_pkt[7] = 64;          /* Hop limit */

    /* Swap Source and Destination IPv6 addresses */
    memcpy(rst_pkt + 8, pkt + 24, 16);
    memcpy(rst_pkt + 24, pkt + 8, 16);

    uint8_t *rst_tcp = rst_pkt + 40;
    memcpy(rst_tcp + 0, tcp + 2, 2); /* DstPort -> SrcPort */
    memcpy(rst_tcp + 2, tcp + 0, 2); /* SrcPort -> DstPort */

    uint32_t in_seq;
    memcpy(&in_seq, tcp + 4, 4);
    uint32_t ack_num = ntohl(in_seq) + 1;
    uint32_t ack_be = htonl(ack_num);
    memcpy(rst_tcp + 8, &ack_be, 4);

    rst_tcp[12] = 0x50; /* Data offset: 5 (20 bytes) */
    rst_tcp[13] = 0x14; /* RST | ACK */

    uint32_t sum = 0;
    for (size_t i = 8; i < 40; i += 2) {
        sum += ((uint32_t)rst_pkt[i] << 8) | rst_pkt[i + 1];
    }
    sum += 20;
    sum += IPPROTO_TCP;

    uint16_t csum = csum_add(sum, rst_tcp, 20);
    rst_tcp[16] = (uint8_t)(csum >> 8);
    rst_tcp[17] = (uint8_t)(csum & 0xFF);

    write_tun_packet(rst_pkt, sizeof(rst_pkt));
}

static void send_tcp_packet_to_tun(const tcp_session_t *s, uint8_t tcp_flags,
                                   const uint8_t *payload, size_t payload_len,
                                   bool include_mss_opt) {
    uint8_t pkt[TUN_BUF_SIZE];
    size_t tcp_hdr_len = include_mss_opt ? 24 : 20;
    size_t total_len = 20 + tcp_hdr_len + payload_len;
    if (total_len > sizeof(pkt)) return;

    memset(pkt, 0, 20 + tcp_hdr_len);

    pkt[0] = 0x45;
    pkt[1] = 0x00;
    pkt[2] = (uint8_t)((total_len >> 8) & 0xFF);
    pkt[3] = (uint8_t)(total_len & 0xFF);
    uint16_t ip_id = (uint16_t)(s->server_seq & 0xFFFF);
    pkt[4] = (uint8_t)(ip_id >> 8);
    pkt[5] = (uint8_t)(ip_id & 0xFF);
    pkt[6] = 0x40; /* Don't Fragment */
    pkt[7] = 0x00;
    pkt[8] = 64;   /* TTL */
    pkt[9] = IPPROTO_TCP;
    memcpy(pkt + 12, &s->dst_ip, 4);
    memcpy(pkt + 16, &s->src_ip, 4);
    uint16_t ip_csum = ip_checksum(pkt, 20);
    pkt[10] = (uint8_t)(ip_csum >> 8);
    pkt[11] = (uint8_t)(ip_csum & 0xFF);

    uint8_t *tcp = pkt + 20;
    memcpy(tcp + 0, &s->dst_port, 2);
    memcpy(tcp + 2, &s->src_port, 2);
    uint32_t seq_be = htonl(s->server_seq);
    uint32_t ack_be = htonl(s->client_seq);
    memcpy(tcp + 4, &seq_be, 4);
    memcpy(tcp + 8, &ack_be, 4);
    tcp[12] = (uint8_t)((tcp_hdr_len / 4) << 4);
    tcp[13] = tcp_flags;
    uint16_t win_be = htons(65535);
    memcpy(tcp + 14, &win_be, 2);

    if (include_mss_opt) {
        tcp[20] = 2; /* Kind: MSS */
        tcp[21] = 4; /* Length */
        tcp[22] = (MTU_PAYLOAD_CHUNK >> 8) & 0xFF;
        tcp[23] = MTU_PAYLOAD_CHUNK & 0xFF;
    }

    if (payload_len > 0 && payload) {
        memcpy(pkt + 20 + tcp_hdr_len, payload, payload_len);
    }

    uint16_t t_csum = transport_checksum(s->dst_ip, s->src_ip, IPPROTO_TCP,
                                         tcp, tcp_hdr_len + payload_len);
    tcp[16] = (uint8_t)(t_csum >> 8);
    tcp[17] = (uint8_t)(t_csum & 0xFF);

    write_tun_packet(pkt, total_len);
}

static void send_rst_to_tun(uint32_t src_ip_be, uint16_t src_port_be,
                            uint32_t dst_ip_be, uint16_t dst_port_be,
                            uint32_t ack_num) {
    tcp_session_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.src_ip = src_ip_be;
    tmp.src_port = src_port_be;
    tmp.dst_ip = dst_ip_be;
    tmp.dst_port = dst_port_be;
    tmp.server_seq = 0;
    tmp.client_seq = ack_num;
    send_tcp_packet_to_tun(&tmp, 0x14 /* RST|ACK */, NULL, 0, false);
}

static void send_udp_packet_to_tun(const udp_session_t *u, const uint8_t *payload, size_t payload_len) {
    uint8_t pkt[TUN_BUF_SIZE];
    size_t udp_len = 8 + payload_len;
    size_t total_len = 20 + udp_len;
    if (total_len > sizeof(pkt)) return;

    memset(pkt, 0, 28);
    pkt[0] = 0x45;
    pkt[2] = (uint8_t)((total_len >> 8) & 0xFF);
    pkt[3] = (uint8_t)(total_len & 0xFF);
    pkt[6] = 0x40;
    pkt[8] = 64;
    pkt[9] = IPPROTO_UDP;
    memcpy(pkt + 12, &u->dst_ip, 4);
    memcpy(pkt + 16, &u->src_ip, 4);
    uint16_t ip_csum = ip_checksum(pkt, 20);
    pkt[10] = (uint8_t)(ip_csum >> 8);
    pkt[11] = (uint8_t)(ip_csum & 0xFF);

    uint8_t *udp = pkt + 20;
    memcpy(udp + 0, &u->dst_port, 2);
    memcpy(udp + 2, &u->src_port, 2);
    udp[4] = (uint8_t)((udp_len >> 8) & 0xFF);
    udp[5] = (uint8_t)(udp_len & 0xFF);
    if (payload_len > 0 && payload) {
        memcpy(udp + 8, payload, payload_len);
    }

    uint16_t u_csum = transport_checksum(u->dst_ip, u->src_ip, IPPROTO_UDP, udp, udp_len);
    if (u_csum == 0) u_csum = 0xFFFF;
    udp[6] = (uint8_t)(u_csum >> 8);
    udp[7] = (uint8_t)(u_csum & 0xFF);

    write_tun_packet(pkt, total_len);
}

static void update_tcp_epoll(int idx, tcp_session_t *s, bool want_out) {
    if (!s || s->sock_fd < 0 || g_epoll_fd < 0) return;
    if (s->epoll_want_out == want_out) return;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP | (want_out ? EPOLLOUT : 0);
    ev.data.ptr = &g_tcp_ctxs[idx];
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, s->sock_fd, &ev) == 0) {
        s->epoll_want_out = want_out;
    }
}

static void close_tcp_session(int idx) {
    if (idx < 0 || idx >= MAX_TCP_SESSIONS) return;
    tcp_session_t *s = g_tcp[idx];
    if (!s) return;

    if (s->sock_fd >= 0) {
        if (g_epoll_fd >= 0) {
            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, s->sock_fd, NULL);
        }
        close(s->sock_fd);
    }
    free(s);
    g_tcp[idx] = NULL;
    if (g_active_sessions > 0) g_active_sessions--;
    sync_idle_timer();
}

static void close_udp_session(int idx) {
    if (idx < 0 || idx >= MAX_UDP_SESSIONS) return;
    udp_session_t *u = g_udp[idx];
    if (!u) return;

    if (u->sock_fd >= 0) {
        if (g_epoll_fd >= 0) {
            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, u->sock_fd, NULL);
        }
        close(u->sock_fd);
    }
    free(u);
    g_udp[idx] = NULL;
    if (g_active_sessions > 0) g_active_sessions--;
    sync_idle_timer();
}

static int queue_tcp_outbound(int idx, tcp_session_t *s, const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    size_t sent = 0;
    if (s->out_len == 0) {
        ssize_t n = send(s->sock_fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            sent = (size_t)n;
            atomic_fetch_add(&g_tx_bytes, (uint64_t)n);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
    size_t rem = len - sent;
    if (rem > 0) {
        if (s->out_len + rem > OUT_BUF_SIZE) {
            return -1;
        }
        memcpy(s->out_buf + s->out_len, data + sent, rem);
        s->out_len += rem;
        update_tcp_epoll(idx, s, true);
    }
    return 0;
}

static int send_with_dpi_bypass(JNIEnv *env, int idx, tcp_session_t *s,
                                const uint8_t *payload, size_t len) {
    (void)env;
    if (len == 0) return 0;
    if (s->dpi_applied || len > TUN_BUF_SIZE) {
        return queue_tcp_outbound(idx, s, payload, len);
    }

    s->dpi_applied = true;
    uint8_t work[TUN_BUF_SIZE];
    memcpy(work, payload, len);

    /* TLS 443 ClientHello: multi-phase fragmentation */
    if (len >= 6 && work[0] == 0x16 && work[1] == 0x03 && work[5] == 0x01) {
        int sni_off = find_tls_sni_offset(work, len);
        size_t split_sni = 0;
        if (sni_off > 2 && (size_t)sni_off + 2 < len) {
            split_sni = (size_t)sni_off + 1; /* Split inside SNI hostname (after 1st char) */
        }

        /*
         * GoodbyeDPI-grade multi-phase fragmentation:
         * 1) First segment: 2 bytes (0x16, 0x03). Breaks the 5-byte TLS record header.
         *    DPI cannot identify TLS record type or length.
         * 2) Second segment: from byte 2 up to split_sni (or byte 2 to len/2 if no SNI).
         *    Cuts off right in the middle of the SNI hostname.
         * 3) Third segment: remaining payload.
         * Pauses of 3000 usec (3ms) force distinct physical segments on the wire.
         */
        size_t seg1_len = 2;
        size_t seg2_len = (split_sni > seg1_len) ? (split_sni - seg1_len) : (len > seg1_len ? (len - seg1_len) / 2 : 0);
        size_t seg3_len = len - seg1_len - seg2_len;

        if (queue_tcp_outbound(idx, s, work, seg1_len) < 0) return -1;
        micro_pause(3000);

        if (seg2_len > 0) {
            if (queue_tcp_outbound(idx, s, work + seg1_len, seg2_len) < 0) return -1;
            micro_pause(3000);
        }

        if (seg3_len > 0) {
            if (queue_tcp_outbound(idx, s, work + seg1_len + seg2_len, seg3_len) < 0) return -1;
        }

        return 0;
    } else if (is_http_payload(work, len)) {
        size_t split_at = apply_http_host_mutation(work, len);
        if (split_at == 0 && len > 2) {
            split_at = 2;
        }
        if (split_at > 0 && split_at < len) {
            if (queue_tcp_outbound(idx, s, work, split_at) < 0) return -1;
            micro_pause(3000);
            return queue_tcp_outbound(idx, s, work + split_at, len - split_at);
        }
    }

    return queue_tcp_outbound(idx, s, work, len);
}

static int find_tcp_session_idx(uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport) {
    for (int i = 0; i < MAX_TCP_SESSIONS; i++) {
        tcp_session_t *s = g_tcp[i];
        if (s && s->src_ip == sip && s->src_port == sport &&
            s->dst_ip == dip && s->dst_port == dport) {
            return i;
        }
    }
    return -1;
}

static int alloc_tcp_slot(void) {
    int oldest_idx = 0;
    time_t oldest_time = now_sec() + 1;
    for (int i = 0; i < MAX_TCP_SESSIONS; i++) {
        if (!g_tcp[i]) {
            return i;
        }
        if (g_tcp[i]->last_active < oldest_time) {
            oldest_time = g_tcp[i]->last_active;
            oldest_idx = i;
        }
    }
    close_tcp_session(oldest_idx);
    return oldest_idx;
}

static int find_udp_session_idx(uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport) {
    for (int i = 0; i < MAX_UDP_SESSIONS; i++) {
        udp_session_t *u = g_udp[i];
        if (u && u->src_ip == sip && u->src_port == sport &&
            u->dst_ip == dip && u->dst_port == dport) {
            return i;
        }
    }
    return -1;
}

static int alloc_udp_slot(void) {
    int oldest_idx = 0;
    time_t oldest_time = now_sec() + 1;
    for (int i = 0; i < MAX_UDP_SESSIONS; i++) {
        if (!g_udp[i]) {
            return i;
        }
        if (g_udp[i]->last_active < oldest_time) {
            oldest_time = g_udp[i]->last_active;
            oldest_idx = i;
        }
    }
    close_udp_session(oldest_idx);
    return oldest_idx;
}

static void handle_tun_tcp(JNIEnv *env, const uint8_t *pkt, size_t pkt_len, size_t ip_hdr_len) {
    if (pkt_len < ip_hdr_len + 20) return;

    uint32_t src_ip, dst_ip;
    memcpy(&src_ip, pkt + 12, 4);
    memcpy(&dst_ip, pkt + 16, 4);

    const uint8_t *tcp = pkt + ip_hdr_len;
    size_t tcp_hdr_len = ((tcp[12] >> 4) & 0x0F) * 4;
    if (tcp_hdr_len < 20 || ip_hdr_len + tcp_hdr_len > pkt_len) return;

    uint16_t src_port, dst_port;
    memcpy(&src_port, tcp + 0, 2);
    memcpy(&dst_port, tcp + 2, 2);

    uint32_t seq_num;
    memcpy(&seq_num, tcp + 4, 4);
    seq_num = ntohl(seq_num);

    uint8_t flags = tcp[13];
    uint16_t win;
    memcpy(&win, tcp + 14, 2);
    win = ntohs(win);

    const uint8_t *payload = pkt + ip_hdr_len + tcp_hdr_len;
    size_t payload_len = pkt_len - ip_hdr_len - tcp_hdr_len;

    int idx = find_tcp_session_idx(src_ip, src_port, dst_ip, dst_port);

    if (flags & 0x04 /* RST */) {
        if (idx >= 0) close_tcp_session(idx);
        return;
    }

    if (ntohs(dst_port) == 853) {
        /* Reject DoT on port 853 immediately so Android 9+ doesn't hang validating Private DNS */
        send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, seq_num + 1);
        return;
    }

    if ((flags & 0x02 /* SYN */) && !(flags & 0x10 /* ACK */)) {
        if (idx >= 0 && g_tcp[idx]) {
            if (g_tcp[idx]->state == TCP_STATE_CONNECTING) {
                /* Connection is already in flight; do not drop/recreate on duplicate SYN */
                return;
            }
            close_tcp_session(idx);
        }

        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, seq_num + 1);
            return;
        }

        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int mss = MTU_PAYLOAD_CHUNK;
        setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));

        if (!protect_fd(env, fd)) {
            close(fd);
            send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, seq_num + 1);
            return;
        }

        uint32_t target_ip = dst_ip;
        uint16_t target_port = dst_port;
        if (ntohs(dst_port) == 53) {
            static atomic_int s_dns_toggle_tcp = 0;
            int tog = atomic_fetch_add(&s_dns_toggle_tcp, 1);
            int pick = tog % 3;
            if (pick < 0) pick = -pick;
            if (pick == 0)      target_ip = inet_addr("208.67.222.222"); /* OpenDNS 1 */
            else if (pick == 1) target_ip = inet_addr("94.140.14.14");   /* AdGuard */
            else                target_ip = inet_addr("208.67.220.220"); /* OpenDNS 2 */
            target_port = htons(5353);
        }

        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = target_ip;
        dst.sin_port = target_port;

        int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, seq_num + 1);
            return;
        }

        tcp_session_t *s = calloc(1, sizeof(tcp_session_t));
        if (!s) {
            close(fd);
            send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, seq_num + 1);
            return;
        }

        idx = alloc_tcp_slot();
        s->state = TCP_STATE_CONNECTING;
        s->sock_fd = fd;
        s->src_ip = src_ip;
        s->src_port = src_port;
        s->dst_ip = dst_ip;
        s->dst_port = dst_port;
        s->client_seq = seq_num + 1;
        s->server_seq = (uint32_t)(now_sec() * 2654435761U) ^ ((uint32_t)idx << 16);
        s->client_win = win > 0 ? win : 65535;
        s->last_active = now_sec();
        s->epoll_want_out = true;

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
        ev.data.ptr = &g_tcp_ctxs[idx];
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            free(s);
            send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, seq_num + 1);
            return;
        }

        g_tcp[idx] = s;
        g_active_sessions++;
        sync_idle_timer();
        return;
    }

    if (idx < 0 || !g_tcp[idx]) {
        if (!(flags & 0x04)) {
            send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, seq_num + (uint32_t)payload_len);
        }
        return;
    }

    tcp_session_t *s = g_tcp[idx];
    s->last_active = now_sec();
    if (win > 0) s->client_win = win;

    if (payload_len > 0) {
        if (seq_num == s->client_seq) {
            s->client_seq += (uint32_t)payload_len;
            send_tcp_packet_to_tun(s, 0x10 /* ACK */, NULL, 0, false);
            if (send_with_dpi_bypass(env, idx, s, payload, payload_len) < 0) {
                send_rst_to_tun(src_ip, src_port, dst_ip, dst_port, s->client_seq);
                close_tcp_session(idx);
                return;
            }
        } else {
            send_tcp_packet_to_tun(s, 0x10 /* ACK */, NULL, 0, false);
        }
    }

    if (flags & 0x01 /* FIN */) {
        if (!s->client_fin) {
            s->client_fin = true;
            s->client_seq += 1;
            send_tcp_packet_to_tun(s, 0x10 /* ACK */, NULL, 0, false);
            shutdown(s->sock_fd, SHUT_WR);
            s->state = TCP_STATE_FIN_WAIT;
            if (s->server_fin) {
                close_tcp_session(idx);
            }
        }
    }
}

static void handle_tun_udp(JNIEnv *env, const uint8_t *pkt, size_t pkt_len, size_t ip_hdr_len) {
    if (pkt_len < ip_hdr_len + 8) return;

    uint32_t src_ip, dst_ip;
    memcpy(&src_ip, pkt + 12, 4);
    memcpy(&dst_ip, pkt + 16, 4);

    const uint8_t *udp = pkt + ip_hdr_len;
    uint16_t src_port, dst_port;
    memcpy(&src_port, udp + 0, 2);
    memcpy(&dst_port, udp + 2, 2);

    uint16_t dport = ntohs(dst_port);
    if (dport == 443) {
        /* Drop QUIC/HTTP3 to force fallback to TCP TLS 443, identical to Win64 */
        return;
    }

    const uint8_t *payload = udp + 8;
    size_t payload_len = pkt_len - ip_hdr_len - 8;

    int idx = find_udp_session_idx(src_ip, src_port, dst_ip, dst_port);
    udp_session_t *u = (idx >= 0) ? g_udp[idx] : NULL;

    bool is_dns = (dport == 53);

    if (!u) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return;

        if (!protect_fd(env, fd)) {
            close(fd);
            return;
        }

        uint32_t target_ip = dst_ip;
        uint16_t target_port = dst_port;
        if (is_dns) {
            static atomic_int s_dns_toggle = 0;
            int tog = atomic_fetch_add(&s_dns_toggle, 1);
            int pick = tog % 3;
            if (pick < 0) pick = -pick;
            if (pick == 0)      target_ip = inet_addr("208.67.222.222"); /* OpenDNS 1 */
            else if (pick == 1) target_ip = inet_addr("94.140.14.14");   /* AdGuard */
            else                target_ip = inet_addr("208.67.220.220"); /* OpenDNS 2 */
            target_port = htons(5353);
        }

        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = target_ip;
        dst.sin_port = target_port;

        if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            close(fd);
            return;
        }

        u = calloc(1, sizeof(udp_session_t));
        if (!u) {
            close(fd);
            return;
        }

        idx = alloc_udp_slot();
        u->sock_fd = fd;
        u->src_ip = src_ip;
        u->src_port = src_port;
        u->dst_ip = dst_ip;
        u->dst_port = dst_port;
        u->forward_ip = target_ip;
        u->is_dns = is_dns;
        u->last_active = now_sec();

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.ptr = &g_udp_ctxs[idx];
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            free(u);
            return;
        }

        g_udp[idx] = u;
        g_active_sessions++;
        sync_idle_timer();
    }

    u->last_active = now_sec();
    if (payload_len > 0) {
        ssize_t n = send(u->sock_fd, payload, payload_len, MSG_DONTWAIT);
        if (n > 0) {
            atomic_fetch_add(&g_tx_bytes, (uint64_t)n);
        }
    }
}

static void handle_tun_read(JNIEnv *env) {
    uint8_t buf[TUN_BUF_SIZE];
    for (int iter = 0; iter < 64; iter++) {
        ssize_t n = read(g_tun_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
        if (n < 20) continue;

        uint8_t version = (buf[0] >> 4) & 0x0F;
        if (version == 6) {
            /* Reject IPv6 TCP immediately with RST so Happy Eyeballs falls back to IPv4 in 0ms */
            if ((size_t)n >= 60 && buf[6] == IPPROTO_TCP) {
                send_ipv6_rst_to_tun(buf, (size_t)n);
            }
            continue;
        }
        if (version != 4) continue;

        size_t ip_hdr_len = (buf[0] & 0x0F) * 4;
        if (ip_hdr_len < 20 || ip_hdr_len > (size_t)n) continue;

        uint16_t total_len = ((uint16_t)buf[2] << 8) | buf[3];
        if (total_len > (size_t)n) total_len = (uint16_t)n;

        uint8_t proto = buf[9];
        if (proto == IPPROTO_TCP) {
            handle_tun_tcp(env, buf, total_len, ip_hdr_len);
        } else if (proto == IPPROTO_UDP) {
            handle_tun_udp(env, buf, total_len, ip_hdr_len);
        }
    }
}

static void handle_tcp_event(int idx, uint32_t events) {
    if (idx < 0 || idx >= MAX_TCP_SESSIONS) return;
    tcp_session_t *s = g_tcp[idx];
    if (!s) return;

    s->last_active = now_sec();

    if (events & EPOLLERR) {
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(s->sock_fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        send_rst_to_tun(s->src_ip, s->src_port, s->dst_ip, s->dst_port, s->client_seq);
        close_tcp_session(idx);
        return;
    }

    if (s->state == TCP_STATE_CONNECTING && (events & EPOLLOUT)) {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(s->sock_fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            send_rst_to_tun(s->src_ip, s->src_port, s->dst_ip, s->dst_port, s->client_seq);
            close_tcp_session(idx);
            return;
        }
        send_tcp_packet_to_tun(s, 0x12 /* SYN|ACK */, NULL, 0, true);
        s->server_seq += 1;
        s->state = TCP_STATE_ESTABLISHED;
        update_tcp_epoll(idx, s, s->out_len > 0);
    }

    if ((events & EPOLLOUT) && s->state != TCP_STATE_CONNECTING && s->out_len > 0) {
        ssize_t n = send(s->sock_fd, s->out_buf, s->out_len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            atomic_fetch_add(&g_tx_bytes, (uint64_t)n);
            size_t rem = s->out_len - (size_t)n;
            if (rem > 0) {
                memmove(s->out_buf, s->out_buf + n, rem);
            }
            s->out_len = rem;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            send_rst_to_tun(s->src_ip, s->src_port, s->dst_ip, s->dst_port, s->client_seq);
            close_tcp_session(idx);
            return;
        }
        if (s->out_len == 0) {
            update_tcp_epoll(idx, s, false);
        }
    }

    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
        uint8_t rbuf[MTU_PAYLOAD_CHUNK];
        for (int iter = 0; iter < 16; iter++) {
            ssize_t n = recv(s->sock_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
            if (n > 0) {
                atomic_fetch_add(&g_rx_bytes, (uint64_t)n);
                send_tcp_packet_to_tun(s, 0x18 /* PSH|ACK */, rbuf, (size_t)n, false);
                s->server_seq += (uint32_t)n;
            } else if (n == 0) {
                if (!s->server_fin) {
                    s->server_fin = true;
                    send_tcp_packet_to_tun(s, 0x11 /* FIN|ACK */, NULL, 0, false);
                    s->server_seq += 1;
                }
                if (s->client_fin) {
                    close_tcp_session(idx);
                } else {
                    s->state = TCP_STATE_FIN_WAIT;
                    /* Unsubscribe EPOLLIN to prevent 100% CPU spin on EOF socket */
                    struct epoll_event ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.events = EPOLLRDHUP | EPOLLERR | EPOLLHUP | (s->out_len > 0 ? EPOLLOUT : 0);
                    ev.data.ptr = &g_tcp_ctxs[idx];
                    epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, s->sock_fd, &ev);
                }
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                send_rst_to_tun(s->src_ip, s->src_port, s->dst_ip, s->dst_port, s->client_seq);
                close_tcp_session(idx);
                break;
            }
        }
    }
}

static void handle_udp_event(int idx, uint32_t events) {
    if (idx < 0 || idx >= MAX_UDP_SESSIONS) return;
    udp_session_t *u = g_udp[idx];
    if (!u) return;

    if (events & (EPOLLERR | EPOLLHUP)) {
        close_udp_session(idx);
        return;
    }

    if (events & EPOLLIN) {
        uint8_t rbuf[MTU_PAYLOAD_CHUNK];
        for (int iter = 0; iter < 8; iter++) {
            ssize_t n = recv(u->sock_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
            if (n > 0) {
                u->last_active = now_sec();
                atomic_fetch_add(&g_rx_bytes, (uint64_t)n);
                send_udp_packet_to_tun(u, rbuf, (size_t)n);
                if (u->is_dns) {
                    /* DNS reply received and forwarded to TUN - release session immediately */
                    close_udp_session(idx);
                    break;
                }
            } else {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                close_udp_session(idx);
                break;
            }
        }
    }
}

static void reap_idle_sessions(void) {
    time_t now = now_sec();
    for (int i = 0; i < MAX_TCP_SESSIONS; i++) {
        tcp_session_t *s = g_tcp[i];
        if (s) {
            time_t limit = (s->state == TCP_STATE_FIN_WAIT) ? 10 : SESSION_IDLE_TIMEOUT_SEC;
            if (now - s->last_active >= limit) {
                close_tcp_session(i);
            }
        }
    }
    for (int i = 0; i < MAX_UDP_SESSIONS; i++) {
        udp_session_t *u = g_udp[i];
        if (u && (now - u->last_active >= UDP_IDLE_TIMEOUT_SEC)) {
            close_udp_session(i);
        }
    }
}

static void *engine_worker(void *arg) {
    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
        LOGE("Failed to attach worker thread to JVM");
        return NULL;
    }

    LOGI("LiberVia epoll worker started (split=%d, disorder=%d, ttl=%d)",
         g_cfg.split_bytes, g_cfg.disorder, g_cfg.fake_ttl);

    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (atomic_load(&g_running)) {
        /* True idle sleep: block indefinitely (-1) until packet, eventfd, or timerfd fires */
        int nfds = epoll_wait(g_epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOGE("epoll_wait failed: %d", errno);
            break;
        }

        for (int i = 0; i < nfds; i++) {
            epoll_ctx_t *ctx = (epoll_ctx_t *)events[i].data.ptr;
            if (!ctx) continue;

            if (ctx->tag == TAG_EVENT) {
                uint64_t val = 0;
                ssize_t r = read(g_event_fd, &val, sizeof(val));
                (void)r;
                break;
            } else if (ctx->tag == TAG_TIMER) {
                uint64_t exp = 0;
                ssize_t r = read(g_timer_fd, &exp, sizeof(exp));
                (void)r;
                reap_idle_sessions();
            } else if (ctx->tag == TAG_TUN) {
                if ((*env)->PushLocalFrame(env, 16) == JNI_OK) {
                    handle_tun_read(env);
                    (*env)->PopLocalFrame(env, NULL);
                } else {
                    handle_tun_read(env);
                }
            } else if (ctx->tag == TAG_TCP) {
                handle_tcp_event(ctx->index, events[i].events);
            } else if (ctx->tag == TAG_UDP) {
                handle_udp_event(ctx->index, events[i].events);
            }
        }
    }

    for (int i = 0; i < MAX_TCP_SESSIONS; i++) close_tcp_session(i);
    for (int i = 0; i < MAX_UDP_SESSIONS; i++) close_udp_session(i);

    (*g_jvm)->DetachCurrentThread(g_jvm);
    LOGI("LiberVia epoll worker exited");
    return NULL;
}

static void stop_engine_internal(JNIEnv *env) {
    pthread_mutex_lock(&g_lifecycle_lock);
    atomic_store(&g_running, false);

    if (g_event_fd >= 0) {
        uint64_t wake = 1;
        ssize_t w = write(g_event_fd, &wake, sizeof(wake));
        (void)w;
    }

    if (g_thread_started) {
        pthread_join(g_worker_thread, NULL);
        g_thread_started = false;
    }

    if (g_timer_fd >= 0) {
        close(g_timer_fd);
        g_timer_fd = -1;
    }
    g_timer_armed = false;
    g_active_sessions = 0;

    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    if (g_event_fd >= 0) {
        close(g_event_fd);
        g_event_fd = -1;
    }
    if (g_tun_fd >= 0) {
        close(g_tun_fd);
        g_tun_fd = -1;
    }

    if (env && g_service_ref) {
        (*env)->DeleteGlobalRef(env, g_service_ref);
        g_service_ref = NULL;
    }
    g_protect_mid = NULL;

    pthread_mutex_unlock(&g_lifecycle_lock);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_libervia_app_LiberViaVpnService_nativeStartEngine(
        JNIEnv *env, jobject thiz, jint tun_fd, jstring flags, jstring dns_server) {
    if (!g_jvm) {
        (*env)->GetJavaVM(env, &g_jvm);
    }

    stop_engine_internal(env);

    pthread_mutex_lock(&g_lifecycle_lock);

    memset(g_tcp, 0, sizeof(g_tcp));
    memset(g_udp, 0, sizeof(g_udp));
    g_active_sessions = 0;
    g_timer_armed = false;

    for (int i = 0; i < MAX_TCP_SESSIONS; i++) {
        g_tcp_ctxs[i].tag = TAG_TCP;
        g_tcp_ctxs[i].index = i;
    }
    for (int i = 0; i < MAX_UDP_SESSIONS; i++) {
        g_udp_ctxs[i].tag = TAG_UDP;
        g_udp_ctxs[i].index = i;
    }

    atomic_store(&g_rx_bytes, 0);
    atomic_store(&g_tx_bytes, 0);

    const char *flags_c = flags ? (*env)->GetStringUTFChars(env, flags, NULL) : NULL;
    parse_flags(flags_c, &g_cfg);
    if (flags_c) (*env)->ReleaseStringUTFChars(env, flags, flags_c);

    const char *dns_c = dns_server ? (*env)->GetStringUTFChars(env, dns_server, NULL) : NULL;
    struct in_addr dns_addr;
    if (dns_c && inet_pton(AF_INET, dns_c, &dns_addr) == 1) {
        g_dns_ip_be = dns_addr.s_addr;
    } else {
        inet_pton(AF_INET, "1.1.1.1", &dns_addr);
        g_dns_ip_be = dns_addr.s_addr;
    }
    if (dns_c) (*env)->ReleaseStringUTFChars(env, dns_server, dns_c);

    int dup_tun = dup((int)tun_fd);
    if (dup_tun < 0 || set_nonblocking(dup_tun) < 0) {
        if (dup_tun >= 0) close(dup_tun);
        pthread_mutex_unlock(&g_lifecycle_lock);
        return -1;
    }

    int ep_fd = epoll_create1(EPOLL_CLOEXEC);
    int ev_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int tmr_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ep_fd < 0 || ev_fd < 0 || tmr_fd < 0) {
        if (ep_fd >= 0) close(ep_fd);
        if (ev_fd >= 0) close(ev_fd);
        if (tmr_fd >= 0) close(tmr_fd);
        close(dup_tun);
        pthread_mutex_unlock(&g_lifecycle_lock);
        return -2;
    }

    jclass cls = (*env)->GetObjectClass(env, thiz);
    jmethodID mid = cls ? (*env)->GetMethodID(env, cls, "protectSocket", "(I)Z") : NULL;
    if (cls) {
        (*env)->DeleteLocalRef(env, cls);
    }
    if (!mid) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        close(tmr_fd);
        close(ep_fd);
        close(ev_fd);
        close(dup_tun);
        pthread_mutex_unlock(&g_lifecycle_lock);
        return -3;
    }

    g_service_ref = (*env)->NewGlobalRef(env, thiz);
    g_protect_mid = mid;
    g_tun_fd = dup_tun;
    g_epoll_fd = ep_fd;
    g_event_fd = ev_fd;
    g_timer_fd = tmr_fd;

    struct epoll_event ev_tun;
    memset(&ev_tun, 0, sizeof(ev_tun));
    ev_tun.events = EPOLLIN;
    ev_tun.data.ptr = &g_tun_ctx;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_tun_fd, &ev_tun) < 0) {
        pthread_mutex_unlock(&g_lifecycle_lock);
        stop_engine_internal(env);
        return -4;
    }

    struct epoll_event ev_wake;
    memset(&ev_wake, 0, sizeof(ev_wake));
    ev_wake.events = EPOLLIN;
    ev_wake.data.ptr = &g_evt_ctx;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_event_fd, &ev_wake) < 0) {
        pthread_mutex_unlock(&g_lifecycle_lock);
        stop_engine_internal(env);
        return -5;
    }

    struct epoll_event ev_tmr;
    memset(&ev_tmr, 0, sizeof(ev_tmr));
    ev_tmr.events = EPOLLIN;
    ev_tmr.data.ptr = &g_tmr_ctx;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_timer_fd, &ev_tmr) < 0) {
        pthread_mutex_unlock(&g_lifecycle_lock);
        stop_engine_internal(env);
        return -6;
    }

    atomic_store(&g_running, true);
    if (pthread_create(&g_worker_thread, NULL, engine_worker, NULL) != 0) {
        atomic_store(&g_running, false);
        pthread_mutex_unlock(&g_lifecycle_lock);
        stop_engine_internal(env);
        return -7;
    }
    g_thread_started = true;

    pthread_mutex_unlock(&g_lifecycle_lock);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_libervia_app_LiberViaVpnService_nativeStopEngine(JNIEnv *env, jobject thiz) {
    stop_engine_internal(env);
}

JNIEXPORT jlongArray JNICALL
Java_com_libervia_app_LiberViaVpnService_nativeGetStats(JNIEnv *env, jobject thiz) {
    jlongArray arr = (*env)->NewLongArray(env, 2);
    if (!arr) return NULL;
    jlong stats[2];
    stats[0] = (jlong)atomic_load(&g_rx_bytes);
    stats[1] = (jlong)atomic_load(&g_tx_bytes);
    (*env)->SetLongArrayRegion(env, arr, 0, 2, stats);
    return arr;
}
