/*
 * =====================================================================================
 *
 *       Filename:  stalker.hpp
 *
 *        Created:  2019-11-22
 *        License:  WTFPL
 *
 *         Author:  李强, liqiang01@qiyi.com
 *
 * =====================================================================================
 */

#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/optional.hpp>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include "fail.hpp"

/*

usage:
void on_error(boost::beast::error_code ec, std::shared_ptr<stalker> s)
{
    //error handling
}

void on_response(stalker::res_type& res, std::shared_ptr<stalker> s)
{
    std::cout << res;
    if (some_condition) {
        s->post(...);
    }
}
auto my_stalker = std::make_shared<stalker>(ioc);
my_stalker->on_error(on_error);
my_stalker->on_response(on_response);
my_stalker->post(host, port, target, version, data, content_type, 500, 500);

*/

// simple http client
// support async get & post, with timeout
// not thread-safe
class stalker : public std::enable_shared_from_this<stalker>
{
public:
    using res_type = boost::beast::http::response<boost::beast::http::dynamic_body>;
    using tcp = boost::asio::ip::tcp;
    using response_handler = std::function<void (res_type&, std::shared_ptr<stalker>)>;
    using error_handler = std::function<void (boost::beast::error_code, std::shared_ptr<stalker>)>;
private:
    tcp::resolver m_resolver;
    boost::beast::tcp_stream m_stream;
    boost::beast::flat_buffer m_buffer;
    boost::beast::http::request<boost::beast::http::string_body> m_req;
    boost::optional<res_type> m_res;
    response_handler m_rh;
    error_handler m_eh;
    unsigned m_conn_timeout_ms {0};
    unsigned m_rw_timeout_ms {0};

  public:
    explicit stalker(boost::asio::io_context& ioc) : m_resolver(boost::asio::make_strand(ioc)),
                                                     m_stream(boost::asio::make_strand(ioc))
    { }

    ~stalker()
    {
        this->shutdown();
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
        if (method != boost::beast::http::verb::get &&
            method != boost::beast::http::verb::post) {
            return; // unreachable
        }
        //clear
        m_res.emplace();
        m_buffer.consume(m_buffer.size());

        //make request
        m_req.version(version);
        m_req.method(method);
        m_req.target(target);
        m_req.set(boost::beast::http::field::host, host_field);
        m_req.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
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
        m_stream.expires_never();
        m_resolver.async_resolve(host,
                                 port,
                                 boost::beast::bind_front_handler(&stalker::on_resolve,
                                                                  shared_from_this()));
    }
    
    
    void on_resolve(boost::beast::error_code ec, tcp::resolver::results_type results)
    {
        if (ec) {
            if (m_eh != nullptr) {
                m_eh(ec, shared_from_this());
            }
            return;
        }

        if (m_conn_timeout_ms > 0) {
            m_stream.expires_after(std::chrono::milliseconds(m_conn_timeout_ms));
        }
        m_stream.async_connect(results,
                               boost::beast::bind_front_handler(&stalker::on_connect,
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

        if (m_rw_timeout_ms > 0) {
            m_stream.expires_after(std::chrono::milliseconds(m_rw_timeout_ms));
        } else {
            m_stream.expires_never();
        }
        boost::beast::http::async_write(m_stream,
                                        m_req,
                                        boost::beast::bind_front_handler(&stalker::on_write,
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
        boost::beast::http::async_read(m_stream,
                                       m_buffer,
                                       *m_res,
                                       boost::beast::bind_front_handler(&stalker::on_read,
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
        shutdown();
        if (m_rh != nullptr) {
            m_rh(*m_res, shared_from_this());
        }
    }

    void shutdown()
    {
        boost::beast::error_code ec;
        m_resolver.cancel();
        if (m_stream.socket().is_open()) {
            m_stream.socket().shutdown(tcp::socket::shutdown_both, ec);
            if (ec && ec != boost::system::errc::not_connected) {
                return fail(ec, FAIL_LOCATION);
            }
            m_stream.socket().cancel(ec);
            m_stream.socket().close(ec);
        }
    }
};
