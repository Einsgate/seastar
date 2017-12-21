#include "core/seastar.hh"
#include "core/future-util.hh"
#include "core/app-template.hh"
#include "core/reactor.hh"
#include <iostream>

using namespace seastar;

int i = 0;

future<> test_exception(){

	return repeat([]{
		std::cout << "i = " << i << std::endl;
		if(i++ == 10) throw std::length_error("");
		if(i++ == 20) return seastar::stop_iteration::yes;
		return seastar::stop_iteration::no;
	});
}

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, [] {
            std::cout << "Start testing...\n";
            return test_exception().then_wrapped([](auto &&f){
            	if(f.failed())
            		std::cout << "failed!\n";
            	return std::move(f);
            });
    });
}
