#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "esp_transport.h"


class StratumTransport {
public:
    explicit StratumTransport(bool use_tls);
    virtual ~StratumTransport();

    virtual bool connect(const char* host, const char* ip, uint16_t port);
    virtual int send(const void* data, size_t len);
    virtual int recv(void* buf, size_t len);
    virtual bool isConnected();
    virtual void close();

private:
    bool m_use_tls;
    void applyKeepAlive_();

protected:
    esp_transport_handle_t m_t;

    // trinetra12 (same fix as upstream develop): esp_transport_*_set_keep_alive() stores this POINTER (no copy)
    // and dereferences it later inside esp_transport_connect(), so the config must outlive applyKeepAlive_().
    // It used to be a local variable there: the keepalive settings were stack garbage, a dead pool link was
    // never detected, and the miner hashed into a half-open socket (2026-10-01 17:56, 11 minutes).
    esp_transport_keep_alive_t m_keepAlive = {};

    // trinetra12: esp_timer time (microseconds) of the last byte received from the pool; see recv()
    int64_t m_lastRxUs = 0;
};

class TcpStratumTransport : public StratumTransport {
public:
    TcpStratumTransport() : StratumTransport(false) {}
};

class TlsStratumTransport : public StratumTransport {
public:
    TlsStratumTransport() : StratumTransport(true) {}
};
