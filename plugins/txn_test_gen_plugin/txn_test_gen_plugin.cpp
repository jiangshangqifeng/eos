#include <eosio/txn_test_gen_plugin/txn_test_gen_plugin.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/thread_utils.hpp>

#include <fc/variant.hpp>
#include <fc/io/json.hpp>
#include <fc/exception/exception.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/io/json.hpp>

#include <boost/asio/high_resolution_timer.hpp>
#include <boost/algorithm/clamp.hpp>

#include <Inline/BasicTypes.h>
#include <IR/Module.h>
#include <IR/Validate.h>
#include <WAST/WAST.h>
#include <WASM/WASM.h>
#include <Runtime/Runtime.h>

#include <contracts.hpp>

using namespace eosio::testing;

namespace eosio { namespace detail {
  struct txn_test_gen_empty {};
  struct txn_test_gen_status {
     string status;
  };
}}

FC_REFLECT(eosio::detail::txn_test_gen_empty, );
FC_REFLECT(eosio::detail::txn_test_gen_status, (status));

namespace eosio {

static appbase::abstract_plugin& _txn_test_gen_plugin = app().register_plugin<txn_test_gen_plugin>();

using namespace eosio::chain;

#define CALL(api_name, api_handle, call_name, INVOKE, http_response_code) \
{std::string("/v1/" #api_name "/" #call_name), \
   [this](string, string body, url_response_callback cb) mutable { \
          try { \
             if (body.empty()) body = "{}"; \
             INVOKE \
             cb(http_response_code, fc::variant(result)); \
          } catch (...) { \
             http_plugin::handle_exception(#api_name, #call_name, body, cb); \
          } \
       }}

#define INVOKE_V_R_R_R(api_handle, call_name, in_param0, in_param1, in_param2) \
     const auto& vs = fc::json::json::from_string(body).as<fc::variants>(); \
     auto status = api_handle->call_name(vs.at(0).as<in_param0>(), vs.at(1).as<in_param1>(), vs.at(2).as<in_param2>()); \
     eosio::detail::txn_test_gen_status result = { status };

#define INVOKE_V_R_R(api_handle, call_name, in_param0, in_param1) \
     const auto& vs = fc::json::json::from_string(body).as<fc::variants>(); \
     api_handle->call_name(vs.at(0).as<in_param0>(), vs.at(1).as<in_param1>()); \
     eosio::detail::txn_test_gen_empty result;

#define INVOKE_V_V(api_handle, call_name) \
     api_handle->call_name(); \
     eosio::detail::txn_test_gen_empty result;

#define CALL_ASYNC(api_name, api_handle, call_name, INVOKE, http_response_code) \
{std::string("/v1/" #api_name "/" #call_name), \
   [this](string, string body, url_response_callback cb) mutable { \
      if (body.empty()) body = "{}"; \
       /*plugin processes many transactions, report only first to avoid http_plugin having to deal with multiple responses*/ \
      auto times_called = std::make_shared<std::atomic<size_t>>(0);\
      auto result_handler = [times_called{std::move(times_called)}, cb, body](const fc::exception_ptr& e) mutable {\
         if( ++(*times_called) > 1 ) return;\
         if (e) {\
            try {\
               e->dynamic_rethrow_exception();\
            } catch (...) {\
               http_plugin::handle_exception(#api_name, #call_name, body, cb);\
            }\
         } else {\
            cb(http_response_code, fc::variant(eosio::detail::txn_test_gen_empty())); \
         }\
      };\
      INVOKE \
   }\
}

#define INVOKE_ASYNC_R_R(api_handle, call_name, in_param0, in_param1) \
   const auto& vs = fc::json::json::from_string(body).as<fc::variants>(); \
   api_handle->call_name(vs.at(0).as<in_param0>(), vs.at(1).as<in_param1>(), result_handler);

struct txn_test_gen_plugin_impl {

   uint64_t _total_us = 0;
   uint64_t _txcount = 0;

   uint16_t                                             thread_pool_size;
   fc::optional<eosio::chain::named_thread_pool>        thread_pool;
   std::shared_ptr<boost::asio::high_resolution_timer>  timer;
   uint16_t total_accounts;
   std::vector<name> accounts;
   name newaccountT;

   void push_next_transaction(const std::shared_ptr<std::vector<signed_transaction>>& trxs, const std::function<void(const fc::exception_ptr&)>& next ) {
      chain_plugin& cp = app().get_plugin<chain_plugin>();

      for (size_t i = 0; i < trxs->size(); ++i) {
         cp.accept_transaction( std::make_shared<packed_transaction>(trxs->at(i)), [=](const fc::static_variant<fc::exception_ptr, transaction_trace_ptr>& result){
            if (result.contains<fc::exception_ptr>()) {
               next(result.get<fc::exception_ptr>());
            } else {
               if (result.contains<transaction_trace_ptr>() && result.get<transaction_trace_ptr>()->receipt) {
                  _total_us += result.get<transaction_trace_ptr>()->receipt->cpu_usage_us;
                  ++_txcount;
               }
            }
         });
      }
   }

   void push_transactions( std::vector<signed_transaction>&& trxs, const std::function<void(fc::exception_ptr)>& next ) {
      auto trxs_copy = std::make_shared<std::decay_t<decltype(trxs)>>(std::move(trxs));
      app().post(priority::low, [this, trxs_copy, next]() {
         push_next_transaction(trxs_copy, next);
      });
   }

  void create_newAccountT(const std::string& init_name, const std::string& init_priv_key, const std::function<void(const fc::exception_ptr&)>& next) {  
	   ilog("create_newAccountT");
	   std::vector<signed_transaction> trxs;
	   trxs.reserve(2);

	  try {
		 name creator(init_name);
		 abi_def currency_abi_def = fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>();
	  
		 controller& cc = app().get_plugin<chain_plugin>().chain();
		 auto chainid = app().get_plugin<chain_plugin>().get_chain_id();
		 auto abi_serializer_max_time = app().get_plugin<chain_plugin>().get_abi_serializer_max_time();
	  
		 abi_serializer eosio_token_serializer{fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>(),
											  abi_serializer::create_yield_function( abi_serializer_max_time )};
		 
         fc::crypto::private_key creator_priv_key = fc::crypto::private_key(init_priv_key);
		 
		 fc::crypto::private_key txn_test_receiver_C_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'c')));
		 fc::crypto::public_key  txn_text_receiver_C_pub_key = txn_test_receiver_C_priv_key.get_public_key();
	  
		 //create newaccountT
		 {
			signed_transaction trx;
			{
			auto owner_auth   = eosio::chain::authority{1, {{txn_text_receiver_C_pub_key, 1}}, {}};
			auto active_auth  = eosio::chain::authority{1, {{txn_text_receiver_C_pub_key, 1}}, {}};
	  
			trx.actions.emplace_back(vector<chain::permission_level>{{creator,name("active")}}, newaccount{creator, newaccountT, owner_auth, active_auth});
			}
	  
			trx.expiration = cc.head_block_time() + fc::seconds(180);
			trx.set_reference_block(cc.head_block_id());
			trx.sign(creator_priv_key, chainid);
			trxs.emplace_back(std::move(trx));
		 }
		 
         //set newaccountT contract to eosio.token & initialize it
         {
            signed_transaction trx;

            vector<uint8_t> wasm = contracts::eosio_token_wasm();

            setcode handler;
            handler.account = newaccountT;
            handler.code.assign(wasm.begin(), wasm.end());

            trx.actions.emplace_back( vector<chain::permission_level>{{newaccountT,name("active")}}, handler);

            {
               setabi handler;
               handler.account = newaccountT;
               handler.abi = fc::raw::pack(json::from_string(contracts::eosio_token_abi().data()).as<abi_def>());
               trx.actions.emplace_back( vector<chain::permission_level>{{newaccountT,name("active")}}, handler);
            }

            {
               action act;
               act.account = newaccountT;
               act.name = N(create);
               act.authorization = vector<permission_level>{{newaccountT,config::active_name}};
               act.data = eosio_token_serializer.variant_to_binary("create",
                                                                   fc::json::from_string(fc::format_string("{\"issuer\":\"${issuer}\",\"maximum_supply\":\"10000000000.0000 LAT\"}}",
                                                                   fc::mutable_variant_object()("issuer",newaccountT.to_string()))),
                                                                   abi_serializer::create_yield_function( abi_serializer_max_time ));
               trx.actions.push_back(act);
            }
            {
               action act;
               act.account = newaccountT;
               act.name = N(issue);
               act.authorization = vector<permission_level>{{newaccountT,config::active_name}};
               act.data = eosio_token_serializer.variant_to_binary("issue",
                                       fc::json::from_string(fc::format_string("{\"to\":\"${to}\",\"quantity\":\"2000000000.0000 LAT\",\"memo\":\"\"}",
                                       fc::mutable_variant_object()("to",newaccountT.to_string()))),
                                       abi_serializer::create_yield_function( abi_serializer_max_time ));
               trx.actions.push_back(act);
            }
			
            trx.expiration = cc.head_block_time() + fc::seconds(180);
            trx.set_reference_block(cc.head_block_id());
            trx.max_net_usage_words = 5000;
            trx.sign(txn_test_receiver_C_priv_key, chainid);
            trxs.emplace_back(std::move(trx));
         }
	  	}catch (const fc::exception& e) {
         next(e.dynamic_copy_exception());
         return;
      }

      push_transactions(std::move(trxs), next);
	  ilog("create_newAccountT OK!");
  	}
  
   void create_test_accounts(const std::string& init_name, const std::string& init_priv_key, const std::function<void(const fc::exception_ptr&)>& next) {
      ilog("create_test_accounts accounts.size= ${s}", ("s", accounts.size()));
      std::vector<signed_transaction> trxs;
      trxs.reserve(total_accounts);

      try {
         name creator(init_name);
		 abi_def currency_abi_def = fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>();
         controller& cc = app().get_plugin<chain_plugin>().chain();
         auto chainid = app().get_plugin<chain_plugin>().get_chain_id();
         auto abi_serializer_max_time = app().get_plugin<chain_plugin>().get_abi_serializer_max_time();
         abi_serializer eosio_token_serializer{fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>(),
                                               abi_serializer::create_yield_function( abi_serializer_max_time )};
         fc::crypto::private_key creator_priv_key = fc::crypto::private_key(init_priv_key);
		 fc::crypto::private_key txn_test_receiver_C_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'c')));

		 for(unsigned int i = 0; i < total_accounts; ++i) {
		     fc::crypto::private_key txn_test_receiver_priv_key = fc::crypto::private_key::regenerate(fc::sha256::hash(accounts[i].to_string()));
			 fc::crypto::public_key  txn_text_receiver_pub_key = txn_test_receiver_priv_key.get_public_key();

			 //create some accounts
			{
			 	signed_transaction trx;
				auto owner_auth   = eosio::chain::authority{1, {{txn_text_receiver_pub_key, 1}}, {}};
				auto active_auth  = eosio::chain::authority{1, {{txn_text_receiver_pub_key, 1}}, {}};
				
				trx.actions.emplace_back(vector<chain::permission_level>{{creator,name("active")}}, newaccount{creator, accounts[i], owner_auth, active_auth});
		        trx.expiration = cc.head_block_time() + fc::seconds(180);
		        trx.set_reference_block(cc.head_block_id());
		        trx.sign(creator_priv_key, chainid);
            	trxs.emplace_back(std::move(trx));
			}
		 }
      } catch (const fc::exception& e) {
         next(e.dynamic_copy_exception());
         return;
      }

      push_transactions(std::move(trxs), next);
   }


   void transfer_test_accounts(const std::string& init_name, const std::string& init_priv_key, const std::function<void(const fc::exception_ptr&)>& next) {
      ilog("transfer_test_accounts accounts.size= ${s}", ("s", accounts.size()));
      std::vector<signed_transaction> trxs;
      trxs.reserve(total_accounts);

      try {
		 abi_def currency_abi_def = fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>();
         controller& cc = app().get_plugin<chain_plugin>().chain();
         auto chainid = app().get_plugin<chain_plugin>().get_chain_id();
         auto abi_serializer_max_time = app().get_plugin<chain_plugin>().get_abi_serializer_max_time();
         abi_serializer eosio_token_serializer{fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>(),
                                               abi_serializer::create_yield_function( abi_serializer_max_time )};
		 fc::crypto::private_key txn_test_receiver_C_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'c')));

		 for(unsigned int i = 0; i < total_accounts; ++i) {
			 //transfer LAT to test accounts
		 	{
			 	signed_transaction trx;
				action act;
				act.account = newaccountT;
				act.name = N(transfer);
				act.authorization = vector<permission_level>{{newaccountT,config::active_name}};
				act.data = eosio_token_serializer.variant_to_binary("transfer",
                                   fc::json::from_string(fc::format_string("{\"from\":\"${from}\",\"to\":\"${to}\",\"quantity\":\"200000.0000 LAT\",\"memo\":\"\"}",
                                   fc::mutable_variant_object()("from",newaccountT.to_string())("to",accounts[i].to_string()))),
                                   abi_serializer::create_yield_function( abi_serializer_max_time ));
				trx.actions.push_back(act);
	            trx.expiration = cc.head_block_time() + fc::seconds(180);
	            trx.set_reference_block(cc.head_block_id());
	            trx.max_net_usage_words = 5000;
	            trx.sign(txn_test_receiver_C_priv_key, chainid);
	            trxs.emplace_back(std::move(trx));
		 	}
		 }
      } catch (const fc::exception& e) {
         next(e.dynamic_copy_exception());
         return;
      }

      push_transactions(std::move(trxs), next);
   }

   string start_generation(const std::string& salt, const uint64_t& period, const uint64_t& batch_size) {
      ilog("Starting transaction test plugin");
      if(running)
         return "start_generation already running";
      if(period < 1 || period > 2500)
         return "period must be between 1 and 2500";
      if(batch_size < 1 || batch_size > 250)
         return "batch_size must be between 1 and 250";
      if(batch_size & 1)
         return "batch_size must be even";
      ilog("Starting transaction test plugin valid");

      running = true;

      timer_timeout = period;
      batch = batch_size/2;
      nonce_prefix = 0;

	  tx_salt = salt;
      thread_pool.emplace( "txntest", thread_pool_size );
      timer = std::make_shared<boost::asio::high_resolution_timer>(thread_pool->get_executor());

      ilog("Started transaction test plugin; generating ${p} transactions every ${m} ms by ${t} load generation threads",
         ("p", batch_size) ("m", period) ("t", thread_pool_size));

      boost::asio::post( thread_pool->get_executor(), [this]() {
         arm_timer(boost::asio::high_resolution_timer::clock_type::now());
      });
      return "success";
   }

   void arm_timer(boost::asio::high_resolution_timer::time_point s) {
      timer->expires_at(s + std::chrono::milliseconds(timer_timeout));
      boost::asio::post( thread_pool->get_executor(), [this]() {
         send_transaction([this](const fc::exception_ptr& e){
            if (e) {
               elog("pushing transaction failed: ${e}", ("e", e->to_detail_string()));
               if(running)
                  stop_generation();
            }
         }, nonce_prefix++);
      });
      timer->async_wait([this](const boost::system::error_code& ec) {
         if(!running || ec)
            return;
         arm_timer(timer->expires_at());
      });
   }

   void send_transaction(std::function<void(const fc::exception_ptr&)> next, uint64_t nonce_prefix) {
      std::vector<signed_transaction> trxs;
      trxs.reserve(2*batch);

      try {
         controller& cc = app().get_plugin<chain_plugin>().chain();
		 auto abi_serializer_max_time = app().get_plugin<chain_plugin>().get_abi_serializer_max_time();
		 abi_serializer eosio_token_serializer{fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>(), abi_serializer::create_yield_function( abi_serializer_max_time )};
         auto chainid = app().get_plugin<chain_plugin>().get_chain_id();

         static uint64_t nonce = static_cast<uint64_t>(fc::time_point::now().sec_since_epoch()) << 32;

         uint32_t reference_block_num = cc.last_irreversible_block_num();
         if (txn_reference_block_lag >= 0) {
            reference_block_num = cc.head_block_num();
            if (reference_block_num <= (uint32_t)txn_reference_block_lag) {
               reference_block_num = 0;
            } else {
               reference_block_num -= (uint32_t)txn_reference_block_lag;
            }
         }

         block_id_type reference_block_id = cc.get_block_id_for_num(reference_block_num);

		uint64_t seed = nonce;
        for(unsigned int i = 0; i < batch; ++i) {
		 	
         uint16_t nonce_index = uint16_t(seed % total_accounts)+i;
         uint16_t a_index = nonce_index >= total_accounts?(nonce_index-total_accounts):nonce_index;
         uint16_t b_index = (a_index+batch) >= total_accounts?(a_index+batch-total_accounts):(a_index+batch);
		 if(b_index == a_index) {
		 	b_index = a_index + uint16_t(total_accounts/2);
		 }
		 
         fc::crypto::private_key a_priv_key = fc::crypto::private_key::regenerate(fc::sha256::hash(accounts[a_index].to_string()));
         fc::crypto::private_key b_priv_key = fc::crypto::private_key::regenerate(fc::sha256::hash(accounts[b_index].to_string()));

		 {
		 //create the actions here
	     action act_a_to_b;
		 act_a_to_b.account = newaccountT;
		 act_a_to_b.name = N(transfer);
		 act_a_to_b.authorization = vector<permission_level>{{accounts[a_index],config::active_name}};
		 act_a_to_b.data = eosio_token_serializer.variant_to_binary("transfer",
						 fc::json::from_string(fc::format_string("{\"from\":\"${from}\",\"to\":\"${to}\",\"quantity\":\"1.0000 LAT\",\"memo\":\"${l}\"}",
						 fc::mutable_variant_object()("from",accounts[a_index].to_string())("to",accounts[b_index].to_string())("l", tx_salt))),
						 abi_serializer::create_yield_function( abi_serializer_max_time ));
		 
         signed_transaction trx;
         trx.actions.push_back(act_a_to_b);
         trx.context_free_actions.emplace_back(action({}, config::null_account_name, name("nonce"), fc::raw::pack( std::to_string(nonce_prefix)+std::to_string(nonce++) )));
         trx.set_reference_block(reference_block_id);
         trx.expiration = cc.head_block_time() + fc::seconds(30);
         trx.max_net_usage_words = 100;
         trx.sign(a_priv_key, chainid);
         trxs.emplace_back(std::move(trx));
		 
         }

         {
	     action act_b_to_a;
		 act_b_to_a.account = newaccountT;
		 act_b_to_a.name = N(transfer);
		 act_b_to_a.authorization = vector<permission_level>{{accounts[b_index],config::active_name}};
		 act_b_to_a.data = eosio_token_serializer.variant_to_binary("transfer",
						 fc::json::from_string(fc::format_string("{\"from\":\"${from}\",\"to\":\"${to}\",\"quantity\":\"1.0000 LAT\",\"memo\":\"${l}\"}",
						 fc::mutable_variant_object()("from",accounts[b_index].to_string())("to",accounts[a_index].to_string())("l", tx_salt))),
						 abi_serializer::create_yield_function( abi_serializer_max_time ));
		 
         signed_transaction trx;
         trx.actions.push_back(act_b_to_a);
         trx.context_free_actions.emplace_back(action({}, config::null_account_name, name("nonce"), fc::raw::pack( std::to_string(nonce_prefix)+std::to_string(nonce++) )));
         trx.set_reference_block(reference_block_id);
         trx.expiration = cc.head_block_time() + fc::seconds(30);
         trx.max_net_usage_words = 100;
         trx.sign(b_priv_key, chainid);
         trxs.emplace_back(std::move(trx));
         }
         }
      } catch ( const fc::exception& e ) {
         next(e.dynamic_copy_exception());
      }

      push_transactions(std::move(trxs), next);
   }

   void stop_generation() {
      if(!running)
         throw fc::exception(fc::invalid_operation_exception_code);
      timer->cancel();
      running = false;
      if( thread_pool )
         thread_pool->stop();

      ilog("Stopping transaction generation test");

      if (_txcount) {
         ilog("${d} transactions executed, ${t}us / transaction", ("d", _txcount)("t", _total_us / (double)_txcount));
         _txcount = _total_us = 0;
      }
   }

   bool running{false};

   unsigned timer_timeout;
   std::string tx_salt;
   unsigned batch;
   uint64_t nonce_prefix;

   int32_t txn_reference_block_lag;
};

txn_test_gen_plugin::txn_test_gen_plugin() {}
txn_test_gen_plugin::~txn_test_gen_plugin() {}

void txn_test_gen_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()
      ("txn-reference-block-lag", bpo::value<int32_t>()->default_value(0), "Lag in number of blocks from the head block when selecting the reference block for transactions (-1 means Last Irreversible Block)")
      ("txn-test-gen-threads", bpo::value<uint16_t>()->default_value(2), "Number of worker threads in txn_test_gen thread pool")
      ("txn-test-gen-account-prefix", bpo::value<string>()->default_value("tx"), "Prefix to use for accounts generated and used by this plugin")
      ("txn-test-gen-account-number", bpo::value<uint16_t>()->default_value(5000), "Total number of accounts")
   ;
}

// return a 10 length name according a random number
std::string gen_random_account_name(uint16_t random_num) {
	const char* charmap = "12345abcdefghijklmnopqrstuvwxyz";
	
	char tmpchar[10], name[10+1];
	memset(tmpchar,0,sizeof(tmpchar));
	memset(name,0,sizeof(name));
	sprintf(tmpchar, "%010d", random_num);
	uint16_t seed = random_num % 31;
	for(int i=0;i<sizeof(tmpchar);++i) {
		int j = tmpchar[i] - '0';
		size_t pos = size_t((seed+j)>=31?(seed+j-31):(seed+j));
	    if(sizeof(tmpchar)==i+1) {
			pos %= 16;
	    }
		name[i] = charmap[pos];
	}
	
	return std::string(name);
}

void txn_test_gen_plugin::plugin_initialize(const variables_map& options) {
   try {
      my.reset( new txn_test_gen_plugin_impl );
      my->txn_reference_block_lag = options.at( "txn-reference-block-lag" ).as<int32_t>();
      my->thread_pool_size = options.at( "txn-test-gen-threads" ).as<uint16_t>();
      my->total_accounts = options.at( "txn-test-gen-account-number" ).as<uint16_t>();	  
      const std::string thread_pool_account_prefix = options.at( "txn-test-gen-account-prefix" ).as<std::string>();
	  my->accounts.reserve(my->total_accounts);

	  for(uint16_t i=0; i<my->total_accounts; ++i) {
		my->accounts.push_back(eosio::chain::name(thread_pool_account_prefix + gen_random_account_name(i)));
	  }
	  
      my->newaccountT = eosio::chain::name(thread_pool_account_prefix + "t");
      EOS_ASSERT( my->thread_pool_size > 0, chain::plugin_config_exception,
                  "txn-test-gen-threads ${num} must be greater than 0", ("num", my->thread_pool_size) );
   } FC_LOG_AND_RETHROW()
}

void txn_test_gen_plugin::plugin_startup() {
   app().get_plugin<http_plugin>().add_api({
      CALL_ASYNC(txn_test_gen, my, create_newAccountT, INVOKE_ASYNC_R_R(my, create_newAccountT, std::string, std::string), 200),
      CALL_ASYNC(txn_test_gen, my, create_test_accounts, INVOKE_ASYNC_R_R(my, create_test_accounts, std::string, std::string), 200),
      CALL_ASYNC(txn_test_gen, my, transfer_test_accounts, INVOKE_ASYNC_R_R(my, transfer_test_accounts, std::string, std::string), 200),
      CALL(txn_test_gen, my, stop_generation, INVOKE_V_V(my, stop_generation), 200),
      CALL(txn_test_gen, my, start_generation, INVOKE_V_R_R_R(my, start_generation, std::string, uint64_t, uint64_t), 200)
   });
}

void txn_test_gen_plugin::plugin_shutdown() {
   try {
      my->stop_generation();
   }
   catch(fc::exception& e) {
   }
}

}
