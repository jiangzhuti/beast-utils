/*
 * =====================================================================================
 *
 *       Filename:  fail.hpp
 *
 *        Created:  11/02/2018
 *        License:  WTFPL
 *
 *         Author:  李强, liqiang01@qiyi.com
 *
 * =====================================================================================
 */

#pragma once

#include <iostream>
#include <string>
#include <thread>
#include <boost/system/error_code.hpp>
#include <boost/beast/core/error.hpp>

static inline void fail(const boost::beast::error_code& ec, std::string const& what)
{
    std::cerr << what << ": " << ec.message() << std::endl;
}

//__func__ is a static char array
//__FILE__ is a literal
//__LINE__ is a number
#define FAIL_LOCATION std::string(__func__).append(" at " __FILE__ ":").append(std::to_string(__LINE__))

