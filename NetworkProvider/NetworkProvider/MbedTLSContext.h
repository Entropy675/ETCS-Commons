#ifndef MBEDTLSWRAPPER_H__
#define MBEDTLSWRAPPER_H__

#include "../../../ontology.h"
#include <iostream>
#include <cstring>
#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/psa_util.h"

#define PLATFORM_SETUP_CERTS "/etc/ssl/certs/ca-certificates.crt"
    
class MbedTLSContext : public ParserBase<MbedTLSContext>
{
    WIRE_TYPE_IDENTITY(MbedTLSContext);

    enum class State : uint8_t
    {
        Idle,
        Handshaking,
        Ready,
        Decrypting,
        Complete,
        Error
    };

    bool SetupSystemCerts()
    {
        int ret = mbedtls_x509_crt_parse_file(&cacert_, PLATFORM_SETUP_CERTS);
        if (ret < 0) { logError("x509_crt_parse_file", ret); return false; }
        return true;
    }

private:
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config  conf_;
    mbedtls_x509_crt    cacert_;

    State state_ = State::Idle;

    ETCS::MirrorBuffer* io_  = nullptr;
    ETCS::SignalContext  ctx_;

    char   plain_[ETCS::Buffer::bufsize * 4];
    size_t plain_len_ = 0;

    char hostname_[MAX_TAG_BUFFER_SIZE];

    static int bioSend(void* self_, const unsigned char* buf, size_t len)
    {
        auto* self = static_cast<MbedTLSContext*>(self_);
        if (!self->io_) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

        size_t to_write = std::min(len, (size_t)ETCS::Buffer::bufsize);

        ETCS::IOSubmission sub;
        sub.op         = ETCS::IOOp::Send;
        sub.fd         = self->io_->writeFd();
        sub.buffer     = const_cast<unsigned char*>(buf);
        sub.buffer_len = to_write;
        sub.priority   = static_cast<int>(ETCS::Priority::High);
        sub.ctx        = self->ctx_;
        sub.callback   = [](ETCS::IOCompletion c)
        {
            if (c.result < 0)
                std::cerr << "[MbedTLSContext::bioSend] Send failed: " << c.result << "\n";
        };

        self->getThreadPool().submit(std::move(sub));
        return static_cast<int>(to_write);
    }

    static int bioRecv(void* self_, unsigned char* buf, size_t len)
    {
        auto* self = static_cast<MbedTLSContext*>(self_);
        if (!self->io_) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

        size_t to_read = std::min(len, (size_t)ETCS::Buffer::bufsize);

        ETCS::IOSubmission sub;
        sub.op         = ETCS::IOOp::Recv;
        sub.fd         = self->io_->readFd();
        sub.buffer     = buf;
        sub.buffer_len = to_read;
        sub.priority   = static_cast<int>(ETCS::Priority::High);
        sub.ctx        = self->ctx_;
        sub.callback   = [self, buf](ETCS::IOCompletion c) mutable
        {
            if (c.result <= 0) return;

            ETCS::Buffer frame;
            std::memcpy(frame.buf, buf, c.result);
            frame.written = static_cast<size_t>(c.result);
            self->io_->writeRaw(frame);
        };

        self->getThreadPool().submit(std::move(sub));
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    void initContexts()
    {
        psa_crypto_init();
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&cacert_);
    }

    void freeContexts()
    {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&cacert_);
    }

    bool setupConfig()
    {
        int ret = mbedtls_ssl_config_defaults(
            &conf_,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT
        );
        if (ret != 0) { logError("ssl_config_defaults", ret); return false; }

        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf_, &cacert_, nullptr);

        ret = mbedtls_ssl_setup(&ssl_, &conf_);
        if (ret != 0) { logError("ssl_setup", ret); return false; }

        ret = mbedtls_ssl_set_hostname(&ssl_, hostname_);
        if (ret != 0) { logError("ssl_set_hostname", ret); return false; }

        mbedtls_ssl_set_bio(&ssl_, this, bioSend, bioRecv, nullptr);

        return true;
    }

    // cerr unreliable, use ETCS_LOG instead
    void logError(const char* context, int ret)
    {
        char errbuf[256];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[MbedTLSContext] " << context << " failed: " << errbuf << "\n";
    }

public:
    
    MbedTLSContext()
    {
        std::memset(hostname_, 0, sizeof(hostname_));
        std::memset(plain_,    0, sizeof(plain_));
        initContexts();
    }

    virtual ~MbedTLSContext()
    {
        freeContexts();
    }

    void SetHostname(const char* hostname)
    {
        size_t len = std::min(std::strlen(hostname), sizeof(hostname_) - 1);
        std::memcpy(hostname_, hostname, len);
        hostname_[len] = '\0';
    }

    bool LoadCACert(const unsigned char* pem, size_t len)
    {
        int ret = mbedtls_x509_crt_parse(&cacert_, pem, len);
        if (ret != 0) { logError("x509_crt_parse", ret); return false; }
        return true;
    }

    void ParseConcrete(ETCS::MirrorBuffer& io, ETCS::SignalContext ctx) override
    {
        io_  = &io;
        ctx_ = ctx;

        if (state_ == State::Idle)
        {
            if (!setupConfig()) { state_ = State::Error; return; }
            state_ = State::Handshaking;
        }

        if (state_ == State::Handshaking)
        {
            int ret = 0;
            do {
                ret = mbedtls_ssl_handshake(&ssl_);
                if (ctx.isInterrupted() || ctx.isTerminated()) { state_ = State::Error; return; }
            } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

            if (ret != 0) { logError("ssl_handshake", ret); state_ = State::Error; return; }
            state_ = State::Ready;
        }

        if (state_ == State::Ready || state_ == State::Decrypting)
        {
            state_ = State::Decrypting;

            while (true)
            {
                if (ctx.isInterrupted() || ctx.isTerminated()) { state_ = State::Error; return; }

                unsigned char chunk[ETCS::Buffer::bufsize];
                int ret = mbedtls_ssl_read(&ssl_, chunk, sizeof(chunk));

                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                    continue;

                if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0)
                {
                    state_ = State::Complete;
                    flushPlain(io);
                    return;
                }

                if (ret < 0) { logError("ssl_read", ret); state_ = State::Error; return; }

                size_t incoming = static_cast<size_t>(ret);
                if (plain_len_ + incoming >= sizeof(plain_))
                    flushPlain(io);

                std::memcpy(plain_ + plain_len_, chunk, incoming);
                plain_len_ += incoming;
            }
        }
    }

    bool ResetConcrete() override
    {
        freeContexts();
        initContexts();

        io_        = nullptr;
        plain_len_ = 0;
        state_     = State::Idle;

        std::memset(plain_, 0, sizeof(plain_));
        return true;
    }

    State GetState() const { return state_; }

private:
    void flushPlain(ETCS::MirrorBuffer& io)
    {
        if (plain_len_ == 0) return;

        size_t offset = 0;
        while (offset < plain_len_)
        {
            ETCS::Buffer frame;
            size_t chunk = std::min(plain_len_ - offset, (size_t)ETCS::Buffer::bufsize);
            std::memcpy(frame.buf, plain_ + offset, chunk);
            frame.written = chunk;
            io.writeRaw(frame);
            offset += chunk;
        }

        plain_len_ = 0;
        std::memset(plain_, 0, sizeof(plain_));
    }
};

#endif
