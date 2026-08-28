#include "velawear_pan.h"

#include <nuttx/config.h>
#include <errno.h>
#include <syslog.h>

#ifndef CONFIG_VELAWEAR_XIAOZHI_PAN_PEER
#  define CONFIG_VELAWEAR_XIAOZHI_PAN_PEER ""
#endif

#if defined(CONFIG_VELAWEAR_XIAOZHI_PAN) && \
    defined(CONFIG_BT_CLASSIC) && \
    defined(CONFIG_NET_TUN) && \
    defined(CONFIG_NET_ETHERNET) && \
    defined(CONFIG_NETUTILS_NETLIB) && \
    defined(CONFIG_NETUTILS_DHCPC)

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <net/if.h>
#include <netutils/netlib.h>
#include <nuttx/net/ioctl.h>
#include <nuttx/net/tun.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/classic/classic.h>
#include <zephyr/bluetooth/classic/sdp.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>

#define VELAWEAR_PAN_IFNAME             "bt-pan"
#define VELAWEAR_PAN_BNEP_PSM           0x000f
#define VELAWEAR_PAN_MTU                640
#define VELAWEAR_PAN_FRAME_MAX          (VELAWEAR_PAN_MTU + 14)
#define VELAWEAR_PAN_L2CAP_MTU          672

#define BNEP_TYPE_GENERAL               0x00
#define BNEP_TYPE_CONTROL               0x01
#define BNEP_TYPE_COMPRESSED            0x02
#define BNEP_TYPE_COMPRESSED_SRC_ONLY   0x03
#define BNEP_TYPE_COMPRESSED_DST_ONLY   0x04

#define BNEP_CONTROL_SETUP_REQUEST      0x01
#define BNEP_CONTROL_SETUP_RESPONSE     0x02
#define BNEP_CONTROL_FILTER_NET_TYPE    0x04
#define BNEP_CONTROL_FILTER_MULTI_ADDR  0x06
#define BNEP_CONTROL_NOT_UNDERSTOOD     0x00
#define BNEP_RESPONSE_SUCCESS           0x0000

NET_BUF_POOL_FIXED_DEFINE(g_pan_sdp_pool,
                          2,
                          BT_L2CAP_BUF_SIZE(CONFIG_BT_L2CAP_TX_MTU),
                          CONFIG_BT_CONN_TX_USER_DATA_SIZE,
                          NULL);

NET_BUF_POOL_FIXED_DEFINE(g_pan_tx_pool,
                          4,
                          BT_L2CAP_BUF_SIZE(VELAWEAR_PAN_L2CAP_MTU),
                          CONFIG_BT_CONN_TX_USER_DATA_SIZE,
                          NULL);

struct velawear_pan_state
{
  pthread_mutex_t lock;
  bool initialized;
  bool callbacks_registered;
  bool configured;
  bool ready;
  bool channel_up;
  bool stop_requested;
  bool io_started;
  bool dhcp_started;
  int tun_fd;
  struct bt_conn *conn;
  struct bt_l2cap_br_chan bnep_chan;
  uint8_t local_mac[6];
  uint8_t last_dst[6];
  uint8_t last_src[6];
  uint8_t last_type[2];
  bool have_compression_context;
  pthread_t io_thread;
  pthread_t dhcp_thread;
};

static struct velawear_pan_state g_pan =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .tun_fd = -1,
};

static uint8_t velawear_pan_sdp_result(
  struct bt_conn *conn,
  struct bt_sdp_client_result *result,
  const struct bt_sdp_discover_params *params);
static void velawear_pan_l2cap_connected(struct bt_l2cap_chan *chan);
static void velawear_pan_l2cap_disconnected(struct bt_l2cap_chan *chan);
static int velawear_pan_l2cap_recv(struct bt_l2cap_chan *chan,
                                   struct net_buf *buf);
static void *velawear_pan_io_thread(void *arg);
static void *velawear_pan_dhcp_thread(void *arg);

static const struct bt_l2cap_chan_ops g_pan_l2cap_ops =
{
  .connected = velawear_pan_l2cap_connected,
  .disconnected = velawear_pan_l2cap_disconnected,
  .recv = velawear_pan_l2cap_recv,
};

static struct bt_sdp_discover_params g_pan_sdp =
{
  .uuid = BT_UUID_DECLARE_16(BT_SDP_NAP_SVCLASS),
  .func = velawear_pan_sdp_result,
  .pool = &g_pan_sdp_pool,
  .type = BT_SDP_DISCOVER_SERVICE_SEARCH_ATTR,
};

static bool velawear_pan_stopping(void)
{
  bool stopping;

  pthread_mutex_lock(&g_pan.lock);
  stopping = g_pan.stop_requested;
  pthread_mutex_unlock(&g_pan.lock);
  return stopping;
}

static int velawear_pan_send(const uint8_t *data, size_t len)
{
  struct net_buf *buf;
  struct bt_l2cap_chan *chan;
  int ret;

  if (data == NULL || len == 0 || len > VELAWEAR_PAN_L2CAP_MTU)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_pan.lock);
  if (!g_pan.initialized || g_pan.stop_requested || !g_pan.channel_up)
    {
      pthread_mutex_unlock(&g_pan.lock);
      return -ENETDOWN;
    }

  chan = &g_pan.bnep_chan.chan;
  if (g_pan.bnep_chan.tx.mtu != 0 && len > g_pan.bnep_chan.tx.mtu)
    {
      pthread_mutex_unlock(&g_pan.lock);
      return -EMSGSIZE;
    }
  pthread_mutex_unlock(&g_pan.lock);

  buf = net_buf_alloc(&g_pan_tx_pool, K_NO_WAIT);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
  net_buf_add_mem(buf, data, len);
  ret = bt_l2cap_chan_send(chan, buf);
  if (ret < 0)
    {
      net_buf_unref(buf);
    }

  return ret;
}

static int velawear_pan_send_control_response(uint8_t control,
                                              uint16_t response)
{
  uint8_t packet[4];

  packet[0] = BNEP_TYPE_CONTROL;
  packet[1] = control;
  packet[2] = (uint8_t)(response >> 8);
  packet[3] = (uint8_t)(response & 0xff);
  return velawear_pan_send(packet, sizeof(packet));
}

static void velawear_pan_mark_link_up(void)
{
  bool start_io = false;
  bool start_dhcp = false;
  int ret;

  ret = netlib_ifup(VELAWEAR_PAN_IFNAME);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[PAN] netlib_ifup(%s) failed: %d\n",
             VELAWEAR_PAN_IFNAME, ret);
      return;
    }

  pthread_mutex_lock(&g_pan.lock);
  if (g_pan.stop_requested)
    {
      pthread_mutex_unlock(&g_pan.lock);
      return;
    }

  if (!g_pan.io_started)
    {
      g_pan.io_started = true;
      start_io = true;
    }
  if (!g_pan.dhcp_started)
    {
      g_pan.dhcp_started = true;
      start_dhcp = true;
    }
  pthread_mutex_unlock(&g_pan.lock);

  if (start_io)
    {
      ret = pthread_create(&g_pan.io_thread, NULL,
                           velawear_pan_io_thread, NULL);
      if (ret != 0)
        {
          pthread_mutex_lock(&g_pan.lock);
          g_pan.io_started = false;
          g_pan.dhcp_started = false;
          pthread_mutex_unlock(&g_pan.lock);
          start_dhcp = false;
          syslog(LOG_ERR, "[PAN] TAP bridge thread failed: %d\n", ret);
        }
    }

  if (start_dhcp)
    {
      ret = pthread_create(&g_pan.dhcp_thread, NULL,
                           velawear_pan_dhcp_thread, NULL);
      if (ret != 0)
        {
          pthread_mutex_lock(&g_pan.lock);
          g_pan.dhcp_started = false;
          pthread_mutex_unlock(&g_pan.lock);
          syslog(LOG_ERR, "[PAN] DHCP thread failed: %d\n", ret);
        }
    }

  if (!velawear_pan_stopping())
    {
      syslog(LOG_INFO, "[PAN] BNEP link up; DHCP pending\n");
    }
}

static int velawear_pan_forward_frame(const uint8_t *data, size_t len)
{
  uint8_t frame[CONFIG_NET_TUN_PKTSIZE];
  size_t header_len;
  size_t payload_offset;
  size_t frame_len;
  ssize_t written;
  int tun_fd;

  if (data == NULL || len == 0 || (data[0] & 0x80) != 0)
    {
      return -EINVAL;
    }

  switch (data[0] & 0x7f)
    {
      case BNEP_TYPE_GENERAL:
        if (len < 15)
          {
            return -EBADMSG;
          }

        header_len = 14;
        payload_offset = 15;
        pthread_mutex_lock(&g_pan.lock);
        memcpy(g_pan.last_dst, data + 1, 6);
        memcpy(g_pan.last_src, data + 7, 6);
        memcpy(g_pan.last_type, data + 13, 2);
        g_pan.have_compression_context = true;
        pthread_mutex_unlock(&g_pan.lock);
        memcpy(frame, data + 1, header_len);
        break;

      case BNEP_TYPE_COMPRESSED:
        if (len < 3)
          {
            return -EBADMSG;
          }

        payload_offset = 3;
        pthread_mutex_lock(&g_pan.lock);
        if (!g_pan.have_compression_context)
          {
            pthread_mutex_unlock(&g_pan.lock);
            return -EBADMSG;
          }
        memcpy(frame, g_pan.last_dst, 6);
        memcpy(frame + 6, g_pan.last_src, 6);
        memcpy(g_pan.last_type, data + 1, 2);
        memcpy(frame + 12, g_pan.last_type, 2);
        pthread_mutex_unlock(&g_pan.lock);
        header_len = 14;
        break;

      case BNEP_TYPE_COMPRESSED_SRC_ONLY:
        if (len < 9)
          {
            return -EBADMSG;
          }

        payload_offset = 9;
        pthread_mutex_lock(&g_pan.lock);
        if (!g_pan.have_compression_context)
          {
            pthread_mutex_unlock(&g_pan.lock);
            return -EBADMSG;
          }
        memcpy(frame, g_pan.last_dst, 6);
        memcpy(frame + 6, data + 1, 6);
        memcpy(g_pan.last_src, data + 1, 6);
        memcpy(g_pan.last_type, data + 7, 2);
        memcpy(frame + 12, g_pan.last_type, 2);
        pthread_mutex_unlock(&g_pan.lock);
        header_len = 14;
        break;

      case BNEP_TYPE_COMPRESSED_DST_ONLY:
        if (len < 9)
          {
            return -EBADMSG;
          }

        payload_offset = 9;
        pthread_mutex_lock(&g_pan.lock);
        if (!g_pan.have_compression_context)
          {
            pthread_mutex_unlock(&g_pan.lock);
            return -EBADMSG;
          }
        memcpy(frame, data + 1, 6);
        memcpy(frame + 6, g_pan.last_src, 6);
        memcpy(g_pan.last_dst, data + 1, 6);
        memcpy(g_pan.last_type, data + 7, 2);
        memcpy(frame + 12, g_pan.last_type, 2);
        pthread_mutex_unlock(&g_pan.lock);
        header_len = 14;
        break;

      default:
        return -EPROTO;
    }

  if (payload_offset > len ||
      len - payload_offset > VELAWEAR_PAN_FRAME_MAX - header_len)
    {
      return -EMSGSIZE;
    }

  frame_len = header_len + len - payload_offset;
  memcpy(frame + header_len, data + payload_offset, len - payload_offset);

  pthread_mutex_lock(&g_pan.lock);
  tun_fd = g_pan.tun_fd;
  pthread_mutex_unlock(&g_pan.lock);
  if (tun_fd < 0)
    {
      return -ENETDOWN;
    }

  written = write(tun_fd, frame, frame_len);
  if (written != (ssize_t)frame_len)
    {
      return written < 0 ? -errno : -EIO;
    }

  return 0;
}

static void *velawear_pan_io_thread(void *arg)
{
  uint8_t packet[CONFIG_NET_TUN_PKTSIZE + 1];

  (void)arg;
  while (!velawear_pan_stopping())
    {
      struct pollfd pfd;
      int tun_fd;
      int ret;
      ssize_t len;

      pthread_mutex_lock(&g_pan.lock);
      tun_fd = g_pan.tun_fd;
      pthread_mutex_unlock(&g_pan.lock);
      if (tun_fd < 0)
        {
          break;
        }

      pfd.fd = tun_fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll(&pfd, 1, 500);
      if (ret <= 0 || (pfd.revents & POLLIN) == 0)
        {
          continue;
        }

      len = read(tun_fd, packet + 1, CONFIG_NET_TUN_PKTSIZE);
      if (len < 14 || len > VELAWEAR_PAN_FRAME_MAX)
        {
          continue;
        }

      packet[0] = BNEP_TYPE_GENERAL;
      ret = velawear_pan_send(packet, (size_t)len + 1);
      if (ret < 0 && ret != -ENETDOWN)
        {
          syslog(LOG_WARNING, "[PAN] BNEP TX failed: %d\n", ret);
        }
    }

  return NULL;
}

static void *velawear_pan_dhcp_thread(void *arg)
{
  int ret;
  int attempt;

  (void)arg;
  sleep(1);
  for (attempt = 0; attempt < 3 && !velawear_pan_stopping(); attempt++)
    {
      ret = netlib_obtain_ipv4addr(VELAWEAR_PAN_IFNAME);
      if (ret == 0)
        {
          pthread_mutex_lock(&g_pan.lock);
          g_pan.ready = true;
          pthread_mutex_unlock(&g_pan.lock);
          syslog(LOG_INFO, "[PAN] DHCP completed on %s\n",
                 VELAWEAR_PAN_IFNAME);
          return NULL;
        }

      syslog(LOG_WARNING, "[PAN] DHCP on %s failed (attempt %d): %d\n",
             VELAWEAR_PAN_IFNAME, attempt + 1, ret);
      if (attempt < 2)
        {
          sleep(2);
        }
    }

  return NULL;
}

static void velawear_pan_l2cap_connected(struct bt_l2cap_chan *chan)
{
  static const uint8_t setup_request[] =
  {
    BNEP_TYPE_CONTROL,
    BNEP_CONTROL_SETUP_REQUEST,
    0x02,
    0x11, 0x16,
    0x11, 0x15,
  };
  int ret;

  (void)chan;
  pthread_mutex_lock(&g_pan.lock);
  g_pan.channel_up = true;
  g_pan.ready = false;
  g_pan.have_compression_context = false;
  pthread_mutex_unlock(&g_pan.lock);

  ret = velawear_pan_send(setup_request, sizeof(setup_request));
  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "[PAN] BNEP channel connected; setup request ret=%d\n", ret);
}

static void velawear_pan_l2cap_disconnected(struct bt_l2cap_chan *chan)
{
  (void)chan;

  pthread_mutex_lock(&g_pan.lock);
  g_pan.channel_up = false;
  g_pan.ready = false;
  g_pan.have_compression_context = false;
  pthread_mutex_unlock(&g_pan.lock);
  syslog(LOG_WARNING, "[PAN] BNEP channel disconnected\n");
}

static int velawear_pan_l2cap_recv(struct bt_l2cap_chan *chan,
                                   struct net_buf *buf)
{
  const uint8_t *data;
  size_t len;
  uint8_t control;
  uint16_t response;
  int ret;

  (void)chan;
  if (buf == NULL || buf->len == 0)
    {
      return -EINVAL;
    }

  data = buf->data;
  len = buf->len;
  if ((data[0] & 0x80) != 0)
    {
      return -ENOTSUP;
    }

  switch (data[0] & 0x7f)
    {
      case BNEP_TYPE_CONTROL:
        if (len < 2)
          {
            return -EBADMSG;
          }

        control = data[1];
        if (control == BNEP_CONTROL_SETUP_RESPONSE)
          {
            if (len < 4)
              {
                return -EBADMSG;
              }

            response = ((uint16_t)data[2] << 8) | data[3];
            if (response == BNEP_RESPONSE_SUCCESS)
              {
                velawear_pan_mark_link_up();
              }
            else
              {
                syslog(LOG_ERR, "[PAN] peer rejected BNEP setup: 0x%04x\n",
                       response);
              }
            return 0;
          }

        if (control == BNEP_CONTROL_SETUP_REQUEST)
          {
            if (len < 7 || data[2] != 2)
              {
                return -EBADMSG;
              }

            ret = velawear_pan_send_control_response(
              BNEP_CONTROL_SETUP_RESPONSE, BNEP_RESPONSE_SUCCESS);
            if (ret == 0)
              {
                velawear_pan_mark_link_up();
              }
            return ret;
          }

        if (control == BNEP_CONTROL_FILTER_NET_TYPE ||
            control == BNEP_CONTROL_FILTER_MULTI_ADDR)
          {
            return velawear_pan_send_control_response(control,
                                                      BNEP_RESPONSE_SUCCESS);
          }

        {
          uint8_t not_understood[3] =
          {
            BNEP_TYPE_CONTROL,
            BNEP_CONTROL_NOT_UNDERSTOOD,
            control,
          };
          return velawear_pan_send(not_understood, sizeof(not_understood));
        }

      case BNEP_TYPE_GENERAL:
      case BNEP_TYPE_COMPRESSED:
      case BNEP_TYPE_COMPRESSED_SRC_ONLY:
      case BNEP_TYPE_COMPRESSED_DST_ONLY:
        ret = velawear_pan_forward_frame(data, len);
        if (ret < 0 && ret != -EAGAIN && ret != -ENETDOWN)
          {
            syslog(LOG_DEBUG, "[PAN] dropping BNEP RX frame: %d\n", ret);
          }
        return ret;

      default:
        return -EPROTO;
    }
}

static uint8_t velawear_pan_sdp_result(
  struct bt_conn *conn,
  struct bt_sdp_client_result *result,
  const struct bt_sdp_discover_params *params)
{
  uint16_t psm = VELAWEAR_PAN_BNEP_PSM;
  int ret;

  (void)params;
  if (result != NULL && result->resp_buf != NULL)
    {
      ret = bt_sdp_get_proto_param(result->resp_buf,
                                   BT_SDP_PROTO_L2CAP, &psm);
      if (ret < 0 || psm == 0)
        {
          psm = VELAWEAR_PAN_BNEP_PSM;
          syslog(LOG_WARNING,
                 "[PAN] NAP SDP record has no L2CAP PSM; using 0x%04x\n",
                 psm);
        }
    }

  pthread_mutex_lock(&g_pan.lock);
  if (g_pan.stop_requested || conn != g_pan.conn)
    {
      pthread_mutex_unlock(&g_pan.lock);
      return BT_SDP_DISCOVER_UUID_STOP;
    }

  memset(&g_pan.bnep_chan, 0, sizeof(g_pan.bnep_chan));
  g_pan.bnep_chan.chan.ops = &g_pan_l2cap_ops;
  g_pan.bnep_chan.rx.mtu = VELAWEAR_PAN_L2CAP_MTU;
  pthread_mutex_unlock(&g_pan.lock);

  ret = bt_l2cap_chan_connect(conn, &g_pan.bnep_chan.chan, psm);
  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "[PAN] NAP SDP PSM 0x%04x; L2CAP connect ret=%d\n", psm, ret);
  return BT_SDP_DISCOVER_UUID_STOP;
}

static void velawear_pan_bt_connected(struct bt_conn *conn, uint8_t err)
{
  struct bt_conn_info info;
  int ret;

  if (bt_conn_get_info(conn, &info) < 0 ||
      info.type != BT_CONN_TYPE_BR)
    {
      return;
    }

  pthread_mutex_lock(&g_pan.lock);
  if (conn != g_pan.conn || g_pan.stop_requested)
    {
      pthread_mutex_unlock(&g_pan.lock);
      return;
    }
  pthread_mutex_unlock(&g_pan.lock);

  if (err != 0)
    {
      syslog(LOG_ERR, "[PAN] BR connection failed: 0x%02x\n", err);
      return;
    }

  ret = bt_sdp_discover(conn, &g_pan_sdp);
  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "[PAN] BR connected; NAP SDP discovery ret=%d\n", ret);
}

static void velawear_pan_bt_disconnected(struct bt_conn *conn, uint8_t reason)
{
  struct bt_conn_info info;
  struct bt_conn *owned_conn = NULL;

  if (bt_conn_get_info(conn, &info) < 0 ||
      info.type != BT_CONN_TYPE_BR)
    {
      return;
    }

  pthread_mutex_lock(&g_pan.lock);
  if (conn == g_pan.conn)
    {
      owned_conn = g_pan.conn;
      g_pan.conn = NULL;
      g_pan.channel_up = false;
      g_pan.ready = false;
    }
  pthread_mutex_unlock(&g_pan.lock);

  if (owned_conn != NULL)
    {
      bt_conn_unref(owned_conn);
    }

  syslog(LOG_WARNING, "[PAN] BR disconnected, reason=0x%02x\n", reason);
}

static struct bt_conn_cb g_pan_conn_cb =
{
  .connected = velawear_pan_bt_connected,
  .disconnected = velawear_pan_bt_disconnected,
};

static int velawear_pan_open_tun(void)
{
  struct ifreq ifr;
  int fd;
  int ret;

  fd = open("/dev/tun", O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      syslog(LOG_ERR, "[PAN] open /dev/tun failed: %d\n", errno);
      return -errno;
    }

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, VELAWEAR_PAN_IFNAME, sizeof(ifr.ifr_name));
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
  ret = ioctl(fd, TUNSETIFF, (unsigned long)((uintptr_t)&ifr));
  if (ret < 0)
    {
      syslog(LOG_ERR, "[PAN] TUNSETIFF(%s) failed: %d\n",
             VELAWEAR_PAN_IFNAME, errno);
      close(fd);
      return -errno;
    }

  ret = netlib_setmacaddr(VELAWEAR_PAN_IFNAME, g_pan.local_mac);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[PAN] setting %s MAC failed: %d\n",
             VELAWEAR_PAN_IFNAME, ret);
    }

  ret = netlib_set_mtu(VELAWEAR_PAN_IFNAME, VELAWEAR_PAN_MTU);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[PAN] setting %s MTU failed: %d\n",
             VELAWEAR_PAN_IFNAME, ret);
    }

  pthread_mutex_lock(&g_pan.lock);
  g_pan.tun_fd = fd;
  pthread_mutex_unlock(&g_pan.lock);
  return 0;
}

int velawear_pan_init(void)
{
  struct bt_br_oob oob;
  bt_addr_t peer;
  const char *peer_text = CONFIG_VELAWEAR_XIAOZHI_PAN_PEER;
  struct bt_conn *conn;
  int ret;

  pthread_mutex_lock(&g_pan.lock);
  if (g_pan.initialized)
    {
      pthread_mutex_unlock(&g_pan.lock);
      return -EALREADY;
    }
  g_pan.stop_requested = false;
  g_pan.configured = false;
  pthread_mutex_unlock(&g_pan.lock);

  ret = bt_br_oob_get_local(&oob);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[PAN] cannot read local BR address: %d\n", ret);
      return ret;
    }
  memcpy(g_pan.local_mac, oob.addr.val, sizeof(g_pan.local_mac));

  if (peer_text != NULL && peer_text[0] != '\0')
    {
      ret = bt_addr_from_str(peer_text, &peer);
      if (ret < 0)
        {
          syslog(LOG_ERR, "[PAN] invalid peer address '%s': %d\n",
                 peer_text, ret);
          return ret;
        }
      g_pan.configured = true;
    }

  ret = bt_conn_cb_register(&g_pan_conn_cb);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[PAN] bt_conn_cb_register failed: %d\n", ret);
      return ret;
    }
  g_pan.callbacks_registered = true;

  ret = velawear_pan_open_tun();
  if (ret < 0)
    {
      bt_conn_cb_unregister(&g_pan_conn_cb);
      g_pan.callbacks_registered = false;
      return ret;
    }

  pthread_mutex_lock(&g_pan.lock);
  g_pan.initialized = true;
  pthread_mutex_unlock(&g_pan.lock);

  if (!g_pan.configured)
    {
      syslog(LOG_WARNING,
             "[PAN] peer not configured; set VELAWEAR_XIAOZHI_PAN_PEER to a paired phone BR address\n");
      return 0;
    }

  conn = bt_conn_create_br(&peer, BT_BR_CONN_PARAM_DEFAULT);
  if (conn == NULL)
    {
      syslog(LOG_ERR, "[PAN] bt_conn_create_br failed\n");
      velawear_pan_cleanup();
      return -EIO;
    }

  pthread_mutex_lock(&g_pan.lock);
  g_pan.conn = conn;
  pthread_mutex_unlock(&g_pan.lock);
  syslog(LOG_INFO, "[PAN] connecting to Classic peer %s\n", peer_text);
  return 0;
}

void velawear_pan_cleanup(void)
{
  struct bt_conn *conn;
  bool channel_up;
  bool callbacks_registered;
  bool io_started;
  bool dhcp_started;
  int tun_fd;

  pthread_mutex_lock(&g_pan.lock);
  g_pan.stop_requested = true;
  g_pan.ready = false;
  channel_up = g_pan.channel_up;
  g_pan.channel_up = false;
  conn = g_pan.conn;
  g_pan.conn = NULL;
  tun_fd = g_pan.tun_fd;
  g_pan.tun_fd = -1;
  io_started = g_pan.io_started;
  dhcp_started = g_pan.dhcp_started;
  callbacks_registered = g_pan.callbacks_registered;
  g_pan.initialized = false;
  pthread_mutex_unlock(&g_pan.lock);

  if (channel_up)
    {
      bt_l2cap_chan_disconnect(&g_pan.bnep_chan.chan);
    }
  if (conn != NULL)
    {
      bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
      bt_conn_unref(conn);
    }
  if (tun_fd >= 0)
    {
      close(tun_fd);
    }
  if (io_started)
    {
      pthread_join(g_pan.io_thread, NULL);
    }
  if (dhcp_started)
    {
      pthread_join(g_pan.dhcp_thread, NULL);
    }
  if (callbacks_registered)
    {
      bt_conn_cb_unregister(&g_pan_conn_cb);
    }

  pthread_mutex_lock(&g_pan.lock);
  g_pan.callbacks_registered = false;
  g_pan.configured = false;
  g_pan.io_started = false;
  g_pan.dhcp_started = false;
  pthread_mutex_unlock(&g_pan.lock);
}

bool velawear_pan_is_configured(void)
{
  bool configured;

  pthread_mutex_lock(&g_pan.lock);
  configured = g_pan.configured;
  pthread_mutex_unlock(&g_pan.lock);
  return configured;
}

bool velawear_pan_is_ready(void)
{
  bool ready;

  pthread_mutex_lock(&g_pan.lock);
  ready = g_pan.ready;
  pthread_mutex_unlock(&g_pan.lock);
  return ready;
}

#else

int velawear_pan_init(void)
{
  return -ENOTSUP;
}

void velawear_pan_cleanup(void)
{
}

bool velawear_pan_is_configured(void)
{
  return false;
}

bool velawear_pan_is_ready(void)
{
  return false;
}

#endif
