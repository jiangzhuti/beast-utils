/*
 * =====================================================================================
 *
 *       Filename:  lurker.hpp
 *
 *        Created:  2019-11-26
 *        License:  WTFPL
 *
 *         Author:  李强, liqiang01@qiyi.com
 *
 * =====================================================================================
 */

#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/lockfree/queue.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include <mutex>
#include <atomic>
#include "fail.hpp"

//websocket session
//thread-safe
class lurker_session : public std::enable_shared_from_this<lurker_session>
{
public:
    using accept_handler = std::function<void (std::shared_ptr<lurker_session>)>;
    using read_handler = std::function<void (std::string, std::shared_ptr<lurker_session>)>;
    using write_handler = std::function<void (std::shared_ptr<lurker_session>)>;
    using error_handler = std::function<void (boost::beast::error_code ec, std::shared_ptr<lurker_session>)>;
    using close_handler = std::function<void (boost::beast::websocket::close_reason, std::shared_ptr<lurker_session>)>;
private:
    using tcp = boost::asio::ip::tcp;
    boost::beast::websocket::stream<tcp::socket> m_ws;

    //handlers
    accept_handler m_accept_hdl;
    read_handler m_read_hdl;
    write_handler m_write_hdl;
    error_handler m_error_hdl;
    close_handler m_close_hdl;

    //buffer
    boost::beast::flat_buffer m_read_buf;
    boost::lockfree::queue<std::string const *> m_write_que;
    std::atomic_bool m_in_writing;
public:
    explicit lurker_session(tcp::socket&& socket) : m_ws(std::move(socket)),
                                                  m_write_que(128),
                                                  m_in_writing(false)
    { }
    ~lurker_session()
    {
        m_write_que.consume_all([](std::string const *str) {delete str;});
    }

    // set_xxx_handler 只能在 m_xxx_hdl()中被调用
    // 这样一来，可以被strand保护，不会产生同一时间两个线程同时修改一个m_xxx_hdl的情况
    void set_accept_handler(accept_handler accept_hdl)
    {
        m_accept_hdl = accept_hdl;
    }
    void set_read_handler(read_handler read_hdl)
    {
        m_read_hdl = read_hdl;
    }
    void set_write_handler(write_handler write_hdl)
    {
        m_write_hdl = write_hdl;
    }
    void set_error_handler(error_handler error_hdl)
    {
        m_error_hdl = error_hdl;
    }
    void set_close_handler(close_handler close_hdl)
    {
        m_close_hdl = close_hdl;
    }

    void write(std::string const& data)
    {
        m_write_que.push(new std::string{data});
        if (!m_in_writing) {
            this->do_write();
        }
    }

    void close(boost::beast::websocket::close_reason const& reason)
    {
        m_ws.async_close(reason,
                         boost::beast::bind_front_handler(&lurker_session::on_close,
                                                          shared_from_this()));
    }

    void run()
    {
        // Set suggested timeout settings for the websocket
        m_ws.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::server));

        // Set a decorator to change the Server of the handshake
        m_ws.set_option(boost::beast::websocket::stream_base::decorator([](boost::beast::websocket::response_type& res)
                                                                        {
                                                                            res.set(boost::beast::http::field::server,
                                                                                    std::string(BOOST_BEAST_VERSION_STRING) +
                                                                                    " lurker");
                                                                        }));
        m_ws.async_accept(boost::beast::bind_front_handler(&lurker_session::on_accept,
                                                           shared_from_this()));
    }

private:

    void do_read()
    {
        m_ws.async_read(m_read_buf,
                        boost::beast::bind_front_handler(&lurker_session::on_read,
                                                         shared_from_this()));
    }

    void do_write()
    {
        std::string const *write_str;
        bool ret = m_write_que.pop(write_str);
        if (ret) {
            m_ws.text(true);
            m_in_writing = true;
            m_ws.async_write(boost::asio::buffer(*write_str),
                             boost::beast::bind_front_handler(&lurker_session::on_write,
                                                              shared_from_this()));
            return;
        }
        m_in_writing = false;
    }

    
    void on_accept(boost::beast::error_code ec)
    {
        if (ec) {
            fail(ec, FAIL_LOCATION);
            if (m_error_hdl != nullptr) {
                m_error_hdl(ec, shared_from_this());
            }
            return;
        }
        if (m_accept_hdl != nullptr) {
            m_accept_hdl(shared_from_this());
        }
        this->do_read();
    }

    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if (ec == boost::beast::websocket::error::closed) {
            if (m_close_hdl != nullptr) {
                m_close_hdl(m_ws.reason(), shared_from_this());
            }
            return;
        }
        if (ec) {
            if (m_error_hdl != nullptr) {
                m_error_hdl(ec, shared_from_this());
            }
            return;
        }
        if (m_read_hdl != nullptr) {
            m_read_hdl(boost::beast::buffers_to_string(m_read_buf.data()), shared_from_this());
        }
        m_read_buf.consume(m_read_buf.size());
        this->do_read();
        
    }

    void on_write(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if (ec) {
            fail(ec, FAIL_LOCATION);
            if (m_error_hdl != nullptr) {
                m_error_hdl(ec, shared_from_this());
            }
            return;
        }
        if (m_write_hdl != nullptr) {
            m_write_hdl(shared_from_this());
        }
        do_write();
    }

    void on_close(boost::beast::error_code ec)
    {
        if (ec) {
            fail(ec, FAIL_LOCATION);
            if (m_error_hdl != nullptr) {
                m_error_hdl(ec, shared_from_this());
            }
            return;
        }
        if (m_close_hdl != nullptr) {
            m_close_hdl(m_ws.reason(), shared_from_this());
        }
    }
};

// simple websocket server
// only support text mode
class lurker : public std::enable_shared_from_this<lurker>
{
public:
    using accept_handler = lurker_session::accept_handler;
    using read_handler = lurker_session::read_handler;
    using write_handler = lurker_session::write_handler;
    using close_handler = lurker_session::close_handler;
    using connect_handler = std::function<void (std::shared_ptr<lurker_session>)>;
private:
    using tcp = boost::asio::ip::tcp;
    tcp::acceptor m_acceptor;
    connect_handler m_connect_hdl;
public:
    lurker(boost::asio::io_context& ioc, tcp::endpoint ep, connect_handler sh) : m_acceptor(ioc), m_connect_hdl(sh)
    {
        boost::system::error_code ec;
        m_acceptor.open(ep.protocol(), ec);
        if (ec) {
            fail(ec, FAIL_LOCATION);
            return;
        }
        m_acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            fail(ec, FAIL_LOCATION);
            return;
        }
        m_acceptor.bind(ep, ec);
        if (ec) {
            fail(ec, FAIL_LOCATION);
            return;
        }
        m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            fail(ec, FAIL_LOCATION);
            return;
        }
    }

    void run()
    {
        if (!m_acceptor.is_open()) {
            return;
        }
        do_accept();
    }

private:
    void do_accept()
    {
        m_acceptor.async_accept(boost::asio::make_strand(m_acceptor.get_executor()),
                                boost::beast::bind_front_handler(&lurker::on_accept,
                                                                 shared_from_this()));
    }

    void on_accept(boost::beast::error_code ec, tcp::socket socket)
    {
        if (ec) {
            return fail(ec, FAIL_LOCATION);
        } else {
            auto session = std::make_shared<lurker_session>(std::move(socket));
            m_connect_hdl(session);
            session->run();
        }
        do_accept();
    }
};

