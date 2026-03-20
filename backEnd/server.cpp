#include "httplib.h"
#include "nlohmann/json.hpp"
#include "threadpool.hpp"
#include "classes.hpp"

using json = nlohmann::json;

ThreadPool pool;

void handleRequest(const httplib::Request& req, httplib::Response& res) {
    // TODO: parse req.body, process, and set res.body
    res.set_content("OK", "text/plain");
}

int main() {
    httplib::Server svr;

    svr.Post("/api/example", [](const httplib::Request& req, httplib::Response& res) {
        pool.enqueue([&req, &res] {
            handleRequest(req, res);
        });
    });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("healthy", "text/plain");
    });

    svr.listen("0.0.0.0", 8080);
    return 0;
}
