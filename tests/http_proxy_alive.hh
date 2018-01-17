#ifndef HTTP_PROXY_ALIVE_HH
#define HTTP_PROXY_ALIVE_HH
#include "core/seastar.hh"
#include "core/sstring.hh"
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/future-util.hh"
#include "net/inet_address.hh"
#include "net/dns.hh"
#include "http/request_parser.hh"
#include "http/http_response_parser.hh"
#include "http/request.hh"
#include "http/reply.hh"
#include "tests/log.hh"
#include <boost/intrusive/list.hpp>

using namespace seastar;
using namespace std;

#define HTTP_PORT	80

struct proxy_request {
	unique_ptr<httpd::request> request;

	unique_ptr<httpd::request> &operator->() {
		return request;
	}

	int extract_url() {
		size_t pos;

		if(!request) return -1;

		if(request->_url.find("http://") == 0) {
			pos = request->_url.find('/', 7);

			//get host if Host header does not exist
			if(request->_headers.find("Host") == request->_headers.end()){
				sstring hostname = request->_url.substr(7, pos - 7);
				size_t pos2;

				//strip username
				pos2 = hostname.find('@');
				if(pos2 != hostname.size()){
					hostname = hostname.substr(pos2, hostname.size() - pos2);
				}

				request->_headers.insert({"Host", hostname});
			}

			if(pos == request->_url.size()){
				request->_url = "/";
			}
			else {
				request->_url = request->_url.substr(pos, request->_url.size() - pos);
			}
		}

		return 0;
	}

	sstring get_request_line() {
		if(request->_version.empty())
			return request->_method + " " + request->_url + "\r\n";
		else
			return request->_method + " " + request->_url + " HTTP/" + request->_version + "\r\n";
	}

	// future<> send_headers(output_stream<char> &out) {
	// 	return do_for_each(request->_headers, [&out] (auto& h) {
	//         return out.write(h.first + ": " + h.second + "\r\n");
	//     });
	// }


	future<> send_without_body(output_stream<char> &out) {
		try {
			log_message("Sending request to server...\n");
			log_message(get_request_line());
			//print_headers(request->_headers);
			sstring temp_s = get_request_line();
			for(auto it = request->_headers.begin(); it != request->_headers.end(); ++it){
				//log_message("%s:%s\n", map_it->first.c_str(), map_it->second.c_str());
				temp_s = temp_s + it->first + ": " + it->second + "\r\n";
			}
			temp_s = temp_s + "\r\n";
	
			return out.write(temporary_buffer<char>{temp_s.c_str(), temp_s.size()});
		}
		catch(std::exception &ex) {
			std::cerr << "proxy_request::send_without_body() error " << ex.what() << std::endl;
			return make_exception_future(ex);
		}
	}
};

struct proxy_response {
	unique_ptr<httpd::reply> response;
	size_t content_length = 0;

	proxy_response() {}

	proxy_response(proxy_response &&) = default;
	proxy_response &operator=(proxy_response &&) = default;

	proxy_response(http_response_parser &response_parser) {
		//initiate response
		response = std::make_unique<httpd::reply>();
		response->_version = std::move(response_parser._rsp->_version);
		response->_headers = std::move(response_parser._rsp->_headers);
		if(sizeof(response->_status) == sizeof(int))
			sscanf(response_parser._rsp->_status_code.c_str(), "%d", reinterpret_cast<int *>(&response->_status));
		else if(sizeof(response->_status) == sizeof(short))
			sscanf(response_parser._rsp->_status_code.c_str(), "%hd", reinterpret_cast<short *>(&response->_status));
		else if(sizeof(response->_status) == sizeof(long))
			sscanf(response_parser._rsp->_status_code.c_str(), "%ld", reinterpret_cast<long *>(&response->_status));
		else{
			assert(0 && "proxy_response(): No matching size to response->_status.");
		}
	}

	unique_ptr<httpd::reply> &operator->() {
		return response;
	}

	sstring get_response_line() {
		return response->response_line();
	}
	
	// future<> send_headers(output_stream<char> &out) {
	// 	return do_for_each(response->_headers, [&out] (auto& h) {
	//         return out.write(h.first + ": " + h.second + "\r\n");
	//     });
	// }

	future<> send_without_body(output_stream<char> &out) {
		try{
			log_message("Sending response to client...\n");
			log_message(get_response_line());
			//print_headers(response->_headers);
			sstring temp_s = get_response_line();
			for(auto it = response->_headers.begin(); it != response->_headers.end(); ++it){
				//log_message("%s:%s\n", map_it->first.c_str(), map_it->second.c_str());
				temp_s = temp_s + it->first + ": " + it->second + "\r\n";
			}
			temp_s = temp_s + "\r\n";

			return out.write(temporary_buffer<char>{temp_s.c_str(), temp_s.size()});
		}
		catch(std::exception &ex){
			std::cerr << "proxy_response::send_without_body() error " << ex.what() << std::endl;
			return make_exception_future(ex);
		}
	}
};

class proxy_client_connection;

class proxy_server_connection {
	proxy_client_connection &_client_connection;
	sstring _hostname;
	connected_socket _fd;
	input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    http_response_parser _parser;
    proxy_response _response;
    bool _keep_alive = false;
    bool _done = false;

public:
	proxy_server_connection() = delete;
	proxy_server_connection(proxy_client_connection &client_connection, sstring hostname, connected_socket fd) : 
		_client_connection(client_connection),
		_hostname(hostname), 
		_fd(std::move(fd)), 
		_read_buf(_fd.input()), 
		_write_buf(_fd.output()) {

	}
	proxy_server_connection(proxy_server_connection &&) = default;
	proxy_server_connection &operator=(proxy_server_connection &&) = default;

	~proxy_server_connection() {
		_fd.shutdown_output();
		_fd.shutdown_input();
	}

	void shutdown() {
		_fd.shutdown_input();
		_fd.shutdown_output();
	}

	bool can_reuse(const sstring &hostname) {
		return _keep_alive && (hostname == _hostname);
	}

	bool alive() {
		return _keep_alive;
	}

	future<> read_one() {
		_parser.init();
		return _read_buf.consume(_parser).then([this] {
			if(_parser.eof()){
				_done = true;
				return make_ready_future();
			}

			_response = proxy_response(_parser);
			return make_ready_future();
		});
	}

	future<> process();
};

class http_proxy_server;

class proxy_client_connection : public boost::intrusive::list_base_hook<> {
	http_proxy_server &_server;
	connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    http_request_parser _parser;
    proxy_request _request;
    bool _keep_alive = false;
    bool _done = false;
    //sstring _last_host;
    unique_ptr<proxy_server_connection> _server_conn;
	
public:
	proxy_client_connection(http_proxy_server& server, connected_socket&& fd) :
		_server(server), 
		_fd(std::move(fd)),
		_read_buf(_fd.input()),
		_write_buf(_fd.output()) {
		on_new_connection();
	}
	proxy_client_connection(proxy_client_connection &&) = default;
	proxy_client_connection &operator=(proxy_client_connection &&) = default;

	void on_new_connection();
	~proxy_client_connection();

	void shutdown() {
		if(_server_conn) _server_conn->shutdown();
		_fd.shutdown_input();
		_fd.shutdown_output();
	}

	future<> connect_to_server(const sstring &hostname, int port) {
		net::dns_resolver dns;
		
		//log_message("Host:%s:%d\n", hostname.c_str(), port);
		//connect to server
		return dns.resolve_name(hostname).then([this, port] (auto &&h) {
			return engine().net().connect(ipv4_addr(h, port)).then_wrapped([this] (auto &&f) {
				if(f.failed()){
					f.ignore_ready_future();
					print("proxy_client_connection::connect_to_server(): Unable to connect to remote server.\n");
					throw 500;
				}

				return std::move(f);
			});
		}).then([this, hostname] (auto &&sock) {
			_server_conn.reset(new proxy_server_connection(*this, hostname, std::move(sock)));
		});
	}

	future<> do_process() {
		return make_ready_future().then([this] {
			_request.request = _parser.get_parsed_request();

			//process client request
			if(_request.extract_url()) {
				assert(0 && "extract_url() failed.");
			}
			//get content length
			_request->content_length = get_content_length(_request->_headers);
			if(_request->content_length < 0) {
				assert(0 && "get_content_length() failed.");
			}

			bool conn_keep_alive = false;
			bool conn_close = false;
			//find if it needs to keep this connection alive
			auto it = _request->_headers.find("Connection");
		    if (it != _request->_headers.end()) {
		        if (it->second == "Keep-Alive") {
		            conn_keep_alive = true;
		        } else if (it->second == "Close") {
		            conn_close = true;
		        }
		    }

			if(_request->_version == "1.0") {
				_keep_alive = conn_keep_alive;
			}
			else if(_request->_version == "1.1") {
				_keep_alive = !conn_close;
			}
			else {
				//for HTTP/0.9, do not keep alive
				_keep_alive = false;
			}

			//find server host name and port
			int port;
			sstring hostname = get_host_name(port);

			//alive connection exists
			if(_server_conn && _server_conn->can_reuse(hostname)) {
				log_message("proxy_client_connection: Server connection still alive, do not need to reconnect.\n");
				return make_ready_future();
			}

			//connect server
			log_message("proxy_client_connection: Connect to a new server.\n");
			return connect_to_server(hostname, port);
		}).then([this] {
			static const char *skipheaders[] = {
				"Proxy-Connection",
				"Te",
				"Trailers",
				"Upgrade"
			};
			return remove_headers(_request->_headers, skipheaders, sizeof(skipheaders) / sizeof(char *));
		}).then([this] {
			log_message("proxy_client_connection: Server connection processing.\n");
			return _server_conn->process();
		}).then([this] {
			if(_keep_alive) {
				log_message("proxy_client_connection: Connection remains.\n");
				_done = false;
			}
			else {
				log_message("proxy_client_connection: Connection is being shut down.\n");
				_done = true;
			}
		});
	}

	future<> process_one() {
		_parser.init();
		return _read_buf.consume(_parser).then([this] {
			if(_parser.eof()) {
				log_message("proxy_client_connection: No more request. Shut down the connection.\n");
				_done = true;
				return make_ready_future();
			}

			return do_process();
		});	
	}

	future<> process() {
		return do_until([this] {
			return _done;
		}, [this] {
			log_message("proxy_client_connection: Start processing one request.\n");
			return process_one();
		}).then_wrapped([this] (auto &&f) {
			if(f.failed()){
				f.ignore_ready_future();
			}
		}).finally([this] {
			log_message("proxy_client_connection: Connection shut down finally.\n");
			return _read_buf.close().then([this] {
				return _write_buf.close();
			});
		});
	}

private:
	//forward len Bytes from dest to src
	static future<> forward_data_zero_copy(output_stream<char> &dest, input_stream<char> &src, size_t len) {
		return do_with(std::move(len), [&dest, &src] (size_t &len) {
			return do_until([&len] {
				return len == 0;
			}, [&dest, &src, &len] {
				return src.read().then([&dest, &len] (auto &&buf) {
					if(buf.empty()){
						len = 0;	//end loop
						return make_ready_future<>();
					}
					len -= buf.size();
					return dest.write(std::move(buf));
				});
			});
		});
	}

	static size_t get_content_length(std::unordered_map<sstring, sstring> &headers) {
		size_t content_length;
		auto entry = headers.find({"Content-Length"});
		if(entry != headers.end()){
			if(sscanf(entry->second.c_str(), "%ld", &content_length) != 1){
				log_message("-----------------------Content Length: error----------------------------\n");
				return -1;
			}
		}
		else{
			log_message("--------------------------Content Length: 0-----------------------------\n");
			return 0;	
		}

		log_message("-------------------------Content Length: %d----------------------------\n", content_length);
		return content_length;
	}

	static future<> remove_headers(std::unordered_map<sstring, sstring> &headers, const char *list[], int n) {
		for(int i = 0; i < n; ++i) {
			headers.erase(list[i]);
		}
		return make_ready_future<>();
	}

	sstring get_host_name(int &port) {
		sstring hostname, port_s;
		sstring hostname_port = _request->get_header("Host");
		size_t p = hostname_port.find_last_of(':');
		if(p >= 0 && p < hostname_port.size()){
			hostname = hostname_port.substr(0, p);
			port_s = hostname_port.substr(p + 1);
			if(sscanf(port_s.c_str(), "%d", &port) != 1)
				assert(0 && "get_host_name() ERROR: sscanf failed.");
		}
		else{
			hostname = std::move(hostname_port);
			port = HTTP_PORT;
		}

		return std::move(hostname);
	}

	friend class proxy_server_connection;
};

class http_proxy_server {
	server_socket _listener;
	uint64_t _connections_being_accepted = 0;
	uint64_t _current_connections = 0;
	bool _stopping = false;
	promise<> _all_connections_stopped;
	future<> _stopped = _all_connections_stopped.get_future();
	boost::intrusive::list<proxy_client_connection> _connections;
private:
	friend class proxy_client_connection;
    void maybe_idle() {
        if (_stopping && !_connections_being_accepted && !_current_connections) {
            _all_connections_stopped.set_value();
        }
    }
public:
	http_proxy_server() {}

	future<> listen(ipv4_addr addr) {
		listen_options lo;
		lo.reuse_address = true;
		_listener = engine().listen(make_ipv4_address(addr), lo);
		_stopped = when_all(std::move(_stopped), do_accepts()).discard_result();
		return make_ready_future();
	}

	future<> stop() {
		_stopping = true;
		_listener.abort_accept();
		for (auto&& c : _connections) {
            c.shutdown();
        }

        return std::move(_stopped);
	}

	future<> do_accepts() {
		++_connections_being_accepted;
        return _listener.accept().then_wrapped(
                [this] (future<connected_socket, socket_address> f_cs_sa) mutable {
            --_connections_being_accepted;
            if (_stopping || f_cs_sa.failed()) {
                f_cs_sa.ignore_ready_future();
                maybe_idle();
                return;
            }
            auto cs_sa = f_cs_sa.get();
            auto conn = new proxy_client_connection(*this, std::get<0>(std::move(cs_sa)));
            conn->process().then_wrapped([conn] (auto&& f) {
                delete conn;
                try {
                    f.get();
                } catch (std::exception& ex) {
                    std::cerr << "request error " << ex.what() << std::endl;
                }
            });
            do_accepts();
        }).then_wrapped([] (auto f) {
            try {
                f.get();
            } catch (std::exception& ex) {
                std::cerr << "accept failed: " << ex.what() << std::endl;
            }
        });
	}
};

#endif