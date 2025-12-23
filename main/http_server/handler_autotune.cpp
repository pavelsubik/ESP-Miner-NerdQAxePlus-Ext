#include "handler_autotune.h"
#include "autotune_task.hpp"
#include "http_utils.h"
#include "http_cors.h"
#include "esp_log.h"
#include "cJSON.h"
#include "ArduinoJson.h"
#include "psram_allocator.h"

static const char *TAG = "handler_autotune";

esp_err_t POST_autotune_start(httpd_req_t *req)
{
    ConGuard g(NULL, req);



    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    PSRAMAllocator allocator;
    JsonDocument doc(&allocator);
    
    esp_err_t err = getJsonData(req, doc);
    if (err != ESP_OK) {

        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    AutotuneConfig config = {};
    
    // Parse parameters
    if (doc["maxFrequency"].is<uint16_t>()) config.max_frequency = doc["maxFrequency"];
    if (doc["maxVoltage"].is<uint16_t>()) config.max_voltage = doc["maxVoltage"];
    if (doc["maxPower"].is<uint16_t>()) config.max_power = doc["maxPower"];
    if (doc["maxVrTemp"].is<uint16_t>()) config.max_temp = doc["maxVrTemp"];


    
    // Start Task
    bool started = AutotuneTask::getInstance().start(config);

    if (started) {

        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    } else {

        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start (already running?)");
    }
}

esp_err_t POST_autotune_stop(httpd_req_t *req)
{
    ConGuard g(NULL, req);
    if (is_network_allowed(req) != ESP_OK) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");

    AutotuneTask::getInstance().stop();
    httpd_resp_send(req, "{\"status\":\"stopping\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t GET_autotune_status(httpd_req_t *req)
{
    ConGuard g(NULL, req);
    if (is_network_allowed(req) != ESP_OK) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");

    AutotuneTask& task = AutotuneTask::getInstance();
    
    // Using cJSON for response generation (maintained from original design)
    cJSON *root = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(root, "isRunning", task.isRunning());
    cJSON_AddNumberToObject(root, "currentStep", task.getCurrentStep());
    cJSON_AddNumberToObject(root, "totalSteps", task.getTotalSteps());
    
    cJSON_AddNumberToObject(root, "currentFrequency", task.getCurrentFreq());
    cJSON_AddNumberToObject(root, "currentVoltage", task.getCurrentVolt());
    
    // Best Hashrate
    AutotuneResult bestHR = task.getBestHashrate();
    cJSON_AddNumberToObject(root, "bestHashrate", bestHR.hashrate_ghs); // matches frontend
    cJSON_AddNumberToObject(root, "bestFrequency", bestHR.frequency);
    cJSON_AddNumberToObject(root, "bestVoltage", bestHR.voltage);
    cJSON_AddNumberToObject(root, "bestHashrateEff", bestHR.efficiency_j_th);
    
    // Best Efficiency
    AutotuneResult bestEff = task.getBestEfficiency();
    cJSON_AddNumberToObject(root, "bestEfficiency", bestEff.efficiency_j_th); // matches frontend
    cJSON_AddNumberToObject(root, "bestEffFrequency", bestEff.frequency);
    cJSON_AddNumberToObject(root, "bestEffVoltage", bestEff.voltage);
    cJSON_AddNumberToObject(root, "bestEfficiencyHR", bestEff.hashrate_ghs);
    
    // Logs (as array of strings)
    cJSON *logs = cJSON_CreateArray();
    std::vector<std::string> logLines = task.getLogLines();
    for(const auto& line : logLines) {
        cJSON_AddItemToArray(logs, cJSON_CreateString(line.c_str()));
    }
    cJSON_AddItemToObject(root, "logs", logs);

    const char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}
