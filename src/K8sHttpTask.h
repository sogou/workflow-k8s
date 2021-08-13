#ifndef K8S_HTTP_TASK_H
#define K8S_HTTP_TASK_H

#include <string>
#include "workflow/WFTaskFactory.h"
#include "workflow/HttpMessage.h"
#include "K8sGlobal.h"

namespace k8s {

using K8sHttpRequest = protocol::HttpRequest;

class K8sHttpResponse : public protocol::HttpResponse {
protected:
    virtual int append(const void *buf, size_t *size);

public:
    void set_watch_response(bool flag) { is_watch_response = flag; }
    void set_watch_context(WatchContext *ctx) { watch_ctx = ctx; }

private:
    int append_normal(const void *buf, size_t *size);
    int append_watch(const void *buf, size_t *size);

    int append_header(const void *buf, size_t *size);
    int append_body(const void *buf, size_t *size);
    int handle_header();

private:
    bool header_complete{false};
    bool is_watch_response{false};
    bool is_last_chunk{false};
    size_t chunk_size{0};
    size_t chunk_recv{0};
    std::string body;
    std::string header;
    std::string chunk_line;
    WatchContext *watch_ctx{nullptr};
};

using K8sHttpTask = WFNetworkTask<K8sHttpRequest, K8sHttpResponse>;
using k8s_http_callback_t = std::function<void (K8sHttpTask*)>;

class ComplexK8sHttpTask : public WFComplexClientTask<K8sHttpRequest, K8sHttpResponse> {
public:
    ComplexK8sHttpTask(int retry_max, k8s_http_callback_t&& callback)
        : WFComplexClientTask(retry_max, std::move(callback))
    {
        auto *client_req = this->get_req();

        client_req->set_method("GET");
        client_req->set_http_version("HTTP/1.1");
        client_req->set_header_pair("Accept-Encoding", "identity");
        client_req->set_header_pair("Accept", "application/json");
    }

    void set_watch_context(WatchContext *ctx) { watch_ctx = ctx; }

protected:
    virtual CommMessageOut *message_out() override;
    virtual CommMessageIn *message_in() override;
    virtual int keep_alive_timeout() override;
    virtual bool init_success() override;
    virtual void init_failed() override;
    virtual bool finish_once() override;

    virtual WFConnection *get_connection() const;

private:
    class K8sSSLWrapper *get_ssl_wrapper(protocol::ProtocolMessage *msg) const;
    protocol::ProtocolMessage *get_message_out();
    int init_ssl_connection();

    WatchContext *watch_ctx{nullptr};
    bool is_ssl;
};

} // namespace protocol

#endif // K8S_HTTP_TASK_H
