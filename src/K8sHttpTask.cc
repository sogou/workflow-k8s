#include <memory>
#include <string>
#include <fstream>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "workflow/HttpUtil.h"
#include "workflow/MD5Util.h"

#include "K8sGlobal.h"
#include "K8sHttpTask.h"
#include "K8sSSLWrapper.h"

#define HTTP_KEEPALIVE_DEFAULT (60 * 1000)
using namespace protocol;
using namespace std;

namespace k8s {

static SSL *__create_ssl(SSL_CTX *ssl_ctx)
{
    BIO *wbio;
    BIO *rbio;
    SSL *ssl;

    rbio = BIO_new(BIO_s_mem());
    if (rbio)
    {
        wbio = BIO_new(BIO_s_mem());
        if (wbio)
        {
            ssl = SSL_new(ssl_ctx);
            if (ssl)
            {
                SSL_set_bio(ssl, rbio, wbio);
                return ssl;
            }

            BIO_free(wbio);
        }

        BIO_free(rbio);
    }

    return NULL;
}

static SSL *__create_ssl(const string &cert, const string &key, string &error) {
    static auto *ssl_ctx = WFGlobal::get_ssl_client_ctx();
    SSL *ssl = __create_ssl(ssl_ctx);

    if(!ssl) {
        error.assign("__create_ssl failed");
        return nullptr;
    }

    if(cert.empty() || key.empty())
        return ssl;

    SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);

    if(SSL_use_certificate_file(ssl, cert.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_use_PrivateKey_file(ssl, key.c_str(), SSL_FILETYPE_PEM) != 1)
    {
        unsigned long err = ERR_get_error();
        error.resize(256);
        ERR_error_string_n(err, &error[0], 256);
        auto len = error.find('\0');
        if(len != string::npos)
            error.resize(len);

        SSL_free(ssl);
        return nullptr;
    }

    return ssl;
}

static string __get_file_md5(const string &filepath) {
    ifstream ifs(filepath, ios::in | ios::binary | ios::ate);
    string content;
    auto size = ifs.tellg();
    ifs.seekg(0, ios::beg);

    if(ifs && size > 0) {
        content.resize(size);
        if(!ifs.read(&content[0], size))
            content.clear();
    }
    ifs.close();
    return MD5Util::md5_string_32(content);
}

bool parse_json_to_pod(const char *first, const char *last, Pod &pod, string &err);
bool parse_json_to_podlist(const char *first, const char *last, PodList &pod, string &err);

struct SSLConnection : public WFConnection
{
    SSL *ssl_;
    K8sSSLWrapper wrapper_;
    SSLConnection(SSL *ssl) : ssl_(ssl), wrapper_(&wrapper_, ssl) { }
};

int K8sHttpResponse::append(const void *buf, size_t *size) {
    return is_watch_response ? append_watch(buf, size) : append_normal(buf, size);
}

int K8sHttpResponse::append_normal(const void *buf, size_t *size) {
    int ret = this->HttpResponse::append(buf, size);

    if(ret > 0) {
        if(handle_header() < 0)
            return -1;

        HttpChunkCursor cursor(this);
        const void *chunk;
        size_t size;

        while(cursor.next(&chunk, &size))
            body.append((const char *)chunk, size);

        auto *cfg = watch_ctx->config;
        auto deleter = [this](EventHandlerBase *hdl) {
            watch_ctx->protector->release_handler(hdl);
        };
        unique_ptr<EventHandlerBase, decltype(deleter)> hdl(
            watch_ctx->protector->acquire_handler(cfg), deleter);

        if(!hdl) {
            errno = EINVAL;
            return -1;
        }

        const char *first, *last;
        bool parse_ok, user_ret = true;
        string errors;
        PodList podlist;

        first = body.c_str();
        last = body.c_str() + body.size();
        parse_ok = parse_json_to_podlist(first, last, podlist, errors);

        if(parse_ok) {
            if(watch_ctx->need_reset) {
                user_ret = hdl->on_reset(cfg);
                watch_ctx->need_reset = false;
            }

            for(size_t i = 0; i < podlist.items.size() && user_ret; i++)
                user_ret = hdl->on_pod(cfg, podlist.items[i]);

            if(!podlist.resource_version.empty())
                watch_ctx->resource_version = podlist.resource_version;
        }
        else {
            user_ret = hdl->on_error(cfg, WatchError {
                .type = ERR_JSON,
                { .json = {first, last} }
            });
            watch_ctx->current_status = ERR_JSON;
        }

        if(user_ret == false)
            watch_ctx->user_exit = true;
    }

    return ret;
}

int K8sHttpResponse::append_header(const void *buf, size_t *size) {
    size_t len;
    size_t pos;
    int ret;

    header.append((const char *)buf, *size);
    pos = header.find("\r\n\r\n");
    if(pos == string::npos)
        return 0;

    pos += 4;
    len = pos;
    ret = this->HttpResponse::append(header.c_str(), &len);

    if(ret < 0)
        return -1;

    if(handle_header() < 0)
        return -1;

    this->HttpResponse::end_parsing();
    header_complete = true;

    len = header.size() - pos;
    // TODO what if len not totally appended
    return append_body(header.c_str() + pos, &len);
}

int K8sHttpResponse::append_body(const void *buf, size_t *size) {
    const char *buffer = (const char *)buf;
    size_t buf_pos = 0;

    while(buf_pos < *size) {
        if(chunk_size == 0 && !is_last_chunk) {
            while(buf_pos < *size) {
                chunk_line.push_back(buffer[buf_pos++]);
                if(buffer[buf_pos-1] == '\n') {
                    chunk_size = strtoll(chunk_line.c_str(), nullptr, 16);
                    chunk_recv = 0;
                    if(chunk_size == 0)
                        is_last_chunk = true;
                    break;
                }
            }
            continue;
        }

        if(chunk_recv < chunk_size && buf_pos < *size) {
            size_t max_append = min(*size - buf_pos, chunk_size - chunk_recv);
            body.append(buffer + buf_pos, max_append);
            buf_pos += max_append;
            chunk_recv += max_append;
        }

        while(buf_pos < *size && chunk_recv < chunk_size + 2)
            buf_pos++, chunk_recv++;

        if(chunk_recv == chunk_size + 2) {
            chunk_recv = 0;
            chunk_line.clear();
            if(chunk_size == 0) {
                *size = buf_pos;
                return 1; // complete this message
            }
            chunk_size = 0;
        }
        else if(chunk_recv > chunk_size + 2)
            assert(false);
    }
    return 0;
}

int K8sHttpResponse::append_watch(const void *buf, size_t *size) {
    size_t start_pos, end_pos;
    string errors;
    bool parse_ok, user_ret = true;
    int ret;

    ret = header_complete ? append_body(buf, size) : append_header(buf, size);

    if(ret < 0)
        return ret;

    auto *cfg = watch_ctx->config;
    auto deleter = [this](EventHandlerBase *hdl) {
        watch_ctx->protector->release_handler(hdl);
    };
    unique_ptr<EventHandlerBase, decltype(deleter)> hdl(
        watch_ctx->protector->acquire_handler(cfg), deleter);

    if(!hdl) {
        errno = EINVAL;
        return -1;
    }

    start_pos = 0;
    while(true) {
        end_pos = body.find_first_of("\r\n", start_pos);
        if(end_pos == string::npos)
            break;

        const char *first = body.c_str() + start_pos;
        const char *last = body.c_str() + end_pos;
        start_pos = end_pos + 1;

        if(first == last)
            continue;

        Pod pod;
        parse_ok = parse_json_to_pod(first, last, pod, errors);
        if(parse_ok) {
            if(watch_ctx->need_reset) {
                user_ret = hdl->on_reset(cfg);
                watch_ctx->need_reset = false;
            }

            if(user_ret)
                user_ret = hdl->on_pod(cfg, pod);

            if(user_ret && pod.error_status.code == 410) {
                watch_ctx->need_reset = true;
                watch_ctx->resource_version.clear();
                watch_ctx->current_status = ERR_INNER;
                errno = EBADMSG;
                return -1;
            }
        }
        else {
            user_ret = hdl->on_error(cfg, WatchError {
                .type = ERR_JSON,
                { .json = {first, last} }
            });
            watch_ctx->current_status = ERR_JSON;
        }

        if(user_ret == false) {
            watch_ctx->user_exit = true;
            errno = EINVAL;
            return -1;
        }

        if(!pod.metadata.resource_version.empty())
            watch_ctx->resource_version = pod.metadata.resource_version;
    }

    body.erase(0, start_pos);
    return ret;
}

int K8sHttpResponse::handle_header() {
    const char *status_code = this->HttpResponse::get_status_code();
    int code = status_code ? atoi(status_code) : 0;

    if(is_watch_response && !this->HttpResponse::is_chunked()) {
        errno = EBADMSG;
        return -1;
    }

    // auto retry when Http 410 Gone
    if(code == 410 && !watch_ctx->resource_version.empty()) {
        // call on_reset when next success
        watch_ctx->need_reset = true;
        watch_ctx->resource_version.clear();
        watch_ctx->current_status = ERR_INNER;
        errno = EBADMSG;
        return -1;
    }

    if(code != 200) {
        watch_ctx->current_status = ERR_HTTP;
        watch_ctx->error = WatchError {
            .type = ERR_HTTP,
            { .http = {code} }
        };
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

int ComplexK8sHttpTask::init_ssl_connection()
{
    const string &cert = watch_ctx->config->cert;
    const string &key = watch_ctx->config->key;
    WFConnection *conn;

    watch_ctx->ssl_errmsg.clear();
    SSL *ssl = __create_ssl(cert, key, watch_ctx->ssl_errmsg);
    if(!ssl) {
        watch_ctx->current_status = ERR_SSL;
        watch_ctx->error = WatchError {
            .type = ERR_SSL,
            { .ssl = { watch_ctx->ssl_errmsg.c_str() } }
        };
        return -1;
    }

    SSL_set_tlsext_host_name(ssl, uri_.host);
    SSL_set_connect_state(ssl);

    conn = this->WFComplexClientTask::get_connection();
    SSLConnection *ssl_conn = new SSLConnection(ssl);

    auto deleter = [] (void *ctx) {
        SSLConnection *ssl_conn = (SSLConnection *)ctx;
        SSL_free(ssl_conn->ssl_);
        delete ssl_conn;
    };

    conn->set_context(ssl_conn, deleter);
    return 0;
}

ProtocolMessage *ComplexK8sHttpTask::get_message_out() {
    auto *req = this->get_req();
    size_t body_size = req->get_output_body_size();

    req->set_header_pair("Content-Length", to_string(body_size));
    req->set_header_pair("Connection", "Keep-Alive");
    auto *msg = (ProtocolMessage *)this->WFComplexClientTask::message_out();
    return msg;
}

CommMessageOut *ComplexK8sHttpTask::message_out() {
    long long seqid = this->get_seq();

    if(seqid == 0 && is_ssl) {
        if(init_ssl_connection() < 0)
            return nullptr;

        // nullptr means get a handshaker
        auto *wrapper = get_ssl_wrapper(nullptr);
        wrapper->set_first();
        return wrapper;
    }

    auto *msg = this->get_message_out();
    return is_ssl ? get_ssl_wrapper(msg) : msg;
}

CommMessageIn *ComplexK8sHttpTask::message_in() {
    auto *msg = (ProtocolMessage *)this->WFComplexClientTask::message_in();
    long long seqid = this->get_seq();

    if(seqid == 0 && is_ssl) {
        auto *out_msg = this->get_message_out();
        auto *wrapper = get_ssl_wrapper(msg);
        wrapper->set_first();
        wrapper->set_out_msg(out_msg);
        return wrapper;
    }

    return is_ssl ? get_ssl_wrapper(msg) : msg;
}

int ComplexK8sHttpTask::keep_alive_timeout() {
    return HTTP_KEEPALIVE_DEFAULT;
}

void ComplexK8sHttpTask::init_failed() { }

bool ComplexK8sHttpTask::init_success() {
    HttpRequest *client_req = this->get_req();
    string request_uri;
    string header_host;

    if (uri_.scheme && strcasecmp(uri_.scheme, "http") == 0)
        is_ssl = false;
    else if (uri_.scheme && strcasecmp(uri_.scheme, "https") == 0)
        is_ssl = true;
    else
    {
        this->state = WFT_STATE_TASK_ERROR;
        this->error = WFT_ERR_URI_SCHEME_INVALID;
        //this->set_empty_request();
        return false;
    }

    request_uri = (uri_.path && uri_.path[0]) ? uri_.path : "/";

    if (uri_.query && uri_.query[0])
        request_uri.append("?").append(uri_.query);

    if (uri_.host && uri_.host[0])
        header_host = uri_.host;

    if (uri_.port && uri_.port[0])
    {
        int port = atoi(uri_.port);

        if ((is_ssl && port != 443) || (!is_ssl && port != 80))
            header_host.append(":").append(uri_.port);
    }

    string info("http-k8s|remote:");
    info += is_ssl ? "https://" : "http://";
    info += header_host;
    info += "|auth:";

    auto *cfg = watch_ctx->config;
    // TODO bug when cert/key file change between set info and create ssl
    if(is_ssl && !cfg->cert.empty() && !cfg->key.empty()) {
        info += __get_file_md5(cfg->cert);
        info += ",";
        info += __get_file_md5(cfg->key);
    }
    this->WFComplexClientTask::set_info(info);

    this->WFComplexClientTask::set_transport_type(TT_TCP);
    client_req->set_request_uri(request_uri.c_str());
    client_req->set_header_pair("Host", header_host.c_str());

    return true;
}

bool ComplexK8sHttpTask::finish_once()
{
    if(this->state == WFT_STATE_SUCCESS)
        this->disable_retry();

    return true;
}

WFConnection *ComplexK8sHttpTask::get_connection() const
{
    WFConnection *conn = this->WFComplexClientTask::get_connection();

    if (conn && is_ssl)
        return (SSLConnection *)conn->get_context();

    return conn;
}

K8sSSLWrapper *ComplexK8sHttpTask::get_ssl_wrapper(ProtocolMessage *msg) const {
    SSLConnection *conn = (SSLConnection *)this->get_connection();
    if(msg == nullptr)
        msg = &conn->wrapper_;
    conn->wrapper_.~K8sSSLWrapper();
    new (&conn->wrapper_) K8sSSLWrapper(msg, conn->ssl_);
    return &conn->wrapper_;
}

} // namespace k8s
