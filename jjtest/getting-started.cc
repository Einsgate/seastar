#include "core/seastar.hh"
#include "core/future-util.hh"
#include "core/app-template.hh"
#include "core/reactor.hh"
#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>

using namespace seastar;
using namespace std;

#define PROXY_DEBUG
//#define PROXY_LOG
#define LOG_FILE	stdout
#define DEBUG_FILE	stdout
#define MAX_READLINE_LEN	2000
#define HTTP_PORT 	80


class http_proxy_conn_s{
	struct request_s {
        string method;
        string protocol;

        string host;
        uint16_t port;

        string path;
	};

	//request_s::request_s() : method(nullptr), protocol(nullptr), host(nullptr), port(0), path(nullptr){}
	//using connected_socket = seastar::connected_socket;
	//using socket_address = seastar::socket_address;
	struct {
		unsigned int major;
		unsigned int minor;
	} protocol;

	request_s request;
	connected_socket fd;
	socket_address addr;
	input_stream<char> read_buf;
    output_stream<char> write_buf;
    temporary_buffer<char> readline_buf;
    temporary_buffer<char> readline_buf_ret;
    temporary_buffer<char> request_line;
    unordered_map<string, string> headers;
public:
	http_proxy_conn_s(connected_socket &&s, socket_address &&a) : fd(std::move(s)), addr(std::move(a)), read_buf(fd.input()), 
				write_buf(fd.output()) {
					//readline_buf = std::make_unique<char>(MAX_READLINE_SIZE);
					//readline_index = -1;
				}

	int extract_url(const char *url, int default_port);
	seastar::future<> handle();
	seastar::future<> read_request_line();
	seastar::future<> read_headers();
	seastar::future<> extract_request_line();
	seastar::future<> read_line();
	seastar::future<> read_line(temporary_buffer<char> &ret_buf);

};

void log_message(const char *fmt){
#ifdef PROXY_DEBUG
	fprintf(DEBUG_FILE, fmt, 0);
#endif
#ifdef PROXY_LOG
	fprintf(LOG_FILE, fmt, 0);
#endif
}

template <typename ... Args>
void log_message(const char *fmt, Args ... args){
#ifdef PROXY_DEBUG
	fprintf(DEBUG_FILE, fmt, args...);
#endif
#ifdef PROXY_LOG
	fprintf(LOG_FILE, fmt, args...);
#endif
}

template <typename T>
void log_message(T obj){
#ifdef PROXY_DEBUG
	obj();
#endif
}

/*
	handle request from the connection
*/
seastar::future<> http_proxy_conn_s::handle(){
	return read_request_line().then([this] {
		return read_headers();
	}).then([this] {
		log_message("Headers:\n");
		for(auto map_it = headers.begin(); map_it != headers.end(); ++map_it){
			log_message("%s:%s\n", map_it->first.c_str(), map_it->second.c_str());
		}
		return extract_request_line();
	}).then([this] {
		log_message("Request details:\n"
			"method: %s\n"
			"protocol: %s\n"
			"host: %s\n"
			"port: %u\n"
			"path: %s\n",
			request.method.c_str(),
			request.protocol.c_str(),
			request.host.c_str(),
			request.port,
			request.path.c_str()
		);
	});
}


/*
	Read the request line from client.

	Return value: When get the request line, resolve the return future.
*/
seastar::future<> http_proxy_conn_s::read_request_line(){
	return seastar::repeat([this] () {
		return read_line(request_line).then([this] () {
			log_message([this] {
				printf("Request line: ");
				std::fwrite(request_line.begin(), request_line.size(), 1, stdout);
				printf("\n");
			});
			
			//skip empty line
			if(request_line)
				return seastar::stop_iteration::yes;
			else
				return seastar::stop_iteration::no;
			
		});
	}).then_wrapped([] (auto &&f) {
		if(f.failed())
			log_message("read_request_line() failed.\n");
		
		return std::move(f);
	});
}

/*
	Read the headers from client. Store them into 'headers'

	Return value: When get all headers, resolve the return future.
*/
seastar::future<> http_proxy_conn_s::read_headers() {
	return seastar::repeat([this] () {
		return read_line().then([this] () {
			log_message([this] {
				printf("Header line: ");
				std::fwrite(readline_buf_ret.begin(), readline_buf_ret.size(), 1, stdout);
				printf("\n");
			});
			
			//continuous line
			//if(strncmp(" ", request_line.get(), 1) == 0 || strncmp("\t", request_line.get(), 1) == 0){
			//	return seastar::stop_iteration::no;
			//}
			if(!readline_buf_ret)
				return seastar::stop_iteration::yes;

			const char *line_begin = readline_buf_ret.get();
			const char *ptr = (char *)memchr(line_begin, ':', readline_buf_ret.size());

			if(!ptr || ptr == readline_buf_ret.end() || ptr == readline_buf_ret.begin()){
				log_message("read_headers(): Syntax error\n");
				throw 400;
			}
			else{
				string key, value;
				key.assign(line_begin, ptr - line_begin);
				value.assign(ptr + 1, readline_buf_ret.end() - ptr);
				headers.insert({key, value});
				return seastar::stop_iteration::no;
			}
			
		});
	}).then_wrapped([] (auto &&f) {
		if(f.failed())
			log_message("read_headers() failed.\n");
		
		return std::move(f);
	});
}


int http_proxy_conn_s::extract_url(const char *url, int default_port) {
	const char *p;
	int port = 0;

	/* Split the URL on the slash to separate host from path */
	p = strchr (url, '/');
	if (p) {
		request.host.assign(url, p - url);
		request.path.assign(p);
	} 
	else {
		request.host.assign(url);
		request.path.assign("/");
	}

	if (request.path.empty())
		return -1;
	
	/* Remove the username/password if they're present */
	p = strchr (request.host.c_str(), '@');
	if(p){
		request.host.assign(p + 1);
	}
	
	/* Find a proper port in www.site.com:8001 URLs */
	p = strchr (request.host.c_str(), ':');
	if(p){
		if(sscanf(p + 1, "%u", &port) != 1)
			return -1;
		request.host = request.host.substr(0, p - request.host.c_str());
	}

	if (request.host.empty())
		return -1;

	request.port = (port != 0) ? port : default_port;
	
/*
	// Remove any surrounding '[' and ']' from IPv6 literals 
	p = strrchr (request->host, ']');
	if (p && (*(request->host) == '[')) {
		memmove(request->host, request->host + 1, strlen(request->host) - 2);
		*p = '\0';
		p--;
		*p = '\0';
	}
*/
	
	return 0;
}


/*
	Extract details from request line. Results would be stored in 'request'

	Return value: ready future or exception future with error number if failed.
*/
seastar::future<> http_proxy_conn_s::extract_request_line(){
	int len = request_line.size();
	string url(len, 0);
	request.method.assign(len, 0);
	request.protocol.assign(len, 0);

	int ret = sscanf(request_line.get(), "%s %s %s", &request.method[0], &url[0], &request.protocol[0]);

	if (ret == 2 && !strcmp (request.method.c_str(), "GET")) {
		request.protocol.clear();
		
		/* Indicate that this is a HTTP/0.9 GET request */
		protocol.major = 0;
		protocol.minor = 9;
	} 
	else if(ret == 3 && !strncmp (request.protocol.c_str(), "HTTP/", 5)) {
		/*
		* Break apart the protocol and update the connection
		* structure.
		*/
		ret = sscanf (request.protocol.c_str() + 5, "%u.%u",
					&protocol.major,
					&protocol.minor);
		
		/*
		* If the conversion doesn't succeed, drop down below and
		* send the error to the user.
		*/
		if (ret != 2) {
			goto BAD_REQUEST_ERROR;
		}
	} 
	else{
BAD_REQUEST_ERROR:
		log_message("extract_request_line(): Syntax error in request line.\n");
		return make_exception_future<>(400);
	}

	//extract url
	if (strncmp (url.c_str(), "http://", 7) == 0) {
		const char *skipped_type = strstr(url.c_str(), "//") + 2;

		if (extract_url(skipped_type, HTTP_PORT) < 0) 
			return make_exception_future<>(400);
	}
/*	else if(strcmp(request.method.c_str(), "CONNECT") == 0) {
		if (extract_url(url.c_str(), HTTP_PORT_SSL) < 0) 
			return make_exception_future<>(400);

		// Verify that the port in the CONNECT method is allowed 
		if (!check_allowed_connect_ports (request->port, config.connect_ports)) 
			return make_exception_future<>(403);
			
		connect_method = TRUE;
	} 
*/
	else {
		return make_exception_future<>(501);
	}

	return make_ready_future<>();
}

/*
	Read a line from connection socket, the read line would be stored in 'readline_buf_ret' by default. If give 'ret_buf',
	the returned line would be stored in *ret_buf. Note: returned lines are without '\r\n'

	Return value: When get the line, returned future is resolved. 
*/
seastar::future<> http_proxy_conn_s::read_line(){
	return read_line(readline_buf_ret);
}

seastar::future<> http_proxy_conn_s::read_line(temporary_buffer<char> &ret_buf){
	return seastar::repeat([this, &ret_buf] () {
		if(readline_buf){	//if there is data left, check it first
			const char *buf_begin = readline_buf.begin();
			const char *ptr = std::strstr(buf_begin, "\r\n");
			//char *ptr = (char *)memchr(buf_begin, '\n', readline_buf.size());
			if(ptr){		//find a line, break the loop and save it in readline_buf_ret
				temporary_buffer<char> tmp_buf{buf_begin, (uintptr_t)ptr - (uintptr_t)buf_begin};
				readline_buf.trim_front(tmp_buf.size() + 2);
				ret_buf = std::move(tmp_buf);
				return make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
			}
		}

		//don't allow readline_buf increase infinitely
		if(readline_buf.size() > MAX_READLINE_LEN){
			log_message("read_line(): current line length exceeds limitation.\n");
			throw 408;
		}
		
		//didn't get a line from remaining data, read more, and continue to next loop
		return read_buf.read().then([this] (auto buf) {
			if(!readline_buf){	//no data left in readline_buf, move buf into readline_buf directly
				readline_buf = std::move(buf);
			}
			else{	//readline_buf still has data, join readline_buf and buf together
				temporary_buffer<char> temp_buf{buf.size() + readline_buf.size()};
				char *temp_buf_ptr = temp_buf.get_write();

				std::copy(readline_buf.begin(), readline_buf.end(), temp_buf_ptr);
				std::copy(buf.begin(), buf.end(), temp_buf_ptr + readline_buf.size());
				readline_buf = std::move(temp_buf);
			}
			return seastar::stop_iteration::no;
		});
	
	});
}

/*  
	Main proxy service loop
*/
seastar::future<> service_loop(){
	seastar::listen_options lo;
	lo.reuse_address = true;

	return seastar::do_with(seastar::listen(seastar::make_ipv4_address(6666), lo),
			[] (auto &listener){
		//return seastar::keep_doing([&listener] (){
			return listener.accept().then([](seastar::connected_socket s, seastar::socket_address a){
				http_proxy_conn_s *conn = new http_proxy_conn_s(std::move(s), std::move(a));
				
				return conn->handle();
			});
		//});
	});
}



int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, [] {
            std::cout << "Start service loop...\n";
            return service_loop();
    });
}
