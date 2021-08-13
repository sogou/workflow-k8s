#include "K8sSSLWrapper.h"

namespace k8s {

static int __ssl_handshake(const void *buf, size_t *size, SSL *ssl,
                           char **ptr, long *len)
{
    BIO *wbio = SSL_get_wbio(ssl);
    BIO *rbio = SSL_get_rbio(ssl);
    int ret;

    if (BIO_reset(wbio) <= 0)
        return -1;

    ret = BIO_write(rbio, buf, *size);
    if (ret <= 0)
        return -1;

    *size = ret;
    ret = SSL_do_handshake(ssl);
    if (ret <= 0)
    {
        ret = SSL_get_error(ssl, ret);
        if (ret != SSL_ERROR_WANT_READ)
        {
            if (ret != SSL_ERROR_SYSCALL)
                errno = -ret;

            return -1;
        }

        ret = 0;
    }

    *len = BIO_get_mem_data(wbio, ptr);
    if (*len < 0)
        return -1;

    return ret;
}

int K8sSSLWrapper::append(const void *buf, size_t *size) {
    char *ptr;
    int ret;
    long len;
    long n;

    ret = __ssl_handshake(buf, size, this->ssl, &ptr, &len);
    if (ret < 0)
        return -1;

    if (len > 0)
        n = this->ProtocolMessage::feedback(ptr, len);
    else
        n = 0;

    if (n == len) {
        // last feedback ok, check out_msg
        // ret > 0: handshake done
        if(ret > 0 && out_msg) {
            int fret = this->feedback_msg();
            out_msg = nullptr;

            if(fret < 0)
                return fret;
        }

        return this->append_message();
    }

    if (n >= 0)
        errno = EAGAIN;

    return -1;
}

int K8sSSLWrapper::encode(struct iovec vectors[], int max) {
    BIO *wbio = SSL_get_wbio(this->ssl);

    if (BIO_reset(wbio) <= 0)
        return -1;

    if(first) {
        protocol::SSLHandshaker h(this->ssl);
        return h.encode(vectors, max);
    }

    return this->SSLWrapper::encode(vectors, max);
}

int K8sSSLWrapper::feedback_msg() {
    constexpr int MAX_VEC = 64;
    struct iovec v[MAX_VEC];

    K8sSSLWrapper encoder(out_msg, this->ssl);
    int ret = encoder.encode(v, MAX_VEC);
    int len;

    if(ret < 0)
        return ret;

    for(int i = 0; i < ret; i++) {
        const char *vbase = (const char *)v[i].iov_base;
        len = this->ProtocolMessage::feedback(vbase, v[i].iov_len);
        if(len != (int)v->iov_len) {
            errno = EAGAIN;
            return -1;
        }
    }
    return 0;
}

} // namespace k8s
