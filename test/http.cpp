#include "config.h"
#include "common.hpp"
#include "fetcher_io.hpp"
#include "tilejson.hpp"
#include "fetch/http.hpp"
#include "logging/logger.hpp"
#include "http_server/server.hpp"
#include "http_server/mapnik_handler_factory.hpp"
#include "vector_tile.pb.h"

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/make_shared.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>

#include <mapnik/datasource_cache.hpp>

#include <iostream>

#include <curl/curl.h>

namespace bpt = boost::property_tree;
using http::server3::request;
using http::server3::reply;
using http::server3::server_options;
using http::server3::handler_factory;
using http::server3::request_handler;
using http::server3::mapnik_server_options;
using http::server3::mapnik_handler_factory;

namespace {

server_options default_options(mapnik_server_options &map_opts) {
  server_options options;
  options.thread_hint = 1;
  options.port = "";
  options.factory.reset(new mapnik_handler_factory(map_opts));
  return options;
}

mapnik_server_options default_mapnik_options(const std::string &map_file, int compression_level) {
  mapnik_server_options options;
  options.path_multiplier = 16;
  options.buffer_size = 0;
  options.scale_factor = 1.0;
  options.offset_x = 0;
  options.offset_y = 0;
  options.tolerance = 1;
  options.image_format = "jpeg";
  options.scaling_method = mapnik::SCALING_NEAR;
  options.scale_denominator = 0.0;
  options.map_file = map_file;
  options.max_age = 60;
  options.compression_level = compression_level;
  return options;
}

struct server_guard {
  mapnik_server_options map_opt;
  server_options srv_opt;
  http::server3::server server;
  std::string port;

  server_guard(const std::string &map_xml, int compression_level = -1)
    : map_opt(default_mapnik_options(map_xml, compression_level))
    , srv_opt(default_options(map_opt))
    , server("localhost", srv_opt)
    , port(server.port()) {

    server.run(false);
  }

  ~server_guard() {
    server.stop();
  }

  std::string base_url() {
    return (boost::format("http://localhost:%1%") % port).str();
  }
};

// variant of the server guard which takes a custom handler factory
struct server_guard2 {
  boost::shared_ptr<handler_factory> factory;
  server_options srv_opt;
  http::server3::server server;
  std::string port;

  server_guard2(boost::shared_ptr<handler_factory> f)
    : factory(f)
    , srv_opt(mk_options(factory))
    , server("localhost", srv_opt)
    , port(server.port()) {

    server.run(false);
  }

  ~server_guard2() {
    server.stop();
  }

  std::string base_url() {
    return (boost::format("http://localhost:%1%") % port).str();
  }

  static server_options mk_options(boost::shared_ptr<handler_factory> f) {
    server_options opts;
    opts.thread_hint = 1;
    opts.port = "";
    opts.factory = f;
    return opts;
  }
};

void test_fetch_empty() {
  server_guard guard("test/empty_map_file.xml");

  avecado::fetch::http fetch(guard.base_url(), "pbf");
  avecado::fetch_response response(fetch(avecado::request(0, 0, 0)).get());

  test::assert_equal<bool>(response.is_left(), true, "should fetch tile OK");
  test::assert_equal<int>(response.left()->mapnik_tile().layers_size(), 0, "should have no layers");
}

void test_fetch_single_line() {
  server_guard guard("test/single_line.xml");

  avecado::fetch::http fetch(guard.base_url(), "pbf");
  avecado::fetch_response response(fetch(avecado::request(0, 0, 0)).get());

  test::assert_equal<bool>(response.is_left(), true, "should fetch tile OK");
  test::assert_equal<int>(response.left()->mapnik_tile().layers_size(), 1, "should have one layer");
}

void assert_is_error(avecado::fetch::http &fetch, int z, int x, int y, avecado::fetch_status status) {
  avecado::fetch_response response(fetch(avecado::request(z, x, y)).get());
  test::assert_equal<bool>(response.is_right(), true, (boost::format("(%1%, %2%, %3%): response should be failure") % z % x % y).str());
  test::assert_equal<avecado::fetch_status>(response.right().status, status,
                                            (boost::format("(%1%, %2%, %3%): response status is not what was expected") % z % x % y).str());
}

void test_fetch_error_coordinates() {
  using avecado::fetch_status;

  server_guard guard("test/empty_map_file.xml");

  avecado::fetch::http fetch(guard.base_url(), "pbf");

  assert_is_error(fetch, -1, 0, 0, fetch_status::not_found);
  assert_is_error(fetch, 31, 0, 0, fetch_status::not_found);
  assert_is_error(fetch, 0, 0, 1, fetch_status::not_found);
  assert_is_error(fetch, 0, 1, 0, fetch_status::not_found);
  assert_is_error(fetch, 0, 0, -1, fetch_status::not_found);
  assert_is_error(fetch, 0, -1, 0, fetch_status::not_found);
}
 
void test_fetch_error_extension() {
  using avecado::fetch_status;

  server_guard guard("test/empty_map_file.xml");

  avecado::fetch::http fetch(guard.base_url(), "gif");

  assert_is_error(fetch, 0, 0, 0, fetch_status::not_found);
}

void test_fetch_error_path_segments() {
  using avecado::fetch_status;

  server_guard guard("test/empty_map_file.xml");

  avecado::fetch::http fetch(guard.base_url(), "/0.pbf");

  assert_is_error(fetch, 0, 0, 0, fetch_status::not_found);
}

void test_fetch_error_non_numeric() {
  using avecado::fetch_status;

  server_guard guard("test/empty_map_file.xml");

  std::vector<std::string> patterns;
  patterns.push_back((boost::format("%1%/a/b/c.pbf") % guard.base_url()).str());
  avecado::fetch::http fetch(std::move(patterns));

  assert_is_error(fetch, 0, 0, 0, fetch_status::not_found);
}

void test_no_url_patterns_is_error() {
  std::vector<std::string> patterns;
  bool threw = false;

  avecado::fetch::http fetch(std::move(patterns));

  try {
    avecado::fetch_response response(fetch(avecado::request(0, 0, 0)).get());
  } catch (...) {
    threw = true;
  }

  test::assert_equal<bool>(threw, true, "Should have thrown exception when patterns was empty.");
}

void test_fetcher_io() {
  using avecado::fetch_status;

  test::assert_equal<std::string>((boost::format("%1%") % fetch_status::not_modified).str(), "Not Modified");
  test::assert_equal<std::string>((boost::format("%1%") % fetch_status::bad_request).str(), "Bad Request");
  test::assert_equal<std::string>((boost::format("%1%") % fetch_status::not_found).str(), "Not Found");
  test::assert_equal<std::string>((boost::format("%1%") % fetch_status::server_error).str(), "Server Error");
  test::assert_equal<std::string>((boost::format("%1%") % fetch_status::not_implemented).str(), "Not Implemented");
}

void test_fetch_tilejson() {
  using avecado::fetch_status;

  server_guard guard("test/single_poly.xml");

  bpt::ptree tilejson = avecado::tilejson(
    (boost::format("%1%/tile.json") % guard.base_url()).str());
}

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  std::stringstream *stream = static_cast<std::stringstream*>(userdata);
  size_t total_bytes = size * nmemb;
  stream->write(ptr, total_bytes);
  return stream->good() ? total_bytes : 0;
}

#define CURL_SETOPT(curl, opt, arg) {                                   \
    CURLcode res = curl_easy_setopt((curl), (opt), (arg));              \
    if (res != CURLE_OK) {                                              \
      throw std::runtime_error("Unable to set cURL option " #opt);      \
    }                                                                   \
  }

void test_tile_is_compressed() {
  server_guard guard("test/single_line.xml", 9);
  std::string uri = (boost::format("%1%/0/0/0.pbf") % guard.base_url()).str();
  std::stringstream stream;

  CURL *curl = curl_easy_init();
  CURL_SETOPT(curl, CURLOPT_URL, uri.c_str());
  CURL_SETOPT(curl, CURLOPT_WRITEFUNCTION, write_callback);
  CURL_SETOPT(curl, CURLOPT_WRITEDATA, &stream);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    throw std::runtime_error("cURL operation failed");
  }

  curl_easy_cleanup(curl);

  std::string data = stream.str();
  test::assert_greater_or_equal<size_t>(data.size(), 2, "tile size");
  // see https://tools.ietf.org/html/rfc1952#page-6 for header magic values
  test::assert_equal<uint32_t>(uint8_t(data[0]), 0x1f, "gzip header magic ID1");
  test::assert_equal<uint32_t>(uint8_t(data[1]), 0x8b, "gzip header magic ID2");
  test::assert_equal<uint32_t>(uint8_t(data[2]), 0x08, "gzip compression method = deflate");
}

void test_tile_is_not_compressed() {
  // check that when the compression level is set to zero, the tile is not
  // compressed and is just the raw PBF.
  server_guard guard("test/single_line.xml", 0);
  std::string uri = (boost::format("%1%/0/0/0.pbf") % guard.base_url()).str();
  std::stringstream stream;

  CURL *curl = curl_easy_init();
  CURL_SETOPT(curl, CURLOPT_URL, uri.c_str());
  CURL_SETOPT(curl, CURLOPT_WRITEFUNCTION, write_callback);
  CURL_SETOPT(curl, CURLOPT_WRITEDATA, &stream);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    throw std::runtime_error("cURL operation failed");
  }

  curl_easy_cleanup(curl);

  // note: this deliberately doesn't use the functions defined on avecado::tile
  // because it needs to avoid any automatic ungzipping.
  stream.seekp(0);
  google::protobuf::io::IstreamInputStream gstream(&stream);
  vector_tile::Tile tile;
  bool read_ok = tile.ParseFromZeroCopyStream(&gstream);
  test::assert_equal<bool>(read_ok, true, "tile was plain PBF");
}

struct cache_header_checker_handler : public request_handler {
  virtual ~cache_header_checker_handler() {}

  // returns 304 if the ETag header is present, otherwise a 500. this
  // is used to check that the HTTP system is correctly sending the
  // cache headers.
  virtual void handle_request(const request &req, reply &rep) {
    rep = reply::stock_reply(reply::internal_server_error);
    for (const auto &header : req.headers) {
      if (boost::iequals(header.name, "If-None-Match") && (header.value == "\"foo\"")) {
        rep = reply::stock_reply(reply::not_modified);
        break;
      } else if (boost::iequals(header.name, "If-Modified-Since") && (header.value == "Wed, 13 May 2015 14:35:10 GMT")) {
        rep = reply::stock_reply(reply::not_modified);
        break;
      }
    }
  }
};

struct cache_header_checker_factory : public handler_factory {
  virtual ~cache_header_checker_factory() {}
  virtual void thread_setup(boost::thread_specific_ptr<request_handler> &tss, const std::string &) {
    tss.reset(new cache_header_checker_handler);
  }
};

void test_http_etag() {
  auto factory = boost::make_shared<cache_header_checker_factory>();
  server_guard2 server(factory);

  avecado::fetch::http fetch(server.base_url(), "png");

  auto req = avecado::request(0, 0, 0);
  req.etag = "foo";
  avecado::fetch_response response(fetch(req).get());
  if (response.is_left()) {
    throw std::runtime_error("Expected 304 when using ETag header, but got 200 OK");
  }
  if (response.right().status != avecado::fetch_status::not_modified) {
    throw std::runtime_error((boost::format("Expected status 304 when using ETag header, but got %1% %2%")
                              % int(response.right().status) % response.right()).str());
  }
}

void test_http_if_modified_since() {
  auto factory = boost::make_shared<cache_header_checker_factory>();
  server_guard2 server(factory);

  avecado::fetch::http fetch(server.base_url(), "png");

  auto req = avecado::request(0, 0, 0);
  req.if_modified_since = boost::posix_time::ptime(boost::gregorian::date(2015, boost::gregorian::May, 13),
                                                   boost::posix_time::time_duration(14, 35, 10));
  avecado::fetch_response response(fetch(req).get());
  if (response.is_left()) {
    throw std::runtime_error("Expected 304 when using If-Modified-Since header, but got 200 OK");
  }
  if (response.right().status != avecado::fetch_status::not_modified) {
    throw std::runtime_error((boost::format("Expected status 304 when using If-Modified-Since header, but got %1% %2%")
                              % int(response.right().status) % response.right()).str());
  }
}

} // anonymous namespace

int main() {
  int tests_failed = 0;

  std::cout << "== Testing HTTP fetching ==" << std::endl << std::endl;

  // need datasource cache set up so that input plugins are available
  // when we parse map XML.
  mapnik::datasource_cache::instance().register_datasources(MAPNIK_DEFAULT_INPUT_PLUGIN_DIR);

#define RUN_TEST(x) { tests_failed += test::run(#x, &(x)); }
  RUN_TEST(test_fetch_empty);
  RUN_TEST(test_fetch_single_line);
  RUN_TEST(test_fetch_error_coordinates);
  RUN_TEST(test_fetch_error_extension);
  RUN_TEST(test_fetch_error_path_segments);
  RUN_TEST(test_fetch_error_non_numeric);
  RUN_TEST(test_no_url_patterns_is_error);
  RUN_TEST(test_fetcher_io);
  RUN_TEST(test_fetch_tilejson);
  RUN_TEST(test_tile_is_compressed);
  RUN_TEST(test_tile_is_not_compressed);
  RUN_TEST(test_http_etag);
  RUN_TEST(test_http_if_modified_since);

  std::cout << " >> Tests failed: " << tests_failed << std::endl << std::endl;

  return (tests_failed > 0) ? 1 : 0;
}
