#ifndef K8S_SSL_WRAPPER_H
#define K8S_SSL_WRAPPER_H

#include "workflow/SSLWrapper.h"

namespace k8s {

class K8sSSLWrapper : public protocol::SSLWrapper {
public:
    K8sSSLWrapper(protocol::ProtocolMessage *msg, SSL *ssl)
        : SSLWrapper(msg, ssl) {}
    K8sSSLWrapper(K8sSSLWrapper &&) = delete;
    K8sSSLWrapper& operator=(K8sSSLWrapper &&) = delete;

    void set_first() { first = true; }
    void set_out_msg(protocol::ProtocolMessage *msg) { out_msg = msg; }

protected:
    virtual int append(const void *buf, size_t *size) override;
    virtual int encode(struct iovec vectors[], int max) override;

private:
    int feedback_msg();

private:
    bool first{false};
    protocol::ProtocolMessage *out_msg{nullptr};
};

} // namespace k8s

#endif // K8S_SSL_WRAPPER_H
