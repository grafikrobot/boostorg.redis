/* Copyright (c) 2019 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <iostream>
#include <map>

#include <aedis/aedis.hpp>

#include "test_stream.hpp"
#include "check.hpp"

namespace net = aedis::net;
using tcp = net::ip::tcp;
using tcp_socket = net::use_awaitable_t<>::as_default_on_t<tcp::socket>;
using test_tcp_socket = net::use_awaitable_t<>::as_default_on_t<aedis::test_stream<aedis::net::system_executor>>;

namespace this_coro = net::this_coro;

using namespace aedis;
using namespace aedis::resp3;

std::vector<node> gresp;

//-------------------------------------------------------------------

struct test_general_fill {
   std::vector<int> list_ {1 ,2, 3, 4, 5, 6};
   std::string set_ {"aaa"};

   void operator()(resp3::serializer<command>& p) const
   {
      p.push(command::flushall);
      p.push_range(command::rpush, "a", std::cbegin(list_), std::cend(list_));
      p.push(command::llen, "a");
      p.push(command::lrange, "a", 0, -1);
      p.push(command::ltrim, "a", 2, -2);
      p.push(command::lpop, "a");
      //p.lpop("a", 2); // Not working?
      p.push(command::set, "b", set_);
      p.push(command::get, "b");
      p.push(command::append, "b", "b");
      p.push(command::del, "b");
      p.push(command::subscribe, "channel");
      p.push(command::publish, "channel", "message");
      p.push(command::incr, "3");

      //----------------------------------
      // transaction
      for (auto i = 0; i < 3; ++i) {
	 p.push(command::multi);
	 p.push(command::ping);
	 p.push(command::lrange, "a", 0, -1);
	 p.push(command::ping);
	 // TODO: It looks like we can't publish to a channel we
	 // are already subscribed to from inside a transaction.
	 //req.publish("some-channel", "message1");
	 p.push(command::exec);
      }
      //----------------------------------

      std::map<std::string, std::string> m1 =
      { {"field1", "value1"}
      , {"field2", "value2"}};

      p.push_range(command::hset, "d", std::cbegin(m1), std::cend(m1));
      p.push(command::hget, "d", "field2");
      p.push(command::hgetall, "d");
      p.push(command::hdel, "d", "field1", "field2"); // TODO: Test as range too.
      p.push(command::hincrby, "e", "some-field", 10);

      p.push(command::zadd, "f", 1, "Marcelo");
      p.push(command::zrange, "f", 0, 1);
      p.push(command::zrangebyscore, "f", 1, 1);
      p.push(command::zremrangebyscore, "f", "-inf", "+inf");

      auto const v = std::vector<int>{1, 2, 3};
      p.push_range(command::sadd, "g", std::cbegin(v), std::cend(v));
      p.push(command::smembers, "g");

      p.push(command::quit);
   }
};

net::awaitable<void>
test_general(net::ip::tcp::resolver::results_type const& res)
{
   std::queue<resp3::serializer<command>> srs;
   srs.push({});
   srs.back().push(command::hello, 3);
   test_general_fill filler;
   filler(srs.back());

   auto ex = co_await this_coro::executor;
   net::ip::tcp::socket socket{ex};
   co_await net::async_connect(socket, res, net::use_awaitable);

   co_await net::async_write(
      socket, net::buffer(srs.back().request()), net::use_awaitable);

   std::string buffer;
   std::vector<node> resp;
   int push_counter = 0;
   for (;;) {
      resp.clear();
      co_await resp3::async_read(socket, net::dynamic_buffer(buffer), adapt(resp), net::use_awaitable);

      if (resp.front().data_type == resp3::type::push) {
	 switch (push_counter) {
	    case 0:
	    {
              std::vector<node> expected
	       { {resp3::type::push,        3UL, 0UL, {}}
	       , {resp3::type::blob_string, 1UL, 1UL, {"subscribe"}}
	       , {resp3::type::blob_string, 1UL, 1UL, {"channel"}}
	       , {resp3::type::number,      1UL, 1UL, {"1"}}
	       };

	       check_equal(resp, expected, "push (value1)"); break;
	    } break;
	    case 1:
	    {
              std::vector<node> expected
	       { {resp3::type::push,        3UL, 0UL, {}}
	       , {resp3::type::blob_string, 1UL, 1UL, {"message"}}
	       , {resp3::type::blob_string, 1UL, 1UL, {"channel"}}
	       , {resp3::type::blob_string, 1UL, 1UL, {"message"}}
	       };

	       check_equal(resp, expected, "push (value2)"); break;
	    } break;
	    default: std::cout << "ERROR: unexpected push event." << std::endl;
	 }
	 ++push_counter;
         continue;
      }

      auto const cmd = srs.front().commands.front();
      srs.front().commands.pop();

      switch (cmd) {
	 case command::hello:
	 {
	 } break;
	 case command::multi:
	 {
           std::vector<node> expected
	    { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };

	    check_equal(resp, expected, "multi");
	 } break;
	 case command::ping:
	 {
           std::vector<node> expected
	    { {resp3::type::simple_string, 1UL, 0UL, {"QUEUED"}} };

	    check_equal(resp, expected, "ping");
	 } break;
	 case command::set:
	 {
           std::vector<node> expected
	    { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };

	    check_equal(resp, expected, "set");
	 } break;
	 case command::quit:
	 {
           std::vector<node> expected
	    { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };

	    check_equal(resp, expected, "quit");
	 } break;
	 case command::flushall:
	 {
           std::vector<node> expected
	    { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };

	    check_equal(resp, expected, "flushall");
	 } break;
	 case command::ltrim:
	 {
           std::vector<node> expected
	    { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };

	    check_equal(resp, expected, "ltrim");
	 } break;
	 case command::append:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"4"}} };

           check_equal(resp, expected, "append");
         } break;
	 case command::hset:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"2"}} };

           check_equal(resp, expected, "hset");
         } break;
	 case command::rpush:
         {
           auto const n = std::to_string(std::size(filler.list_));
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, n} };

           check_equal(resp, expected, "rpush (value)");
         } break;
	 case command::del:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"1"}} };

           check_equal(resp, expected, "del");
         } break;
	 case command::llen:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"6"}} };

           check_equal(resp, expected, "llen");
         } break;
	 case command::incr:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"1"}} };

           check_equal(resp, expected, "incr");
         } break;
	 case command::publish:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"1"}} };

           check_equal(resp, expected, "publish");
         } break;
	 case command::hincrby:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"10"}} };

           check_equal(resp, expected, "hincrby");
         } break;
	 case command::zadd:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"1"}} };

           check_equal(resp, expected, "zadd");
         } break;
	 case command::sadd:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"3"}} };

           check_equal(resp, expected, "sadd");
         } break;
	 case command::hdel:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"2"}} };

           check_equal(resp, expected, "hdel");
         } break;
	 case command::zremrangebyscore:
         {
           std::vector<node> expected
	    { {resp3::type::number, 1UL, 0UL, {"1"}} };

           check_equal(resp, expected, "zremrangebyscore");
         } break;
	 case command::get:
	 {
           std::vector<node> expected
	       { {resp3::type::blob_string, 1UL, 0UL, filler.set_} };

	    check_equal(resp, expected, "get");
	 } break;
	 case command::hget:
	 {
           std::vector<node> expected
	       { {resp3::type::blob_string, 1UL, 0UL, std::string{"value2"}} };

	    check_equal(resp, expected, "hget");
	 } break;
	 case command::lrange:
         {
            static int c = 0;

            if (c == 0) {
              std::vector<node> expected
                  { {resp3::type::array,       6UL, 0UL, {}}
                  , {resp3::type::blob_string, 1UL, 1UL, {"1"}}
                  , {resp3::type::blob_string, 1UL, 1UL, {"2"}}
                  , {resp3::type::blob_string, 1UL, 1UL, {"3"}}
                  , {resp3::type::blob_string, 1UL, 1UL, {"4"}}
                  , {resp3::type::blob_string, 1UL, 1UL, {"5"}}
                  , {resp3::type::blob_string, 1UL, 1UL, {"6"}}
                  };

               check_equal(resp, expected, "lrange ");
            } else {
              std::vector<node> expected
                  { {resp3::type::simple_string, 1UL, 0UL, {"QUEUED"}} };

               check_equal(resp, expected, "lrange (inside transaction)");
            }
            
            ++c;
         } break;
	 case command::hvals:
         {
           std::vector<node> expected
	       { {resp3::type::array, 2UL, 0UL, {}}
	       , {resp3::type::array, 1UL, 1UL, {"value1"}}
	       , {resp3::type::array, 1UL, 1UL, {"value2"}}
               };

           check_equal(resp, expected, "hvals");
         } break;
	 case command::zrange:
         {
           std::vector<node> expected
	       { {resp3::type::array,       1UL, 0UL, {}}
	       , {resp3::type::blob_string, 1UL, 1UL, {"Marcelo"}}
               };

           check_equal(resp, expected, "hvals");
         } break;
	 case command::zrangebyscore:
         {
           std::vector<node> expected
	       { {resp3::type::array,       1UL, 0UL, {}}
	       , {resp3::type::blob_string, 1UL, 1UL, {"Marcelo"}}
               };

           check_equal(resp, expected, "zrangebyscore");
         } break;
	 case command::lpop:
	 {
	    switch (resp.front().data_type)
	    {
	       case resp3::type::blob_string:
	       {
                 std::vector<node> expected
		     { {resp3::type::blob_string, 1UL, 0UL, {"3"}} };

	          check_equal(resp, expected, "lpop");
	       } break;
	       case resp3::type::array:
               {
                 std::vector<node> expected
                    { {resp3::type::array, 2UL, 0UL, {}}
                    , {resp3::type::array, 1UL, 1UL, {"4"}}
                    , {resp3::type::array, 1UL, 1UL, {"5"}}
                    };

                 check_equal(resp, expected, "lpop");
               } break;
	       default:
               {
                 std::cout << "Error." << std::endl;
               }
	    }
	 } break;
	 case command::exec:
	 {
           std::vector<node> expected
               { {resp3::type::array,         3UL, 0UL, {}}
               , {resp3::type::simple_string, 1UL, 1UL, {"PONG"}}
               , {resp3::type::array,         2UL, 1UL, {}}
               , {resp3::type::blob_string,   1UL, 2UL, {"4"}}
               , {resp3::type::blob_string,   1UL, 2UL, {"5"}}
               , {resp3::type::simple_string, 1UL, 1UL, {"PONG"}}
               };

	    check_equal(resp, expected, "transaction");

	 } break;
	 case command::hgetall:
	 {
           std::vector<node> expected
	    { {resp3::type::map,         2UL, 0UL, {}}
	    , {resp3::type::blob_string, 1UL, 1UL, {"field1"}}
	    , {resp3::type::blob_string, 1UL, 1UL, {"value1"}}
	    , {resp3::type::blob_string, 1UL, 1UL, {"field2"}}
	    , {resp3::type::blob_string, 1UL, 1UL, {"value2"}}
	    };
	    check_equal(resp, expected, "hgetall (value)");
	 } break;
	 case command::smembers:
	 {
           std::vector<node> expected
	    { {resp3::type::set,          3UL, 0UL, {}}
	    , {resp3::type::blob_string,  1UL, 1UL, {"1"}}
	    , {resp3::type::blob_string,  1UL, 1UL, {"2"}}
	    , {resp3::type::blob_string,  1UL, 1UL, {"3"}}
	    };
	    check_equal(resp, expected, "smembers (value)");
	 } break;
	 default: { std::cout << "Error: " << resp.front().data_type << " " << cmd << std::endl; }
      }

      resp.clear();
   }
}

//-------------------------------------------------------------------

//net::awaitable<void>
//test_list(net::ip::tcp::resolver::results_type const& results)
//{
//   std::vector<int> list {1 ,2, 3, 4, 5, 6};
//
//   resp3::serializer p;
//   p.push(command::hello, 3);
//   p.push(command::flushall);
//   p.push_range(command::rpush, "a", std::cbegin(list), std::cend(list));
//   p.push(command::lrange, "a", 0, -1);
//   p.push(command::lrange, "a", 2, -2);
//   p.push(command::ltrim, "a", 2, -2);
//   p.push(command::lpop, "a");
//   p.push(command::quit);
//
//   auto ex = co_await this_coro::executor;
//   tcp_socket socket {ex};
//   co_await async_connect(socket, results);
//   co_await async_write(socket, net::buffer(p.payload));
//   std::string buf;
//
//   {  // hello
//      gresp.clear();
//      co_await async_read(socket, buf, gresp);
//   }
//
//   {  // flushall
//      gresp.clear();
//      co_await async_read(socket, buf, gresp);
//      std::vector<node> expected
//        { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };
//      check_equal(gresp, expected, "flushall");
//   }
//
//   {  // rpush
//      gresp.clear();
//      std::vector<node> expected
//        { {resp3::type::number, 1UL, 0UL, {"6"}} };
//      co_await async_read(socket, buf, gresp);
//      check_equal(gresp, expected, "rpush");
//   }
//
//   {  // lrange
//      resp3::flat_array_int_type buffer;
//      resp3::detail::basic_flat_array_adapter<int> res{&buffer};
//      co_await async_read(socket, buf, res);
//      check_equal(buffer, list, "lrange-1");
//   }
//
//   {  // lrange
//      resp3::flat_array_int_type buffer;
//      resp3::detail::basic_flat_array_adapter<int> res{&buffer};
//      co_await async_read(socket, buf, res);
//      check_equal(buffer, std::vector<int>{3, 4, 5}, "lrange-2");
//   }
//
//   {  // ltrim
//      gresp.clear();
//      std::vector<node> expected
//	 { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };
//
//      co_await async_read(socket, buf, gresp);
//      check_equal(gresp, expected, "ltrim");
//   }
//
//   {  // lpop. Why a blob string instead of a number?
//      gresp.clear();
//      std::vector<node> expected
//	 { {resp3::type::blob_string, 1UL, 0UL, {"3"}} };
//
//      co_await async_read(socket, buf, gresp);
//      check_equal(gresp, expected, "lpop");
//   }
//
//   {  // quit
//      gresp.clear();
//      co_await async_read(socket, buf, gresp);
//      std::vector<node> expected
//      { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };
//      check_equal(gresp, expected, "ltrim");
//   }
//}

std::string test_bulk1(10000, 'a');

net::awaitable<void>
test_set(net::ip::tcp::resolver::results_type const& results)
{
   using namespace aedis;

   // Tests whether the parser can handle payloads that contain the separator.
   test_bulk1[30] = '\r';
   test_bulk1[31] = '\n';

   std::string test_bulk2 = "aaaaa";

   resp3::serializer<command> sr;
   sr.push(command::hello, 3);
   sr.push(command::flushall);
   sr.push(command::set, "s", test_bulk1);
   sr.push(command::get, "s");
   sr.push(command::set, "s", test_bulk2);
   sr.push(command::get, "s");
   sr.push(command::set, "s", "");
   sr.push(command::get, "s");
   sr.push(command::quit);

   auto ex = co_await this_coro::executor;
   tcp_socket socket {ex};
   co_await async_connect(socket, results);

   co_await net::async_write(socket, net::buffer(sr.request()));

   std::string buf;
   {  // hello, flushall
      gresp.clear();
      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
   }

   {  // set
      gresp.clear();
      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      std::vector<node> expected
      { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };
      check_equal(gresp, expected, "set1");
   }

   {  // get
      gresp.clear();
      std::vector<node> expected
	 { {resp3::type::blob_string, 1UL, 0UL, test_bulk1} };

      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      check_equal(gresp, expected, "get1");
   }

   {  // set
      gresp.clear();
      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      std::vector<node> expected
      { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };
      check_equal(gresp, expected, "ltrim");
   }

   {  // get
      gresp.clear();
      std::vector<node> expected
	 { {resp3::type::blob_string, 1UL, 0UL, test_bulk2} };
      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      check_equal(gresp, expected, "get2");
   }

   {  // set
      gresp.clear();
      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      std::vector<node> expected
      { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };
      check_equal(gresp, expected, "set3");
   }

   {  // get
      gresp.clear();
      std::vector<node> expected
	 { {resp3::type::blob_string, 1UL, 0UL, {}} };

      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      check_equal(gresp,  expected, "get3");
   }

   {  // quit
      gresp.clear();
      co_await resp3::async_read(socket, net::dynamic_buffer(buf), adapt(gresp));
      std::vector<node> expected
      { {resp3::type::simple_string, 1UL, 0UL, {"OK"}} };
      check_equal(gresp, expected, "quit");
   }
}

int main()
{
   net::io_context ioc {1};
   tcp::resolver resv(ioc);
   auto const res = resv.resolve("127.0.0.1", "6379");

   //co_spawn(ioc, test_list(res), net::detached);
   co_spawn(ioc, test_set(res), net::detached);
   co_spawn(ioc, test_general(res), net::detached);
   ioc.run();
}

