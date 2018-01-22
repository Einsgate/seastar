#include "core/future-util.hh"
#include "core/app-template.hh"
#include "core/reactor.hh"
#include "core/distributed.hh"
#include "tests/http_proxy_alive.hh"

using namespace seastar;
using namespace std;

//send the ready request to server, get the response and process it(like reading content-legth)
future<> proxy_server_connection::process() {
	//send request
	return _client_connection._request.send_without_body(_write_buf).then([this] {
		//forward data from client to server if there is any
		if(_client_connection._request->content_length > 0) 
			return proxy_client_connection::forward_data_zero_copy(_write_buf, _client_connection._read_buf, _client_connection._request->content_length);
		else
			return make_ready_future();			
	}).then([this] {
		return _write_buf.flush();
	}).then([this] {
		return read_one();
	}).then([this] {
		bool conn_keep_alive = false;
		bool conn_close = false;

		if(_done){
			_keep_alive = false;
			return make_ready_future();
		}

		_response.content_length = proxy_client_connection::get_content_length(_response->_headers);
		if(_response.content_length < 0) {
			assert(0 && "get_content_length() failed.");
		}

		auto it = _response->_headers.find("Connection");
	    if (it != _response->_headers.end()) {
	        if (it->second == "Keep-Alive") {
	            conn_keep_alive = true;
	        } else if (it->second == "Close") {
	            conn_close = true;
	        }
	    }

		if(_response->_version == "1.0") {
			_keep_alive = conn_keep_alive;
		}
		else if(_response->_version == "1.1") {
			_keep_alive = !conn_close;
		}
		else {
			//for HTTP/0.9, do not response to client
			_keep_alive = false;
			return make_ready_future<>();
		}

		static const char *skipheaders[] = {
			//"Connection",
			//"Keep-Alive",
			"Proxy-Authenticate",
			"Proxy-Authorization",
			"Proxy-Connection"
		};

		//do some process with server headers
		return proxy_client_connection::remove_headers(_response->_headers, skipheaders, sizeof(skipheaders) / sizeof(char *)).then([this] {
			//send respond back to client
			return _response.send_without_body(_client_connection._write_buf);
		}).then([this] {
			if(_response.content_length == 0) _response.content_length = -1; //indicate the body size is not given
			return proxy_client_connection::forward_data_zero_copy(_client_connection._write_buf, _read_buf, _response.content_length);
		}).then([this] {
			return _client_connection._write_buf.flush();
		});
	}).then_wrapped([this] (auto &&f){
		if(f.failed()){
			f.ignore_ready_future();
			//std::cerr << "proxy_server_connection: process() failed.\n";
			_done = true;
			_keep_alive = false;
		}

		if(_keep_alive == false){
			log_message("proxy_server_connection: Connection shutdown.\n");
			_write_buf.close();
			_read_buf.close();
		}
		else{
			log_message("proxy_server_connection: Connection remains.\n");
		}
	});
}


void proxy_client_connection::on_new_connection() {
	_server._current_connections++;
	_server._connections.push_back(*this);
}

proxy_client_connection::~proxy_client_connection() {
	_server._current_connections--;
	_server._connections.erase(_server._connections.iterator_to(*this));
	_server.maybe_idle();
}

namespace bpo = boost::program_options;

int main(int argc, char** argv) {
	seastar::app_template app;
	app.add_options()("port, p", bpo::value<uint16_t>()->default_value(10000),
            "HTTP Proxy port");

	return app.run_deprecated(argc, argv, [&] {
		auto&& config = app.configuration();
		auto port = config["port"].as<uint16_t>();

		auto *server = new distributed<http_proxy_server>;

		server->start().then([server, port] {
			return server->invoke_on_all(&http_proxy_server::listen, ipv4_addr{port});
		}).then([server, port] {
			std::cout << "Seastar proxy server listening on port " << port << " ...\n";
            engine().at_exit([server] {
                return server->stop();
            });
		});
	});
}