#include <steem/http_plugin/http_plugin.hpp>
#include <fc/network/ip.hpp>
#include <fc/log/logger_config.hpp>

#include <boost/asio.hpp>
#include <boost/optional.hpp>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/logger/stub.hpp>

#include <thread>
#include <memory>

namespace steem {
   namespace asio = boost::asio;

   using boost::optional;
   using boost::asio::ip::tcp;
   using std::shared_ptr;
   using websocketpp::connection_hdl;


   namespace detail {

      struct asio_with_stub_log : public websocketpp::config::asio {
          typedef asio_with_stub_log type;
          typedef asio base;

          typedef base::concurrency_type concurrency_type;

          typedef base::request_type request_type;
          typedef base::response_type response_type;

          typedef base::message_type message_type;
          typedef base::con_msg_manager_type con_msg_manager_type;
          typedef base::endpoint_msg_manager_type endpoint_msg_manager_type;

          /// Custom Logging policies
          /*typedef websocketpp::log::syslog<concurrency_type,
              websocketpp::log::elevel> elog_type;
          typedef websocketpp::log::syslog<concurrency_type,
              websocketpp::log::alevel> alog_type;
          */
          //typedef base::alog_type alog_type;
          //typedef base::elog_type elog_type;
          typedef websocketpp::log::stub elog_type;
          typedef websocketpp::log::stub alog_type;

          typedef base::rng_type rng_type;

          struct transport_config : public base::transport_config {
              typedef type::concurrency_type concurrency_type;
              typedef type::alog_type alog_type;
              typedef type::elog_type elog_type;
              typedef type::request_type request_type;
              typedef type::response_type response_type;
              typedef websocketpp::transport::asio::basic_socket::endpoint
                  socket_type;
          };

          typedef websocketpp::transport::asio::endpoint<transport_config>
              transport_type;

          static const long timeout_open_handshake = 0;
      };
   }

   typedef websocketpp::server<detail::asio_with_stub_log>  websocket_server_type;

   class http_plugin_impl {
      public:
         shared_ptr<std::thread>  http_thread;
         asio::io_service         http_ios;
         map<string,url_handler>  url_handlers;
         optional<tcp::endpoint>  listen_endpoint;

         websocket_server_type    server;
   };

   http_plugin::http_plugin():my( new http_plugin_impl() ){}
   http_plugin::~http_plugin(){}

   void http_plugin::set_program_options( options_description& cli, options_description& cfg ) {
      cfg.add_options()
            ("http-server-endpoint", bpo::value<string>()->default_value( "127.0.0.1:8888" ), 
             "The local IP and port to listen for incoming http connections.")
            ;
   }

   void http_plugin::plugin_initialize( const variables_map& options ) {
      if( options.count( "http-server-endpoint" ) ) {
         auto lipstr = options.at("http-server-endpoint").as< string >();
         auto fcep   = fc::ip::endpoint::from_string( lipstr );
         my->listen_endpoint = tcp::endpoint( boost::asio::ip::address_v4::from_string( (string)fcep.get_address() ), fcep.port() );
         ilog( "configured http to listen on ${ep}", ("ep", fcep) );
      }
   }

   void http_plugin::plugin_startup() {
      if( my->listen_endpoint ) {

         my->http_thread = std::make_shared<std::thread>( [&](){ 
            // fc::set_thread_name( "http" );
            ilog( "start processing http thread" );
            try {

               my->server.clear_access_channels( websocketpp::log::alevel::all );
               my->server.init_asio(&my->http_ios);
               my->server.set_reuse_addr(true);

               my->server.set_http_handler( [&]( connection_hdl hdl ) {
                  auto con = my->server.get_con_from_hdl( hdl );
                  ilog( "handle http request: ${url}", ("url",con->get_uri()->str()) );
                  ilog( "${body}", ("body", con->get_request_body() ) );

                  auto body        = con->get_request_body();
                  auto resource    = con->get_uri()->get_resource();
                  auto handler_itr = my->url_handlers.find( resource );
                  if( handler_itr != my->url_handlers.end() ) {
                     handler_itr->second( resource, body, [con,this]( int code, string body) {
                        my->http_ios.post( [=]() {
                           con->set_body( body );
                           con->set_status( websocketpp::http::status_code::value(code) );
                        });
                     });
                  } else {
                     wlog( "404 - not found: ${ep}", ("ep",resource) );
                     con->set_body( "Unknown Endpoint" );
                     con->set_status( websocketpp::http::status_code::not_found );
                  }
               } );

               ilog( "start listening for http requests" );
               my->server.listen( *my->listen_endpoint );
               my->server.start_accept();

                my->http_ios.run(); 
                wlog( "http io service exit" );
            } catch ( ... ) {
                elog( "error thrown from http io service" );
            }
         });

      }
   }

   void http_plugin::plugin_shutdown() {
      if( my->http_thread ) {
         if( my->server.is_listening() )
             my->server.stop_listening();
         my->http_ios.stop();
         my->http_thread->join();
         my->http_thread.reset();
      }
   }

   void http_plugin::add_handler( const string& url, const url_handler& handler )
   {
      my->http_ios.post( [=](){
        my->url_handlers.insert( std::make_pair(url,handler) );
      });
   }

}
