#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"

#include "nvs_config.h"
#include "stratum_transport.h"

static const char* TAG = "stratum_transport";

// trinetra12: a pool that is alive sends data regularly (ours: one mining.notify every ~62 s, plus a reply to every
// share). If NOTHING arrives for this long the link is treated as dead and the stratum task reconnects. This is the
// second line of defence behind TCP keepalive (which only proves the peer's TCP stack answers, not that the pool does).
static const int64_t RX_SILENCE_LIMIT_US = 180LL * 1000LL * 1000LL;

StratumTransport::StratumTransport(bool use_tls)
    : m_use_tls(use_tls), m_t(nullptr) {}

StratumTransport::~StratumTransport()
{
    close();
}

bool StratumTransport::connect(const char* host, const char* ip, uint16_t port)
{
    close();

    esp_transport_handle_t t = nullptr;

    if (m_use_tls) {
        t = esp_transport_ssl_init();
        if (!t) {
            ESP_LOGE(TAG, "esp_transport_ssl_init failed");
            return false;
        }

        esp_transport_ssl_crt_bundle_attach(t, esp_crt_bundle_attach);
        esp_transport_ssl_set_common_name(t, host);
    } else {
        t = esp_transport_tcp_init();
        if (!t) {
            ESP_LOGE(TAG, "esp_transport_tcp_init failed");
            return false;
        }
    }

    m_t = t;
    applyKeepAlive_();

    const char* connect_host = (m_use_tls ? host : (ip ? ip : host));

    ESP_LOGI(TAG, "Connecting (%s) to %s:%u",
             m_use_tls ? "TLS" : "TCP", connect_host, (unsigned)port);

    if (esp_transport_connect(m_t, connect_host, (int)port, 5000) != 0) {
        int terr = esp_transport_get_errno(m_t);
        ESP_LOGE(TAG, "esp_transport_connect failed, errno=%d (%s)", terr, strerror(terr));
        close();
        return false;
    }

    ESP_LOGI(TAG, "Connected");
    ESP_LOGI(TAG, "TCP keepalive %s; rx-silence limit %d s",
             m_keepAlive.keep_alive_enable ? "on (idle 10 s, interval 5 s, 3 probes)" : "off",
             (int)(RX_SILENCE_LIMIT_US / 1000000));
    m_lastRxUs = esp_timer_get_time();
    return true;
}

int StratumTransport::send(const void* data, size_t len)
{
    if (!m_t) {
        errno = ENOTCONN;
        return -1;
    }

    int ret = esp_transport_write(m_t, (const char*)data, (int)len, 30000);
    if (ret < 0) {
        int terr = esp_transport_get_errno(m_t);
        errno = (terr > 0) ? terr : ECONNRESET;
        ESP_LOGW(TAG, "write failed ret=%d errno=%d (%s)", ret, errno, strerror(errno));
        return -1;
    }

    // Optional: treat "0 bytes written" as timeout-ish if len>0
    if (ret == 0 && len > 0) {
        errno = EAGAIN;
        return -1;
    }

    return ret;
}

int StratumTransport::recv(void* buf, size_t len)
{
    if (!m_t) {
        errno = ENOTCONN;
        return -1;
    }

    int ret = esp_transport_read(m_t, (char*)buf, (int)len, 30000);

    if (ret > 0) {
        m_lastRxUs = esp_timer_get_time();
        return ret;
    }

    // esp_tcp_transport_err_t: timeout == 0 :contentReference[oaicite:4]{index=4}
    if (ret == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) {
        // trinetra12: nothing within this 30-s read. Fine if the pool spoke recently; if it has been silent for
        // RX_SILENCE_LIMIT_US, report a hard error (not EAGAIN) so receiveJsonRpcLine() gives up and the stratum
        // task closes the socket and reconnects.
        int64_t silentUs = esp_timer_get_time() - m_lastRxUs;
        if (silentUs > RX_SILENCE_LIMIT_US) {
            ESP_LOGW(TAG, "no data from the pool for %d s -> treating the link as dead, reconnecting",
                     (int)(silentUs / 1000000));
            errno = ETIMEDOUT;
            return -1;
        }
        errno = EAGAIN;
        return -1;
    }

    // esp_tcp_transport_err_t: FIN == -1 :contentReference[oaicite:5]{index=5}
    if (ret == ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN) {
        return 0; // peer closed
    }

    // other negatives: transport/tls error (can be mbedTLS codes etc.)
    int terr = esp_transport_get_errno(m_t); // get+clear :contentReference[oaicite:6]{index=6}
    errno = (terr > 0) ? terr : ECONNRESET;
    ESP_LOGW(TAG, "read failed ret=%d errno=%d (%s)", ret, errno, strerror(errno));
    return -1;
}

bool StratumTransport::isConnected()
{
    if (!m_t) {
        return false;
    }

    int r = esp_transport_poll_write(m_t, 0);
    return (r >= 0);
}

void StratumTransport::close()
{
    if (m_t) {
        esp_transport_close(m_t);
        esp_transport_destroy(m_t);
        m_t = nullptr;
    }
}

void StratumTransport::applyKeepAlive_()
{
    // trinetra12: the transport keeps the POINTER, so the config lives in the object (was a dead local variable)
    m_keepAlive = {};
    m_keepAlive.keep_alive_enable = Config::isStratumKeepaliveEnabled();
    m_keepAlive.keep_alive_idle = 10;
    m_keepAlive.keep_alive_interval = 5;
    m_keepAlive.keep_alive_count = 3;

    if (!m_keepAlive.keep_alive_enable) {
        return;
    }

    if (m_use_tls) {
        esp_transport_ssl_set_keep_alive(m_t, &m_keepAlive);
    } else {
        esp_transport_tcp_set_keep_alive(m_t, &m_keepAlive);
    }
}
