/* Copyright (c) 2019 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <iostream>
#include <queue>
#include <vector>
#include <string>

#include <aedis/aedis.hpp>
#include <aedis/src.hpp>

#include "lib/user_session.hpp"
#include "lib/net_utils.hpp"

namespace net = aedis::net;
namespace redis = aedis::redis;
using aedis::redis::command;
using aedis::redis::client;
using aedis::resp3::node;

// From lib/net_utils.hpp
using aedis::user_session;
using aedis::user_session_base;
using client_type = client<aedis::net::ip::tcp::socket>;
using tcp_acceptor = net::use_awaitable_t<>::as_default_on_t<net::ip::tcp::acceptor>;

class receiver : public std::enable_shared_from_this<receiver> {
private:
   std::vector<node<std::string>> resps_;
   std::queue<std::shared_ptr<user_session_base>> sessions_;

public:
   void on_message(command cmd)
   {
      switch (cmd) {
         case command::ping:
            sessions_.front()->deliver(resps_.front().value);
            sessions_.pop();
            break;

         case command::incr:
            std::cout << "Echos so far: " << resps_.front().value << std::endl;
            break;

         default:
            { /* Ignore */; }
      }

      resps_.clear();
   }

   auto adapter()
      { return redis::adapt(resps_); }

   void add_user_session(std::shared_ptr<user_session_base> session)
      { sessions_.push(session); }
};

net::awaitable<void>
listener(
    std::shared_ptr<tcp_acceptor> acc,
    std::shared_ptr<client_type> db,
    std::shared_ptr<receiver> recv)
{
   for (;;) {
      auto socket = co_await acc->async_accept();
      auto session = std::make_shared<user_session>(std::move(socket));

      auto on_user_msg = [db, recv, session](std::string const& msg)
      {
         db->send(command::ping, msg);
         db->send(command::incr, "echo-counter");
         recv->add_user_session(session);
      };

      session->start(on_user_msg);
   }
}

int main()
{
   try {
      net::io_context ioc;

      auto db = std::make_shared<client_type>(ioc.get_executor());
      auto recv = std::make_shared<receiver>();
      db->set_response_adapter(recv->adapter());
      db->set_reader_callback([recv](command cmd) {recv->on_message(cmd);});
      db->async_run({net::ip::make_address("127.0.0.1"), 6379}, [](auto){});
      auto endpoint = net::ip::tcp::endpoint{net::ip::tcp::v4(), 55555};
      auto acc = std::make_shared<tcp_acceptor>(ioc.get_executor(), endpoint);
      net::signal_set signals(ioc.get_executor(), SIGINT, SIGTERM);

      signals.async_wait([=](auto, int){
         // Request redis to close the connection.
         db->send(aedis::redis::command::quit);

         // Stop the listener.
         acc->cancel();
      });

      co_spawn(ioc, listener(acc, db, recv), net::detached);

      ioc.run();
   } catch (std::exception const& e) {
      std::cerr << e.what() << std::endl;
   }
}
