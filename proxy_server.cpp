#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/http.hpp>
#include <boost/config.hpp>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <thread>
#include <string>
#include <boost/array.hpp>
#include <ctime>
#include <mutex>
#include <boost/optional.hpp>
#include <boost/regex.hpp>
#include <unistd.h>
#include <syslog.h>
#include "lru_cache.cpp"

#define LOG_FILE_PATH "logs/proxy.log"
#define CACHE_LINES 4

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

std::mutex log_mutex;
std::fstream log_;
void log(const std::string& s){
  std::lock_guard<std::mutex> lock(log_mutex);
  log_ << s << std::endl;
}

void
fail(beast::error_code ec, char const* where, const std::string id_)
{
  log(id_ + "Error [" +  std::string(where) + "]: " + ec.message().c_str());
}


// Handles an HTTP proxy connection
class session : public std::enable_shared_from_this<session>
{
private:
  tcp::socket srv_sock_;
  tcp::socket cli_sock_;
  boost::asio::strand<
    boost::asio::io_context::executor_type> strand_;
  boost::beast::flat_buffer srv_http_buffer_;
  boost::beast::flat_buffer cli_http_buffer_;
  http::request<http::string_body> req_;
  http::response<http::dynamic_body> res_;
  tcp::resolver resolver_;
  boost::asio::streambuf srv_use_write_buf;
  boost::asio::streambuf cli_use_write_buf;

  http::response<http::empty_body> res_200_OK;
  http::response<http::dynamic_body> res_400_BAD_REQUEST;
  http::response<http::dynamic_body> res_502_BAD_GATEWAY;
  http::response<http::dynamic_body> cached_res;
  http::request<http::string_body> cached_res_validation_req;
  boost::beast::flat_buffer validation_buffer_;
  http::response<http::dynamic_body> validation_res;
  time_t cached_res_expired_time;
  size_t const read_buf_size;
  LRUCache<std::string, std::pair<http::response<http::dynamic_body>, time_t>>& lru_cache_;
  std::string id_;
  //std::mutex& cache_mutex_;


public:
  explicit
  session(
	  tcp::socket server_socket,
	  tcp::socket client_socket,
	  net::io_context& ioc,
	  LRUCache<std::string, std::pair<http::response<http::dynamic_body>, time_t>>& lru_cache, unsigned long id)
    : srv_sock_(std::move(server_socket))
    , cli_sock_(std::move(client_socket))
    , strand_{ioc.get_executor()}
    , resolver_{ioc}
    , read_buf_size{8192}
    , lru_cache_{lru_cache}
    , id_(std::to_string(id) + ": ")
  {
  }

  void
  run()
  {
    do_recv_req_connect_server();
  }

  void
  do_recv_req_connect_server()
  {   
    req_ = std::move(http::request<http::string_body>());

    // Receive req_ from server
    http::async_read(cli_sock_, cli_http_buffer_, req_,
		     boost::asio::bind_executor(
						strand_,
						std::bind(
							  &session::on_recv_req_connect_server,
							  shared_from_this(),
							  std::placeholders::_1,
							  std::placeholders::_2)));
  }

  void
  on_recv_req_connect_server(
			     boost::system::error_code ec,
			     std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    //ID: REQUEST from IP @ TIME
    // TIME is now in GMT timezone
    log_mutex.lock();
    float version = (req_.version() == 11)? 1.1:1.0;
    auto cli_addr = cli_sock_.remote_endpoint().address();
    time_t now  = time(nullptr);
    struct tm gmt_buffer;
    time_t now_in_gmt = mktime(gmtime_r(&now, &gmt_buffer));
    char ctime_buffer[100];
    ctime_r(&now_in_gmt, ctime_buffer);
    auto now_in_gmt_string = std::string(ctime_buffer);
    now_in_gmt_string.pop_back();
    now_in_gmt_string += " GMT";
    
    log_ << id_ << req_.method() << " " << req_.target()
    	 << " "<< "HTTP/" << version << " from "
    << cli_addr << " @ " << now_in_gmt_string << std::endl;
    
    log_mutex.unlock();
	
	
    // Client closed the connection
    if(ec == http::error::end_of_stream)
      return do_close();
    if(ec)
      return fail(ec, "on_recv_req_connect_server", id_);
        
    // Ensure the http::method is correct, request is in correct format.(Todo)
    switch(req_.method())
      {
      case http::verb::get: break;
      case http::verb::post: break;
      case http::verb::connect: break;
      default: 
	//std::cout << req_.method() << std::endl;
	return fail(ec, "handle_init_request: method not supported", id_);
      }

    // Parse and get the host and port
    auto remove_port_info = [](std::string req_host_string, http::verb method)
      {
	if(method == http::verb::connect && req_host_string.rfind(':') != std::string::npos)
	  req_host_string.erase(req_host_string.rfind(':'));
	return req_host_string;
      };

    auto const host = remove_port_info(std::string(req_.base()[http::field::host]), req_.method());
    auto const port = req_.method() == http::verb::connect ? "443" : "80"; // 443 for https, 80 for http

    resolver_.async_resolve(
			    host,
			    port,
			    boost::asio::bind_executor(
						       strand_,
						       std::bind(
								 &session::on_resolve,
								 shared_from_this(),
								 std::placeholders::_1,
								 std::placeholders::_2)));
  }

  void
  on_resolve(
	     beast::error_code ec,
	     tcp::resolver::results_type results)
  {
    if(ec)
      return fail(ec, "on_resolve", id_);
        
    boost::asio::async_connect(
			       srv_sock_,
			       results.begin(),
			       results.end(),
			       boost::asio::bind_executor(
							  strand_,
							  std::bind(
								    &session::on_connect,
								    shared_from_this(),
								    std::placeholders::_1)));
  }

  void
  on_connect(beast::error_code ec)
  {
    if(ec)
      return fail(ec, "on_connect", id_);
        
    if(req_.method() == http::verb::connect)
      do_https_send_200_OK_res();
    else if(req_.method() == http::verb::post)
      do_http_send_req_to_server();
    else if(req_.method() == http::verb::get)
      do_check_in_cache();
  }

  void
  do_check_in_cache()
  {
    auto target = std::string(req_.target());
    auto cached_res_optional = lru_cache_.get(target);
	
    if(! cached_res_optional)
      {
	log(id_ + "not in cache");
	do_http_send_req_to_server();
      }
    else
      {
	cached_res = std::get<0>(cached_res_optional.get());
	cached_res_expired_time = std::get<1>(cached_res_optional.get());
	do_check_cached_response_need_validate();
      }
  }

  void
  do_check_cached_response_need_validate()
  {
    if(need_validate())
      do_cached_response_validate();
    else
      {
	log(id_ + "in cache, valid");
	res_ = cached_res;
	do_http_send_res_to_client();
      }
  }

  void
  do_cached_response_validate()
  {
    cached_res_validation_req = req_;

    auto etag = cached_res.base()["ETag"];
    auto last_modified = cached_res.base()["Last-Modified"];
    
    if(etag != "")
      {
	log_mutex.lock();
	log_ << id_ << "NOTE ETag: " << etag << std::endl;
	log_mutex.unlock();
      
	cached_res_validation_req.set("ETag", etag);
      }
    if(last_modified != "")
      cached_res_validation_req.set("If-Modified-Since", last_modified);
    /*
    log_mutex.lock();
    std::stringstream ss;
    ss << cached_res_validation_req;
    
    log_ << id_ << "Requesting REQUEST from SERVER" << std::endl;
    log_ << id_ << endl << ss.str() << std::endl;
    log_mutex.unlock();
    */
    http::async_write(srv_sock_, cached_res_validation_req,
		      boost::asio::bind_executor(
						 strand_,
						 std::bind(
							   &session::on_send_validation_req_to_server,
							   shared_from_this(),
							   std::placeholders::_1,
							   std::placeholders::_2)));
  }

  void
  on_send_validation_req_to_server(
				   const boost::system::error_code& ec,
				   std::size_t bytes_transferred)
  {
    if(ec)
      return fail(ec, "on_send_validation_req_to_server", id_);
        
    do_recv_validation_response_from_server();
  }

  void
  do_recv_validation_response_from_server()
  {
    http::async_read(srv_sock_, validation_buffer_, validation_res,
		     boost::asio::bind_executor(
						strand_,
						std::bind(
							  &session::on_recv_validation_response_from_server,
							  shared_from_this(),
							  std::placeholders::_1,
							  std::placeholders::_2)));
  }

  void
  on_recv_validation_response_from_server(
					  const boost::system::error_code& ec,
					  std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    log_mutex.lock();
    std::stringstream ss;
    ss << validation_res.base();
    
    log_ << id_ << "Received revalidation from SERVER" << std::endl
	 << id_ << endl << ss.str() << std::endl;
      
    log_mutex.unlock();
    
    if(validation_res.result_int() == 304)
      {
	res_ = cached_res;
	do_http_send_res_to_client();
      }
    else if(validation_res.result_int() == 200)
      {
	res_ = validation_res;
	save_res_to_cache();
	do_http_send_res_to_client();
      }
  }


  bool
  need_validate()
  {
    return cached_response_no_cache() || cached_response_out_of_date();
  }

  bool
  cached_response_no_cache()
  {
    if(cached_res.base()["Cache-Control"] != "")
      {
	if(cached_res.base()["Cache-Control"].find("no-cache") != std::string::npos)
	  {
	    log(id_ + "in cache, requires validation");
	    return true;
	  }
      }
    return false;
  }

  bool
  cached_response_out_of_date()
  {
    time_t expired_time_in_gmt = cached_res_expired_time;
    time_t now = time(nullptr);
    struct tm gmt_buffer;
    time_t now_in_gmt = mktime(gmtime_r(&now, &gmt_buffer));
    
    char ctime_buffer[100];
    ctime_r(&expired_time_in_gmt, ctime_buffer);
    auto expired_time_in_gmt_string = std::string(ctime_buffer);
    expired_time_in_gmt_string.pop_back();
    expired_time_in_gmt_string += " GMT";
      
    if(now_in_gmt > expired_time_in_gmt)
      {
	log(id_ + "in cache, but expired at " + expired_time_in_gmt_string);
	return true;
      }
    return false;
  }


  // HTTPS methods section

  void
  do_https_send_200_OK_res()
  {
    // method to generate a 200-OK response
    // MUST NOT HAVE BODY
    auto generate_200_OK_response = []()
      {
	http::response<http::empty_body> res;
	res.result(200); 
	res.prepare_payload();
	return res;
      };

    // res_200_OK must survive until next callback, so we've made it a property
    res_200_OK = generate_200_OK_response(); 

    http::async_write(cli_sock_, res_200_OK,
		      boost::asio::bind_executor(
						 strand_,
						 std::bind(
							   &session::on_https_send_200_OK_res,
							   shared_from_this(),
							   std::placeholders::_1,
							   std::placeholders::_2)));
        
  }

  void
  on_https_send_200_OK_res(
			   boost::system::error_code ec,
			   std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);
       
    if(ec)
      return fail(ec, "on_https_send_200_OK_res", id_);
    log(id_ + "Successfully send 200-OK to client");
    do_cli_sock_read_some();
    do_srv_sock_read_some();
  }

  // Client --> Server

  void
  do_cli_sock_read_some()
  {
    cli_sock_.async_read_some(
			      srv_use_write_buf.prepare(read_buf_size),
			      std::bind(
					&session::on_cli_sock_read_some,
					shared_from_this(),
					std::placeholders::_1,
					std::placeholders::_2));
  }

  void
  on_cli_sock_read_some(
			const boost::system::error_code& ec,
			std::size_t bytes_transferred)
  {
    srv_use_write_buf.commit(bytes_transferred);
    if(ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset)
      return do_close();
    if(ec)
      return fail(ec, "on_cli_sock_read_some", id_);
        
    boost::asio::async_write(
			     srv_sock_,
			     srv_use_write_buf.data(),
			     std::bind(
				       &session::on_srv_sock_write,
				       shared_from_this(),
				       std::placeholders::_1,
				       std::placeholders::_2));
  }

  void
  on_srv_sock_write(const boost::system::error_code& ec,
		    std::size_t bytes_transferred)
  {
    srv_use_write_buf.consume(bytes_transferred);
    if(ec)
      return fail(ec, "on_srv_sock_write", id_);
        
    do_cli_sock_read_some();
  }

  // Server --> Client

  void
  do_srv_sock_read_some()
  {
    srv_sock_.async_read_some(
			      cli_use_write_buf.prepare(read_buf_size),
			      std::bind(
					&session::on_srv_sock_read_some,
					shared_from_this(),
					std::placeholders::_1,
					std::placeholders::_2));
  }

  void
  on_srv_sock_read_some(
			const boost::system::error_code& ec,
			std::size_t bytes_transferred)
  {
    cli_use_write_buf.commit(bytes_transferred);
    if(ec == boost::asio::error::eof){
      log(id_ + "Responding RESPONSE");
      return do_close();
    }
    if(ec)
      return fail(ec, "on_srv_sock_read_some", id_);
        
    boost::asio::async_write(
			     cli_sock_,
			     cli_use_write_buf.data(),
			     std::bind(
				       &session::on_cli_sock_write,
				       shared_from_this(),
				       std::placeholders::_1,
				       std::placeholders::_2));
  }

  void
  on_cli_sock_write(
		    const boost::system::error_code& ec,
		    std::size_t bytes_transferred)
  {
    cli_use_write_buf.consume(bytes_transferred);
    if(ec)
      return fail(ec, "on_cli_sock_write_some", id_);
        
    do_srv_sock_read_some();
  }

  /////////////////////////////////////////////////////////////////////////////////
  // HTTP methods section

  void
  do_http_send_req_to_server()
  {
    // Send req_ to server
    log_mutex.lock();
    float version = (req_.version() == 11)? 1.1:1.0;
    log_ << id_ << "Requesting " << req_.method() << " " << req_.target()
	 << " " << "HTTP/" << version << " from "
	 << req_.base()["Host"] << std::endl;
      log_mutex.unlock();
    http::async_write(srv_sock_, req_,
		      boost::asio::bind_executor(
						 strand_,
						 std::bind(
							   &session::on_http_send_req_to_server,
							   shared_from_this(),
							   std::placeholders::_1,
							   std::placeholders::_2)));
  }

  void
  on_http_send_req_to_server(
			     boost::system::error_code ec,
			     std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    if(ec)
      return fail(ec, "on_http_send_req_to_server", id_);
        
    do_http_recv_res_from_server();
  }

  void
  do_http_recv_res_from_server()
  {
    // Make a new response container
    res_ = std::move(http::response<http::dynamic_body>());

    // Receive res_ from server
    http::async_read(srv_sock_, srv_http_buffer_, res_,
		     boost::asio::bind_executor(
						strand_,
						std::bind(
							  &session::on_http_recv_res_from_server,
							  shared_from_this(),
							  std::placeholders::_1,
							  std::placeholders::_2)));
  }

  void
  on_http_recv_res_from_server(
			       boost::system::error_code ec,
			       std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    // Server closed the connection
    if(ec == http::error::end_of_stream)
      return do_close();
    if(ec)
    {
      auto generate_400_BAD_REQUEST_response = []()
      {
      http::response<http::dynamic_body> res;
      res.result(400); 
      res.prepare_payload();
      return res;
      };
      res_400_BAD_REQUEST = generate_400_BAD_REQUEST_response();
      res_ = res_400_BAD_REQUEST;
      do_http_send_res_to_client();
    }else
      {
	log_mutex.lock();
    
	float version = (res_.version() == 11)? 1.1:1.0;
	log_ << id_ << "Received HTTP/"<< version << " "
	     << res_.result_int() << " from " << req_.base()["Host"] << std::endl;
	/*
	//used to check response content
	std::stringstream ss;
	ss << res_.base();
	log_ << id_ << ss.str() << std::endl;
	*/
    
	log_mutex.unlock();
	save_res_to_cache();
	do_http_send_res_to_client();
      }
  }

  void
  do_http_send_res_to_client()
  {
    
    std::stringstream ss;
    float version = (res_.version() == 11)? 1.1:1.0;
    ss << "HTTP/"<< version << " " << res_.result_int();
    log(id_ + "Responding " + ss.str());
    http::async_write(cli_sock_, res_,
		      boost::asio::bind_executor(
						 strand_,
						 std::bind(
							   &session::on_http_send_res_to_client,
							   shared_from_this(),
							   std::placeholders::_1,
							   std::placeholders::_2)));
  }

  void
  save_res_to_cache()
  {
    
    if(cached_res.base()["Cache-Control"].find("private") != std::string::npos)
      {
	log(id_ + "not cacheable because PRIVATE");
	return;
      }
    if(cached_res.base()["Cache-Control"].find("no-store") != std::string::npos)
      {
	log(id_ + "not cacheable because NO-STORE");
	return;
      }
    if(cached_res.base()["Cache-Control"] == "" && expire_time_string_not_in_GMT_format(std::string(cached_res.base()["Expires"])))
      {
	log(id_ + "not cacheable because no Cache-Control and Expires");
	return;
      }

    log(id_ + "NOTE cache the response");
    auto key = std::string(req_.target());
    auto response = res_;
    auto expire_time = get_expire_time(res_);
    
    auto evicted = lru_cache_.store(key, std::make_pair(response, expire_time));
    
    if(std::get<1>(evicted) != "")
      {
	log("NOTE evicted " + std::get<1>(evicted));
      }
  }

  bool
  expire_time_string_not_in_GMT_format(std::string time_string)
  {
    struct tm tm;
        strptime(time_string.c_str(), "%a, %d %b %Y %H:%M:%S %Z", &tm);
    return (mktime(&tm) < 0) ? true : false;
  }


    
  std::string
  parse_key_equal_value_pair(std::string const key, string const target)
  {
    const string pattern = key + "=([[:digit:]]+)";
    boost::regex regexPattern(pattern, boost::regex::extended);
    boost::smatch what;
    if(boost::regex_search(target, what, regexPattern))
      return what[1];
    return std::string("");
  }

  // Will return -1 if string is not in format at all
    time_t
    gmt_time_to_time_t(std::string time_string)
    {
        struct tm tm;
        strptime(time_string.c_str(), "%a, %d %b %Y %H:%M:%S %Z", &tm);
        // Attention, we have to manually set tm_isdst as -1 to let system deal with daylight saving
        tm.tm_isdst = -1;
        return mktime(&tm);
    }

  // Attention: Have to make sure that res has s_maxage or max_age or expires
  time_t
  get_expire_time(http::response<http::dynamic_body>& res)
  {
    std::string s_maxage_string = parse_key_equal_value_pair("s-maxage", std::string(res.base()["Cache-Control"]));
    std::string maxage_string = parse_key_equal_value_pair("max-age", std::string(res.base()["Cache-Control"]));
    std::string expires = std::string(res.base()["Expires"]);
    
      //mark
    if(s_maxage_string != "")
      {
          time_t now = time(nullptr);
          struct tm gmt_buffer;
          time_t now_in_gmt = mktime(gmtime_r(&now, &gmt_buffer));
          return now_in_gmt + std::stoi(s_maxage_string);
      }
      
    else if(maxage_string != "")
      {
          time_t now = time(nullptr);
          struct tm gmt_buffer;
          time_t now_in_gmt = mktime(gmtime_r(&now, &gmt_buffer));
          return now_in_gmt + std::stoi(maxage_string);
      }
    else
      return gmt_time_to_time_t(expires);
  }

  void
  on_http_send_res_to_client(
			     boost::system::error_code ec,
			     std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    if(ec)
      return fail(ec, "on_http_recv_res_from_server", id_);
        
    do_http_recv_req_from_client();
  }

  void
  do_http_recv_req_from_client()
  {
    // Make a new request container
    req_ = std::move(http::request<http::string_body>());

    // Receive req_ from server
    http::async_read(cli_sock_, cli_http_buffer_, req_,
		     boost::asio::bind_executor(
						strand_,
						std::bind(
							  &session::on_http_recv_req_from_client,
							  shared_from_this(),
							  std::placeholders::_1,
							  std::placeholders::_2)));
  }

  void
  on_http_recv_req_from_client(
			       boost::system::error_code ec,
			       std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    // Client closed the connection
    if(ec == http::error::end_of_stream)
      return do_close();
    if(ec)
      {
	auto generate_400_BAD_REQUEST_response = []()
	  {
	    http::response<http::dynamic_body> res;
	    res.result(400); 
	    res.prepare_payload();
	    return res;
	  };
	res_400_BAD_REQUEST = generate_400_BAD_REQUEST_response();
	res_ = res_400_BAD_REQUEST;
	do_http_send_res_to_client();
      }else
      {
	do_http_send_req_to_server();
      }
  }

  void
  do_close()
  {
    // Send a TCP shutdown
    boost::system::error_code ec;
    log(id_ + "Tunnel closed");
    srv_sock_.shutdown(tcp::socket::shutdown_both, ec);
    cli_sock_.shutdown(tcp::socket::shutdown_both, ec);
  }
};
////////////////////////////////////////////////////////////////////////////////////
class listener : public std::enable_shared_from_this<listener>
{
private:
  tcp::acceptor acceptor_;
  tcp::socket srv_sock_;
  tcp::socket cli_sock_;
  net::io_context& ioc_;
  boost::asio::signal_set signals_;
  LRUCache<std::string, std::pair<http::response<http::dynamic_body>, time_t>>& lru_cache_;
  unsigned long id;
  //std::mutex& cache_mutex_;
  void become_daemon(){
    signals_.async_wait(
			boost::bind(&boost::asio::io_service::stop, &ioc_));

    ioc_.notify_fork(boost::asio::io_service::fork_prepare);

    if (pid_t pid = fork())
      {
	if (pid > 0)
	  {
	    exit(0);
	  }
	else
	  {
	    log("Become Daemon: first fork failed");
	    exit(EXIT_FAILURE);
	  }
      }
    setsid();
    chdir("/");
    umask(0);

    if (pid_t pid = fork())
      {
	if (pid > 0)
	  {
	    exit(0);
	  }
	else
	  {
	    log("Become Daemon: second fork failed");
	    exit(EXIT_FAILURE);
     
	  }
      }
    close(0);
    close(1);
    close(2);

    // We don't want the daemon to have any standard input.
    if (open("/dev/null", O_RDONLY) < 0)
      {
	log("Unable to open /dev/null");
	exit(EXIT_FAILURE);
     
      }
    
    ioc_.notify_fork(boost::asio::io_service::fork_child);

    // The io_service can now be used normally.
    log("Daemon started");
  }

public:
  listener(
	   boost::asio::io_context& ioc,
	   tcp::endpoint endpoint,
	   LRUCache<std::string, std::pair<http::response<http::dynamic_body>, time_t>>& lru_cache)
    : acceptor_{ioc}
    , srv_sock_{ioc}
    , cli_sock_{ioc}
    , ioc_{ioc}
    , signals_{ioc_, SIGINT, SIGTERM, SIGHUP}
    , lru_cache_{lru_cache}
    , id{0}
      //, cache_mutex_{cache_mutex}
  {
    boost::system::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if(ec)
      {
	fail(ec, "open acceptor, listener init", "(no id)");
	return;
      }

    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if(ec)
      {
	fail(ec, "acceptor set_option, listener init","(no id)");
	return;
      }
        
    acceptor_.bind(endpoint, ec);
    if(ec)
      {
	fail(ec, "bind acceptor, listener init","(no id)");
	return;
      }
            
    become_daemon();
	
    acceptor_.listen(
		     boost::asio::socket_base::max_listen_connections, ec);
    if(ec)
      {
	fail(ec, "listen acceptor, listener init", "(no id)");
	return;
      }
            
  }

  void
  run()
  {
    if(! acceptor_.is_open())
      return;

    do_accept();
  }

  void
  do_accept()
  {
    acceptor_.async_accept(
			   cli_sock_,
			   std::bind(
				     &listener::on_accept,
				     shared_from_this(),
				     std::placeholders::_1));
  }

  void
  on_accept(boost::system::error_code ec)
  {
    if(ec)
      return fail(ec, "on_accept", "(no id)");

	
    std::make_shared<session>(
			      std::move(srv_sock_),
			      std::move(cli_sock_),
			      ioc_,
			      lru_cache_,
			      id)->run();

    id++;
    do_accept();
  }
};


int main(int argc, char* argv[])
{
  log_.open(LOG_FILE_PATH,std::fstream::in | std::fstream::out | std::fstream::trunc);
  if(!log_.is_open()){
    cerr << "log file can't be opened/created" << std::endl;
    exit(EXIT_FAILURE);
  }

  //drop root privilege after open the file
  setuid(1001);

  std::cout << "Server start\n" << std::endl;
  log("Server start");
  //start listen and receive request from client
  auto const address = net::ip::make_address("127.0.0.1");
  auto const port = static_cast<unsigned short>(std::atoi("12345"));
  auto const threads = std::max<int>(1, std::atoi("4"));

  LRUCache<std::string, std::pair<http::response<http::dynamic_body>, time_t>> lru_cache{CACHE_LINES};
  //std::mutex cache_mutex;
    
  net::io_context ioc{threads};
    
  std::make_shared<listener>(
			     ioc,
			     tcp::endpoint(address, port),
			     lru_cache)->run();

  std::vector<std::thread> v;
  v.reserve(threads - 1);
  for(auto i = threads - 1; i > 0; --i)
    v.emplace_back(
		   [&ioc]
		   {
		     ioc.run();
		   });
  ioc.run();
    
  return EXIT_SUCCESS;    
}
