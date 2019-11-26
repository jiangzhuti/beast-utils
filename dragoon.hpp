/*
 * =====================================================================================
 *
 *       Filename:  dragoon.hpp
 *
 *        Created:  2019-11-22
 *        License:  WTFPL
 *
 *         Author:  李强, liqiang01@qiyi.com
 *
 * =====================================================================================
 */

#pragma once

#include "root_certificates.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/optional.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <mutex>

#include "fail.hpp"

namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
namespace http = boost::beast::http;    // from <boost/beast/http.hpp>

//------------------------------------------------------------------------------

// Performs an HTTP GET and prints the response
class dragoon : public std::enable_shared_from_this<dragoon>
{
public:
    using res_type = boost::beast::http::response<boost::beast::http::dynamic_body>;
    using tcp = boost::asio::ip::tcp;
    using response_handler = std::function<void (res_type&, std::shared_ptr<dragoon>)>;
    using error_handler = std::function<void (boost::beast::error_code, std::shared_ptr<dragoon>)>;
    response_handler m_rh;
    error_handler m_eh;
public:
    boost::optional<boost::beast::ssl_stream<boost::beast::tcp_stream>> m_stream;
    tcp::resolver m_resolver;
    boost::beast::flat_buffer m_buffer; // (Must persist between reads)
    http::request<http::string_body> m_req;
    boost::optional<res_type> m_res;
    uint32_t m_conn_timeout_ms { 0 };
    uint32_t m_rw_timeout_ms { 0 };
    ssl::context& m_ctx;

  public:
    // Resolver and stream require an io_context
    explicit dragoon(boost::asio::io_context& ioc, ssl::context& ctx)
        : m_resolver(boost::asio::make_strand(ioc))
        , m_stream(boost::in_place_init, boost::asio::make_strand(ioc), ctx)
        , m_ctx(ctx)
    { }

    ~dragoon()
    {
    }

    void on_error(error_handler eh)
    {
        m_eh = std::move(eh);
    }
    void on_response(response_handler rh)
    {
        m_rh = std::move(rh);
    }
    void post(const std::string& host,
              const std::string& port,
              const std::string& target,
              int version,
              const std::string& data,
              const std::string& content_type,
              uint32_t conn_timeout_ms,
              uint32_t rw_timeout_ms)
    {
        run(boost::beast::http::verb::post, host, port, target, version, data, content_type, conn_timeout_ms, rw_timeout_ms, host);
        return;
    }

    void get(const std::string& host,
             const std::string& port,
             const std::string& target,
             int version,
             uint32_t conn_timeout_ms,
             uint32_t rw_timeout_ms)
    {
        run(boost::beast::http::verb::get, host, port, target, version, std::string{}, std::string{}, conn_timeout_ms, rw_timeout_ms, host);
        return;
    }
    void post(const std::string& host,
              const std::string& port,
              const std::string& target,
              int version,
              const std::string& data,
              const std::string& content_type,
              uint32_t conn_timeout_ms,
              uint32_t rw_timeout_ms,
              const std::string& host_field)
    {
        run(boost::beast::http::verb::post, host, port, target, version, data, content_type, conn_timeout_ms, rw_timeout_ms, host_field);
        return;
    }

    void get(const std::string& host,
             const std::string& port,
             const std::string& target,
             int version,
             uint32_t conn_timeout_ms,
             uint32_t rw_timeout_ms,
             const std::string& host_field)
    {
        run(boost::beast::http::verb::get, host, port, target, version, std::string{}, std::string{}, conn_timeout_ms, rw_timeout_ms, host_field);
        return;
    }
private:
    void run(boost::beast::http::verb method,
             const std::string& host,
             const std::string& port,
             const std::string& target,
             int version,
             const std::string& data,
             const std::string& content_type,
             uint32_t conn_timeout_ms,
             uint32_t rw_timeout_ms,
             const std::string& host_field)
    {
        // Set SNI Hostname (many hosts need this to handshake successfully)
        if (!SSL_set_tlsext_host_name(m_stream->native_handle(), host_field.c_str())) {
            boost::beast::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
            fail(ec, FAIL_LOCATION);
            return;
        }
        if (method != boost::beast::http::verb::get &&
            method != boost::beast::http::verb::post) {
            return; // unreachable
        }
        //clear
        auto executor = m_stream->get_executor();
        m_stream.emplace(executor, m_ctx);
        m_res.emplace();
        m_buffer.consume(m_buffer.size());

        //make request
        m_req.version(version);
        m_req.method(method);
        m_req.target(target);
        m_req.set(http::field::host, host_field);
        m_req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        if (method == boost::beast::http::verb::post) {
            m_req.keep_alive(true);
            m_req.chunked(true);
            m_req.set(boost::beast::http::field::content_type, content_type);
            m_req.body() = data;
            m_req.prepare_payload();
        }

        //resolve host
        m_conn_timeout_ms = conn_timeout_ms;
        m_rw_timeout_ms = rw_timeout_ms;
        boost::beast::get_lowest_layer(*m_stream).expires_never();
        m_resolver.async_resolve(host,
                                 port,
                                 boost::beast::bind_front_handler(&dragoon::on_resolve,
                                                                  shared_from_this()));
    }

    void shutdown(bool from_read)
    {
        auto self = shared_from_this();
        m_stream->async_shutdown([self, this, from_read](boost::beast::error_code ec) {
                                     if (ec == boost::asio::error::eof) {
                                         // Rationale:
                                         // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
                                         ec = {};
                                     }
                                     if (ec) {
                                         // fail(ec, FAIL_LOCATION);
                                     }
                                     if (m_stream && boost::beast::get_lowest_layer(*m_stream).socket().is_open()) {
                                         boost::beast::get_lowest_layer(*m_stream).socket().shutdown(tcp::socket::shutdown_both, ec);
                                         if (ec && ec != boost::beast::errc::not_connected) {
                                             return fail(ec, FAIL_LOCATION);
                                         }
                                         boost::beast::get_lowest_layer(*m_stream).socket().cancel(ec);
                                         boost::beast::get_lowest_layer(*m_stream).socket().close(ec);
                                     }
                                     if (from_read) {
                                         if (m_rh != nullptr) {
                                             m_rh(*m_res, self);
                                         }
                                     }
                                 });
    }

    void on_resolve(boost::beast::error_code ec, tcp::resolver::results_type results)
    {
        if (ec) {
            if (m_eh != nullptr) {
                m_eh(ec, shared_from_this());
            }
            return;
        }
        boost::beast::error_code ec_;

        boost::beast::get_lowest_layer(*m_stream).socket().set_option(boost::asio::socket_base::reuse_address(true), ec_);
        if (ec_) {
            return fail(ec_, FAIL_LOCATION);
        }
        if (m_conn_timeout_ms > 0) {
            boost::beast::get_lowest_layer(*m_stream).expires_after(std::chrono::milliseconds(m_conn_timeout_ms));
        }
        // Make the connection on the IP address we get from a lookup
        boost::beast::get_lowest_layer(*m_stream).async_connect(results,
                                                                boost::beast::bind_front_handler(&dragoon::on_connect,
                                                                                                 shared_from_this()));
    }

    void on_connect(boost::beast::error_code ec, tcp::resolver::results_type::endpoint_type)
    {
        if (ec) {
            if (m_eh != nullptr) {
                m_eh(ec, shared_from_this());
            }
            return;
        }
        // Perform the SSL handshake
        m_stream->async_handshake(
            ssl::stream_base::client,
            boost::beast::bind_front_handler(&dragoon::on_handshake,
                                             shared_from_this()));
    }

    void on_handshake(boost::beast::error_code ec)
    {
        if (ec) {
            if (m_eh != nullptr) {
                m_eh(ec, shared_from_this());
            }
            return;
        }
        if (m_rw_timeout_ms > 0) {
            boost::beast::get_lowest_layer(*m_stream).expires_after(std::chrono::milliseconds(m_rw_timeout_ms));
        } else {
            boost::beast::get_lowest_layer(*m_stream).expires_never();
        }

        // Send the HTTP request to the remote host
        http::async_write(*m_stream, m_req,
                          boost::beast::bind_front_handler(&dragoon::on_write,
                                                           shared_from_this()));
    }
    void on_write(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if (ec) {
            if (m_eh != nullptr) {
                m_eh(ec, shared_from_this());
            }
            return;
        }
        // Receive the HTTP response
        http::async_read(*m_stream,
                         m_buffer,
                         *m_res,
                         boost::beast::bind_front_handler(&dragoon::on_read,
                                                          shared_from_this()));
    }

    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec) {
            if (m_eh != nullptr) {
                m_eh(ec, shared_from_this());
            }
            return;
        }
        shutdown(true);
    }
};

//------------------------------------------------------------------------------
/*
int main(int argc, char** argv)
{
    // Check command line arguments.
    if(argc != 4 && argc != 5)
    {
        std::cerr <<
            "Usage: http-client-async-ssl <host> <port> <target> [<HTTP version: 1.0 or 1.1(default)>]\n" <<
            "Example:\n" <<
            "    http-client-async-ssl www.example.com 443 /\n" <<
            "    http-client-async-ssl www.example.com 443 / 1.0\n";
        return EXIT_FAILURE;
    }
    auto const host = argv[1];
    auto const port = argv[2];
    auto const target = argv[3];
    int version = argc == 5 && !std::strcmp("1.0", argv[4]) ? 10 : 11;

    // The io_context is required for all I/O
    boost::asio::io_context ioc;

    // The SSL context is required, and holds certificates
    ssl::context ctx{ssl::context::sslv23_client};

    // This holds the root certificate used for verification
    load_root_certificates(ctx);
    
    // Verify the remote server's certificate
    ctx.set_verify_mode(ssl::verify_peer);

    // Launch the asynchronous operation
    std::make_shared<session>(ioc, ctx)->run(host, port, target, version);

    // Run the I/O service. The call will return when
    // the get operation is complete.
    ioc.run();

    return EXIT_SUCCESS;
}
*/
