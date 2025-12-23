#include "autotune_task.hpp"
#include "esp_log.h"
#include "boards/board.h"
#include "nvs_config.h"
#include "esp_timer.h"
#include "global_state.h"
#include <cmath>
#include <algorithm>
#include <cstdarg>

static const char* TAG = "AutotuneTask";

// Helper for median calculation
static float calculate_median(std::vector<float>& values) {
    if (values.empty()) return 0.0f;
    size_t n = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + n, values.end());
    return values[n];
}

AutotuneTask::AutotuneTask() {}

bool AutotuneTask::start(const AutotuneConfig& config) {
    if (isRunning()) {
        ESP_LOGW(TAG, "Autotune start failed: Task already running (Handle: %p)", m_taskHandle);
        return false;
    }

    m_config = config;
    m_stop_requested = false;
    m_state = AutotuneState::INIT;
    
    // Create FreeRTOS task
    BaseType_t res = xTaskCreate(
        taskFunction,
        "autotune_task",
        10240, // Stack size (10KB)
        this,
        10,    // Priority (higher)
        &m_taskHandle
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Autotune start failed: xTaskCreate returned %d. Heap free: %d", res, (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        m_taskHandle = nullptr; // Ensure clean state
    } else {
        log("DEBUG: Task created with handle %p", m_taskHandle);
        ESP_LOGI(TAG, "Autotune task created successfully");
    }

    return res == pdPASS;
}

void AutotuneTask::stop() {
    m_stop_requested = true;
    log("Stop requested by user");
}

void AutotuneTask::taskFunction(void* pvParameters) {
    AutotuneTask* task = static_cast<AutotuneTask*>(pvParameters);
    task->run();
    
    task->m_state = AutotuneState::IDLE;
    task->m_taskHandle = nullptr;
    vTaskDelete(NULL);
}

void AutotuneTask::run() {
    // Immediate UI log to confirm entry
    log("DEBUG: Task run() entered. Stack OK.");

    Board* board = SYSTEM_MODULE.getBoard();
    
    if (!board) {
        log("FATAL: Board instance is NULL!");
        m_state = AutotuneState::ERROR_STOPPED;
        return;
    }
    
    // Log board info to confirm access
    log("DEBUG: Board found. Model: %s", board->getDeviceModel());


    // Resolve Auto-Config (0 values) and Clamp to Board Limits
    uint16_t boardMaxFreq = board->getAbsMaxAsicFrequency();
    if (m_config.max_frequency == 0 || m_config.max_frequency > boardMaxFreq) 
        m_config.max_frequency = boardMaxFreq;

    uint16_t boardMaxVolt = board->getAbsMaxAsicVoltageMillis();
    if (m_config.max_voltage == 0 || m_config.max_voltage > boardMaxVolt) 
        m_config.max_voltage = boardMaxVolt;

    uint16_t boardMaxPower = (uint16_t)board->getMaxPin();
    if (m_config.max_power == 0 || m_config.max_power > boardMaxPower) 
        m_config.max_power = boardMaxPower;

    uint16_t userMaxTemp = Config::getOverheatTemp();
    if (m_config.max_temp == 0 || m_config.max_temp > userMaxTemp) 
        m_config.max_temp = userMaxTemp;

    log("Autotune started. Max Freq: %d MHz, Max Volt: %d mV, Max Power: %d W, Max Temp: %d C", 
        m_config.max_frequency, m_config.max_voltage, m_config.max_power, m_config.max_temp);

    // Baseline / Start values
    m_current_freq = board->getAsicFrequency();
    m_current_volt = (uint16_t)(board->getAsicVoltageMillis());
    m_start_volt = m_current_volt; // Carry this forward

    // If starting freq is too low compared to config, maybe bump it?
    // For now, respect current settings as start point.

    // Scanning variables
    uint16_t scan_freq_start = m_current_freq;
    uint16_t scan_freq_end = m_config.max_frequency;
    uint16_t freq_step = 10; // 10 MHz steps
    uint16_t volt_step = 10; // 10 mV steps

    m_total_estimated_steps = ((scan_freq_end - scan_freq_start) / freq_step) + 1;
    m_step_counter = 0;

    std::vector<float> samples_hr;
    std::vector<float> samples_pwr;
    uint64_t state_start_time = esp_timer_get_time() / 1000;
    
    // Voltage sweep context
    std::vector<int> volt_offsets = {0, -1, 1, -2, 2}; // Checking center first, then neighbors
    size_t volt_idx = 0;
    AutotuneResult best_this_freq;

    m_state = AutotuneState::SCAN_FREQ_INIT;

    while (!m_stop_requested) {
        // Run state machine
        switch (m_state) {
            case AutotuneState::SCAN_FREQ_INIT: {
                if (m_current_freq > scan_freq_end) {
                    m_state = AutotuneState::FINISHED;
                    break;
                }
                
                log("=== Scanning Frequency: %d MHz ===", m_current_freq);
                m_step_counter++;
                
                // Reset voltage sweep
                volt_idx = 0;
                best_this_freq = {}; // Invalid
                
                m_state = AutotuneState::SCAN_FREQ_VOLT_SWEEP;
                break;
            }

            case AutotuneState::SCAN_FREQ_VOLT_SWEEP: {
                if (volt_idx >= volt_offsets.size()) {
                    // Done with this frequency
                    if (best_this_freq.valid) {
                        // We found a stable point
                        m_start_volt = best_this_freq.voltage; // Carry forward closest good voltage
                        log("Freq %d MHz best voltage: %d mV (HR: %.1f GH/s)", m_current_freq, m_start_volt, best_this_freq.hashrate_ghs);
                        
                        // Check Global Bests
                        if (best_this_freq.hashrate_ghs > m_best_hashrate.hashrate_ghs) {
                            m_best_hashrate = best_this_freq;
                            log("NEW BEST HASHRATE: %.1f GH/s @ %d MHz / %d mV", m_best_hashrate.hashrate_ghs, m_current_freq, best_this_freq.voltage);
                        }
                        
                        // Check Efficiency (Lower J/TH is better, so > 0 check)
                        if (m_best_efficiency.efficiency_j_th == 0 || best_this_freq.efficiency_j_th < m_best_efficiency.efficiency_j_th) {
                             m_best_efficiency = best_this_freq;
                             log("NEW BEST EFFICIENCY: %.2f J/TH @ %d MHz / %d mV", m_best_efficiency.efficiency_j_th, m_current_freq, best_this_freq.voltage);
                        }

                        // Move to next freq
                        m_current_freq += freq_step;
                        m_state = AutotuneState::SCAN_FREQ_INIT;
                    } else {
                        log("Freq %d MHz: no stable voltage found", m_current_freq);
                        // Abort or Skip? Python script stops after 2 bad streaks.
                        // For simplicity here, we assume if we cant stabilize at this freq, higher freq wont work either.
                        // But let's try next freq just in case (maybe local issue).
                        m_current_freq += freq_step; 
                        m_state = AutotuneState::SCAN_FREQ_INIT; 
                    }
                    break;
                }

                // Determine target voltage
                int offset = volt_offsets[volt_idx];
                m_current_volt = m_start_volt + (offset * volt_step);
                
                // Clamp voltage
                if (m_current_volt > m_config.max_voltage) m_current_volt = m_config.max_voltage;
                if (m_current_volt < 1100) m_current_volt = 1100; // Hard min limit

                // Apply
                if (applySettings(m_current_freq, m_current_volt)) {
                    log("Testing: %d MHz @ %d mV", m_current_freq, m_current_volt);
                    state_start_time = esp_timer_get_time() / 1000;
                    m_state = AutotuneState::WAIT_STABLE;
                } else {
                    log("Failed to set %d MHz @ %d mV", m_current_freq, m_current_volt);
                    volt_idx++; // Try next
                }
                break;
            }

            case AutotuneState::WAIT_STABLE: {
                // Check limits continuously
                if (!checkLimits()) {
                    log("Safety limit exceeded during settling. Skipping this setting.");
                    volt_idx++;
                    m_state = AutotuneState::SCAN_FREQ_VOLT_SWEEP;
                    break;
                }

                uint64_t now = esp_timer_get_time() / 1000;
                if ((now - state_start_time) >= SETTLE_TIME_MS) {
                    state_start_time = now;
                    samples_hr.clear();
                    samples_pwr.clear();
                    m_state = AutotuneState::MEASURE;
                }
                break;
            }

            case AutotuneState::MEASURE: {
                 if (!checkLimits()) {
                    log("Safety limit exceeded during measurement.");
                    volt_idx++;
                    m_state = AutotuneState::SCAN_FREQ_VOLT_SWEEP;
                    break;
                }

                // Collect samples
                float hr = HASHRATE_MONITOR.getSmoothedTotalChipHashrate();
                float pwr = board->getPin();
                
                if (hr > 0) samples_hr.push_back(hr);
                if (pwr > 0) samples_pwr.push_back(pwr);

                uint64_t now = esp_timer_get_time() / 1000;
                if ((now - state_start_time) >= MEASURE_TIME_MS) {
                    m_state = AutotuneState::ANALYZE;
                }
                break;
            }

            case AutotuneState::ANALYZE: {
                // Calculate stats
                float median_hr = calculate_median(samples_hr);
                float avg_pwr = 0;
                for (float p : samples_pwr) avg_pwr += p;
                if (!samples_pwr.empty()) avg_pwr /= samples_pwr.size();

                float temp = board->getVRTemp();
                for (int i=0; i<board->getNumTempSensors(); i++) {
                     float t = board->getChipTemp(i);
                     if (!std::isnan(t) && t > temp) temp = t;
                }

                bool valid = true;
                if (median_hr < 1.0f) valid = false; // Dead
                if (avg_pwr > m_config.max_power) valid = false;
                if (temp > m_config.max_temp) valid = false;

                if (valid) {
                    float efficiency = (median_hr > 0) ? (avg_pwr / (median_hr / 1000.0f)) : 999.0f; // J/TH (W / TH/s)

                    log("Result: VALID | HR: %.1f GH/s | Pwr: %.1f W | Eff: %.2f J/TH", median_hr, avg_pwr, efficiency);
                    
                    // Is this better for this frequency?
                    // Typically 'better' means stable. We prefer lower voltage for same freq if stable.
                    // Since we sweep specific pattern, we can just take the first valid one or the one with best eff.
                    // Let's take best efficiency for this freq.
                    if (!best_this_freq.valid || efficiency < best_this_freq.efficiency_j_th) {
                         best_this_freq = {
                            m_current_freq, m_current_volt, median_hr, avg_pwr, efficiency, temp, true
                         };
                    }
                } else {
                    log("Result: INVALID (pwr/temp/hr)");
                }

                volt_idx++;
                m_state = AutotuneState::SCAN_FREQ_VOLT_SWEEP;
                break;
            }

            case AutotuneState::FINISHED:
                log("Autotune Finished.");
                if (m_best_efficiency.valid) {
                     log("Applying Best Efficiency: %d MHz @ %d mV", m_best_efficiency.frequency, m_best_efficiency.voltage);
                     applySettings(m_best_efficiency.frequency, m_best_efficiency.voltage);
                }
                m_stop_requested = true;
                break;
                
            case AutotuneState::ERROR_STOPPED:
                m_stop_requested = true;
                break;
                
            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLLING_INTERVAL_MS));
    }
    
    log("Autotune task ending.");
}

bool AutotuneTask::applySettings(uint16_t freq, uint16_t volt) {
    Board* board = SYSTEM_MODULE.getBoard();
    if (!board) return false;
    
    // Safety check again
    if (volt > m_config.max_voltage) volt = m_config.max_voltage;
    
    bool v_ok = board->setVoltage((float)volt / 1000.0f);
    bool f_ok = board->setAsicFrequency((float)freq);
    
    return v_ok && f_ok;
}

bool AutotuneTask::checkLimits() {
    Board* board = SYSTEM_MODULE.getBoard();
    if (!board) return false;
    
    float pwr = board->getPin();
    
    if (pwr > m_config.max_power) {
        log("SAFETY: Max Power %.1f W exceeded limit %d W", pwr, m_config.max_power);
        return false;
    }
    
    float max_t = board->getVRTemp();
    for (int i=0; i<board->getNumTempSensors(); i++) {
            float t = board->getChipTemp(i);
            if (!std::isnan(t) && t > max_t) max_t = t;
    }
    if (max_t > m_config.max_temp) {
        log("SAFETY: Max Temp %.1f C exceeded limit %d C", max_t, m_config.max_temp);
        return false;
    }
    
    return true;
}

void AutotuneTask::log(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    

    
    std::lock_guard<std::mutex> lock(m_log_mutex);
    m_log_buffer.push_back(std::string(buffer));
    // Keep last 50 lines
    if (m_log_buffer.size() > 50) {
        m_log_buffer.erase(m_log_buffer.begin());
    }
}

std::vector<std::string> AutotuneTask::getLogLines() const {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    return m_log_buffer;
}
