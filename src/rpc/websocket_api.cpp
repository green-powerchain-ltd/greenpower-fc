
#include <fc/rpc/websocket_api.hpp>

namespace fc { namespace rpc {

websocket_api_connection::~websocket_api_connection()
{
}

websocket_api_connection::websocket_api_connection( fc::http::websocket_connection& c, uint32_t max_depth )
   : api_connection(max_depth),_connection(c)
{
   _rpc_state.add_method( "call", [this]( const variants& args ) -> variant
   {
      FC_ASSERT( args.size() == 3 && args[2].is_array() );
      api_id_type api_id;
      if( args[0].is_string() )
      {
         variant subresult = this->receive_call( 1, args[0].as_string() );
         api_id = subresult.as_uint64();
      }
      else
         api_id = args[0].as_uint64();

      return this->receive_call(
         api_id,
         args[1].as_string(),
         args[2].get_array() );
   } );

   _rpc_state.add_method( "notice", [this]( const variants& args ) -> variant
   {
      FC_ASSERT( args.size() == 2 && args[1].is_array() );
      this->receive_notice( args[0].as_uint64(), args[1].get_array() );
      return variant();
   } );

   _rpc_state.add_method( "callback", [this]( const variants& args ) -> variant
   {
      FC_ASSERT( args.size() == 2 && args[1].is_array() );
      this->receive_callback( args[0].as_uint64(), args[1].get_array() );
      return variant();
   } );

   _rpc_state.on_unhandled( [&]( const std::string& method_name, const variants& args )
   {
      return this->receive_call( 0, method_name, args );
   } );

   _connection.on_message_handler( [&]( const std::string& msg ){ on_message(msg,true); } );
   _connection.on_http_handler( [&]( const std::string& msg ){ return on_message(msg,false); } );
   _connection.closed.connect( [this](){ closed(); } );
}

variant websocket_api_connection::send_call(
   api_id_type api_id,
   string method_name,
   variants args /* = variants() */ )
{
   auto request = _rpc_state.start_remote_call(  "call", {api_id, std::move(method_name), std::move(args) } );
   _connection.send_message( fc::json::to_string(fc::variant(request, _max_conversion_depth),
                                                 fc::json::stringify_large_ints_and_doubles, _max_conversion_depth ) );
   return _rpc_state.wait_for_response( *request.id );
}

variant websocket_api_connection::send_callback(
   uint64_t callback_id,
   variants args /* = variants() */ )
{
   auto request = _rpc_state.start_remote_call( "callback", {callback_id, std::move(args) } );
   _connection.send_message( fc::json::to_string(fc::variant(request, _max_conversion_depth),
                                                 fc::json::stringify_large_ints_and_doubles, _max_conversion_depth ) );
   return _rpc_state.wait_for_response( *request.id );
}

void websocket_api_connection::send_notice(
   uint64_t callback_id,
   variants args /* = variants() */ )
{
   fc::rpc::request req{ optional<uint64_t>(), "notice", {callback_id, std::move(args)}};
   _connection.send_message( fc::json::to_string(fc::variant(req, _max_conversion_depth),
                                                 fc::json::stringify_large_ints_and_doubles, _max_conversion_depth ) );
}

std::string websocket_api_connection::on_message(
   const std::string& message,
   bool send_message /* = true */ )
{
   try
   {
      auto var = fc::json::from_string(message, fc::json::legacy_parser, _max_conversion_depth);
      const auto& var_obj = var.get_object();

      string ssid;
      if (var_obj.contains("ssid"))
         ssid = var_obj["ssid"].as_string();

      if( var_obj.contains( "method" ) )
      {
         auto call = var.as<fc::rpc::request>(_max_conversion_depth);
         exception_ptr optexcept;
         try
         {
            try
            {
#ifdef LOG_LONG_API
               auto start = time_point::now();
#endif

               auto result = _rpc_state.local_call( call.method, call.params );

#ifdef LOG_LONG_API
               auto end = time_point::now();

               if( end - start > fc::milliseconds( LOG_LONG_API_MAX_MS ) )
                  elog( "API call execution time limit exceeded. method: ${m} params: ${p} time: ${t}", ("m",call.method)("p",call.params)("t", end - start) );
               else if( end - start > fc::milliseconds( LOG_LONG_API_WARN_MS ) )
                  wlog( "API call execution time nearing limit. method: ${m} params: ${p} time: ${t}", ("m",call.method)("p",call.params)("t", end - start) );
#endif

               if( call.id )
               {
                  auto reply = fc::json::to_string( response( *call.id, ssid, result, "2.0" ), fc::json::stringify_large_ints_and_doubles, _max_conversion_depth );
                  if( send_message )
                     _connection.send_message( reply );
                  return reply;
               }
            }
            FC_CAPTURE_AND_RETHROW( (call.method)(call.params) )
         }
         catch ( const fc::exception& e )
         {
            if( call.id )
            {
               optexcept = e.dynamic_copy_exception();
            }
         }
         if( optexcept ) {

               auto reply = fc::json::to_string( variant(response( *call.id, ssid, error_object{ 1, optexcept->to_string(), fc::variant(*optexcept, _max_conversion_depth)}, "2.0" ), _max_conversion_depth ),
                                                 fc::json::stringify_large_ints_and_doubles, _max_conversion_depth );
               if( send_message )
                  _connection.send_message( reply );

               return reply;
         }
      }
      else
      {
         auto reply = var.as<fc::rpc::response>(_max_conversion_depth);
         _rpc_state.handle_reply( reply );
      }
   }
   catch ( const fc::exception& e )
   {
      wdump((e.to_detail_string()));
      return e.to_detail_string();
   }
   return string();
}

} } // namespace fc::rpc
