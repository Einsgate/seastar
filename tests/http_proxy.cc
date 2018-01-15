#include "core/seastar.hh"
#include "core/future-util.hh"
#include "core/app-template.hh"
#include "core/reactor.hh"
#include "net/inet_address.hh"
#include "net/dns.hh"
#include "core/sstring.hh"
#include "http/request_parser.hh"
#include "http/http_response_parser.hh"
#include "http/request.hh"
#include "http/reply.hh"
#include "core/distributed.hh"
#include <iostream>
#include <cstring>
#include "tests/log.hh"
#include "tests/http_proxy.hh"

using namespace seastar;
using namespace std;

enum{
	OK = 200, //!< OK
	CREATED = 201, //!< CREATED
	ACCEPTED = 202, //!< ACCEPTED
	NO_CONTENT = 204, //!< NO_CONTENT
	MULTIPLE_CHOICES = 300, //!< MULTIPLE_CHOICES
	MOVED_PERMANENTLY = 301, //!< MOVED_PERMANENTLY
	MOVED_TEMPORARILY = 302, //!< MOVED_TEMPORARILY
	NOT_MODIFIED = 304, //!< NOT_MODIFIED
	BAD_REQUEST = 400, //!< BAD_REQUEST
	UNAUTHORIZED = 401, //!< UNAUTHORIZED
	FORBIDDEN = 403, //!< FORBIDDEN
	NOT_FOUND = 404, //!< NOT_FOUND
	INTERNAL_SERVER_ERROR = 500, //!< INTERNAL_SERVER_ERROR
	NOT_IMPLEMENTED = 501, //!< NOT_IMPLEMENTED
	BAD_GATEWAY = 502, //!< BAD_GATEWAY
	SERVICE_UNAVAILABLE = 503  //!< SERVICE_UNAVAILABLE
};

void print_headers(std::unordered_map<sstring, sstring> &headers);

struct proxy_request {
	unique_ptr<httpd::request> request;

	unique_ptr<httpd::request> &operator->() {
		return request;
	}

	int init_url() {
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

	future<> send_headers(output_stream<char> &out) {
		return do_for_each(request->_headers, [&out] (auto& h) {
	        return out.write(h.first + ": " + h.second + "\r\n");
	    });
	}


	future<> send_without_body(output_stream<char> &out) {
		//log_message("Sending request to server...\n");
		//log_message(get_request_line());
		//print_headers(request->_headers);
		sstring temp_s = get_request_line();
		for(auto it = request->_headers.begin(); it != request->_headers.end(); ++it){
			//log_message("%s:%s\n", map_it->first.c_str(), map_it->second.c_str());
			temp_s = temp_s + it->first + ": " + it->second + "\r\n";
		}
		temp_s = temp_s + "\r\n";

		return out.write(temporary_buffer<char>{temp_s.c_str(), temp_s.size()});
	}
};

struct proxy_response {
	unique_ptr<httpd::reply> response;
	size_t content_length = 0;

	unique_ptr<httpd::reply> &operator->() {
		return response;
	}

	sstring get_response_line() {
		return response->response_line();
	}
	
	future<> send_headers(output_stream<char> &out) {
		return do_for_each(response->_headers, [&out] (auto& h) {
	        return out.write(h.first + ": " + h.second + "\r\n");
	    });
	}

	future<> send_without_body(output_stream<char> &out) {
		//log_message("Sending response to client...\n");
		//log_message(get_response_line());
		//print_headers(response->_headers);
		sstring temp_s = get_response_line();
		for(auto it = response->_headers.begin(); it != response->_headers.end(); ++it){
			//log_message("%s:%s\n", map_it->first.c_str(), map_it->second.c_str());
			temp_s = temp_s + it->first + ": " + it->second + "\r\n";
		}
		temp_s = temp_s + "\r\n";

		return out.write(temporary_buffer<char>{temp_s.c_str(), temp_s.size()});
	}
};

class http_proxy_connect {
	size_t _id;
	connected_socket c_fd;
	connected_socket s_fd;
	socket_address client_addr;
	input_stream<char> c_in;
	input_stream<char> s_in;
	output_stream<char> c_out;
	output_stream<char> s_out;
	http_request_parser request_parser;
	http_response_parser response_parser;
	proxy_request request;
	proxy_response response;

	using Headers = std::unordered_map<sstring, sstring>;
	
	template <typename Parser>
	future<> read_with_parser(input_stream<char> &in, Parser &parser);
	future<> remove_headers(Headers &headers, const char *list[], int n);
	future<> forward_data_zero_copy(output_stream<char> &dest, input_stream<char> &src, size_t len);
	future<> forward_data_zero_copy_flush(output_stream<char> &dest, input_stream<char> &src, size_t len);
	future<> handle_http();
	future<> handle_https();
	
public:
	http_proxy_connect(connected_socket &&s, socket_address &&a, size_t id) : _id(id), c_fd(std::move(s)), client_addr(std::move(a)), 
							c_in(c_fd.input()), c_out(c_fd.output()) {}
	size_t get_content_length(Headers &headers);
	future<> handle();
};

class http_proxy_server {
	server_socket _listener;
	semaphore conn_id{1};
	size_t id = 0;
public:
	future<> do_accepts(server_socket &listener) {
		//log_message("accept() BEGINS.\n");
		return listener.accept().then([&listener, this] (seastar::connected_socket s, seastar::socket_address a) {
			//log_message("Here comes a new connection...\n");
			return conn_id.wait(1).then([s = std::move(s), a = std::move(a), &listener, this] () mutable {
				//log_message("Here comes a new connection %d\n", id);
				http_proxy_connect *conn = new http_proxy_connect(std::move(s), std::move(a), id);
				id++;
				conn_id.signal(1);
			
				if(conn) {
					conn->handle().then_wrapped([conn] (auto &&f) {
						//log_message("handle() finished.");
						f.ignore_ready_future();
						delete conn;
					});
				}
				else{
					std::cerr << "do_accepts() ERROR: Out of memory.\n";
				}

				this->do_accepts(listener);
			});
		}).then_wrapped([] (auto f) {
            try {
                f.get();
            } catch (std::exception& ex) {
                std::cerr << "do_accepts() ERROR: accept failed: " << ex.what() << std::endl;
            }
        });
	}

	/*  
		Main proxy service loop
	*/
	future<> service_loop(){
		seastar::listen_options lo;
		lo.reuse_address = true;
		_listener = engine().listen(seastar::make_ipv4_address(6666), lo);
		do_accepts(_listener);
		return make_ready_future();
	}

	future<> stop() {
        return make_ready_future();
    }
};


void print_headers(std::unordered_map<sstring, sstring> &headers) {
	for(auto it = headers.begin(); it != headers.end(); it++) {
		log_message("%s: %s\n", it->first.c_str(), it->second.c_str());
	}
}

/*
	handle request from the connection
*/
seastar::future<> http_proxy_connect::handle_http(){
	static const char *skipheaders[] = {
		"Keep-Alive",
		"Connection",
		"Proxy-Connection",
		"Te",
		"Trailers",
		"Upgrade"
	};

	//extract url
	request.init_url();

	//get content length from headers
	if((request->content_length = get_content_length(request->_headers)) < 0) {
		log_message("Connection %d: handle(): Failed to get client request content length.\n", _id);
		throw INTERNAL_SERVER_ERROR;
	}

	//do some process with client headers
	return remove_headers(request->_headers, skipheaders, sizeof(skipheaders) / sizeof(char *)).then([this] {
		//do some more process with client headers
		request->_headers.insert({"Connection", "Close"});
		//record old version in http_version_major and http_version_minor
		if(request->_version == "1.0"){
			request->http_version_major = 1;
			request->http_version_minor = 0;
		}
		else if(request->_version == "1.1"){
			request->http_version_major = 1;
			request->http_version_minor = 1;
		}
		else if(request->_version == "2.0"){
			request->http_version_major = 2;
			request->http_version_minor = 0;
		}
		else{	//otherwise is HTTP/0.9
			request->http_version_major = 0;
			request->http_version_minor = 9;
		}
		//the request to server are always in HTTP/1.0 version
		request->_version = "1.0";

		//send request line and headers to server
		return request.send_without_body(s_out).then([this] {
			return s_out.flush();
		});
	}).then([this] {
		//forward http body from client to server 
		if(request->content_length > 0)
			forward_data_zero_copy(s_out, c_in, request->content_length).then([this] {
				return s_out.flush();
			});

		//read server response
		return read_with_parser(s_in, response_parser);
	}).then([this] {
		//initiate response
		response.response = std::make_unique<httpd::reply>();
		response->_version = std::move(response_parser._rsp->_version);
		response->_headers = std::move(response_parser._rsp->_headers);
		if(sizeof(response->_status) == sizeof(int))
			sscanf(response_parser._rsp->_status_code.c_str(), "%d", reinterpret_cast<int *>(&response->_status));
		else if(sizeof(response->_status) == sizeof(short))
			sscanf(response_parser._rsp->_status_code.c_str(), "%hd", reinterpret_cast<short *>(&response->_status));
		else if(sizeof(response->_status) == sizeof(long))
			sscanf(response_parser._rsp->_status_code.c_str(), "%ld", reinterpret_cast<long *>(&response->_status));
		else{
			log_message("Connection %d: handle(): No matching size to response->_status.\n", _id);
			throw INTERNAL_SERVER_ERROR;
		}
		log_message("Connection %d: Server Response: ", _id);
		log_message(response.get_response_line());
		//print_headers(response->_headers);
		
		//get content length
		if((response.content_length = get_content_length(response->_headers)) < 0){
			log_message("Connection %d: handle(): Failed to get server response content length.\n", _id);
			throw INTERNAL_SERVER_ERROR;
		}

		//for HTTP/0.9, do not response to client
		if(request->http_version_major < 1) 
			return make_ready_future<>();

		static const char *skipheaders[] = {
			"Connection",
			"Keep-Alive",
			"Proxy-Authenticate",
			"Proxy-Authorization",
			"Proxy-Connection"
		};

		//do some process with server headers
		return remove_headers(response->_headers, skipheaders, sizeof(skipheaders) / sizeof(char *)).then([this] {
			//response->_headers.insert({"Connection", "Close"});
			//send response line and headers to client
			return response.send_without_body(c_out).then([this] {
				return c_out.flush();
			});
		});
	}).then([this] {
		//forward http body from server to client
		if(response.content_length == 0) response.content_length = -1;	//indicate to receive data until server closes the connection
		return forward_data_zero_copy(c_out, s_in, response.content_length).then([this] {
			return c_out.flush();
		});
	});
}

#define SSL_CONNECTION_RESPONSE "HTTP/1.0 200 Connection established\r\n\r\n"

seastar::future<> http_proxy_connect::handle_https(){
	return c_out.write(temporary_buffer<char>{SSL_CONNECTION_RESPONSE, strlen(SSL_CONNECTION_RESPONSE)}).then([this] {
		return c_out.flush();
	}).then([this] {
		return when_all(forward_data_zero_copy_flush(s_out, c_in, -1), forward_data_zero_copy_flush(c_out, s_in, -1)).then([] (auto &&f) {
			return make_ready_future<>();
		});
	});
}

seastar::future<> http_proxy_connect::handle(){
	//read http request from client
	return read_with_parser(c_in, request_parser).then([this] {
		request.request = std::move(request_parser._req);
		log_message("Connection %d: Client Request: %s", _id, request.get_request_line().c_str());
		//print_headers(request->_headers);
	}).then([this] {
		net::dns_resolver dns;

		int port;
		sstring hostname, port_s;
		sstring hostname_port = request->get_header("Host");
		size_t p = hostname_port.find_last_of(':');
		if(p >= 0 && p < hostname_port.size()){
			hostname = hostname_port.substr(0, p);
			port_s = hostname_port.substr(p + 1);
			if(sscanf(port_s.c_str(), "%d", &port) != 1)
				throw INTERNAL_SERVER_ERROR;
		}
		else{
			hostname = std::move(hostname_port);
			port = HTTP_PORT;
		}
		//log_message("Host:%s:%d\n", hostname.c_str(), port);
		//connect to server
		return dns.resolve_name(hostname).then([this, port] (auto &&h) {
			return engine().net().connect(ipv4_addr(h, port)).then_wrapped([this] (auto &&f) {
				if(f.failed()){
					f.ignore_ready_future();
					print("Connection %d: handle(): Unable to connect to remote server(%s).\n", _id, request->get_header("Host").c_str());
					throw INTERNAL_SERVER_ERROR;
				}

				return std::move(f);
			});
		});
	}).then([this] (auto &&sock) {
		s_fd = std::move(sock);
		s_out = s_fd.output();
		s_in = s_fd.input();

		if(request->_method == "CONNECT") return this->handle_https();
		else return this->handle_http();
	}).then_wrapped([this] (auto &&f) {
		try {
			f.get();
			//log_message("\nhandle(): Success!!!\n");
		}
		catch(int err) {
			switch(err){
				case INTERNAL_SERVER_ERROR: log_message("Connection %d: handle(): Internal server error.\n", _id); break;
				default: log_message("Connection %d: handle(): Unfinished error.\n", _id); break;
			}

			goto fail;
		}
		catch(...) {
			log_message("Connection %d: handle(): Unknown error.\n", _id);
			goto fail;
			
		}

		return when_all(c_in.close(), c_out.close(), s_in.close(), s_out.close()).then([] (auto &&f) {
			return make_ready_future<>();
		});

	fail:
		return when_all(c_in.close(), c_out.close()).then([] (auto &&f) {
			return make_ready_future<>();
		});
	});
}

template <typename Parser>
future<> http_proxy_connect::read_with_parser(seastar::input_stream<char> &in, Parser &parser) {
	parser.init();
	return in.consume(parser).then([&parser, this] () mutable {
		if (parser.eof()) {
			log_message("Connection %d: read_with_parser(): Parsing failed.\n", _id);
			throw -1;
		}

		return make_ready_future<>();
	});
}

future<> http_proxy_connect::remove_headers(Headers &headers, const char *list[], int n) {
	for(int i = 0; i < n; ++i) {
		headers.erase(list[i]);
	}

	return make_ready_future<>();
}

size_t http_proxy_connect::get_content_length(Headers &headers) {
	size_t content_length;
	auto entry = headers.find({"Content-Length"});
	if(entry != headers.end()){
		if(sscanf(entry->second.c_str(), "%ld", &content_length) != 1){
			log_message("Connection %d: -----------------------Content Length: error----------------------------\n", _id);
			return -1;
		}
	}
	else{
		log_message("Connection %d: --------------------------Content Length: 0-----------------------------\n", _id);
		return 0;	
	}

	log_message("Connection %d: -------------------------Content Length: %d----------------------------\n", _id, content_length);
	return content_length;
}

future<> http_proxy_connect::forward_data_zero_copy(output_stream<char> &dest, input_stream<char> &src, size_t len) {
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

future<> http_proxy_connect::forward_data_zero_copy_flush(output_stream<char> &dest, input_stream<char> &src, size_t len) {
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
				return dest.write(std::move(buf)).then([&dest] {
					return dest.flush();
				});
			});
		});
	});
}

int main(int argc, char** argv) {
	seastar::app_template app;
	return app.run_deprecated(argc, argv, [] {
		auto *server = new distributed<http_proxy_server>;

		std::cout << "Start service loop...\n";
		return server->start().then([server] {
			return server->invoke_on_all(&http_proxy_server::service_loop);
		});
	});
}
