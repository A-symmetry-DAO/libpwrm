#include "reporting.h"

using json = nlohmann::json;


void report(const string* base_file,  std::vector<string> devices,  std::map<string, double> measurements, const string* ceramic_url, const string* did)
{
    info("Loading base reporting data file {}...", *base_file);
    std::ifstream f(*base_file);
    string str = "";
    if(f) {
      ostringstream ss;
      ss << f.rdbuf(); 
      f.close();
      str = ss.str();
    }
    else {
      error("Could not open file {} for reading.", *base_file);
      return;
    }
    json data = json::parse(str);

    
    time_t now = time(0);
    string dt = ctime(&now);
    data["date"] = dt;
    data["devices"] = devices;
    data["measurements"] = measurements;
    std::string header = R"("header": { "family": "test"})";
    std::string body = R"(
    {
      "type": 0,
      "genesis": {
        "header": {
          "family": "test"
        },
        "pwr": 
            )" + data.dump() + "} }";
    
    data = json::parse(body); 
    string k = "did:key:" + (*did);
    string controllers[] = {k};
    data["genesis"]["header"]["controllers"] = controllers;
    debug("JSON data document:{}", data.dump());

    // HTTPS
    httplib::Client client(*ceramic_url);
    if (auto res = client.Post("/api/v0/streams", data.dump(), "application/json")) {
      if (res->status == 200) {
        std::cout << res->body << std::endl;
      }
    } else {
      auto err = res.error();
      std::cout << "HTTP error: " << httplib::to_string(err) << std::endl;
  }
    //r.
    //json data = R"(
  //{
  //  "location": {
  //      "lat": 0.0,
  //      "long": 0.0
  //  },
  //  "devices":[]
  //}
//)"_json;
//f.close();

}