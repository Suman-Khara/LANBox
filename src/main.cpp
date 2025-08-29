#include <bits/stdc++.h>
#include "json.hpp"

using namespace std;
using json = nlohmann::json;

const string CONFIG_FILE = "../data/lanbox.json";

void createDefaultConfig() {
    json config;
    config["device_id"] = "device-" + to_string(rand() % 10000);
    config["shared_folder"] = "./LANBoxFolder";
    config["last_updated"] = "2025-08-25T10:00:00";
    config["files"] = json::array();

    ofstream file(CONFIG_FILE);
    file << config.dump(4);
    file.close();

    cout << "Created new config at " << CONFIG_FILE << "\n";
}

void loadConfig() {
    ifstream file(CONFIG_FILE);
    if (!file.is_open()) {
        cout << "No config found. Creating default...\n";
        createDefaultConfig();
        return;
    }

    json config;
    file >> config;

    cout << "Loaded config:\n";
    cout << config.dump(4) << "\n";
}

int main() {
    cout << "LANBox CLI started!\n";

    loadConfig();

    return 0;
}
