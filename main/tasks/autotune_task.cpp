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
    
    // Clear previous logs
    {
        std::lock_guard<std::mutex> lock(m_log_mutex);
        m_log_buffer.clear();
    }
    
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
    m_carry_volt = m_current_volt; // Initialize carry voltage
    m_bad_streak = 0; // Reset bad streak counter
    m_all_results.clear(); // Clear result cache

    // Scanning parameters
    uint16_t scan_freq_start = m_current_freq;
    uint16_t scan_freq_end = m_config.max_frequency;
    uint16_t freq_step = 10; // 10 MHz steps (coarse)
    uint16_t volt_step = 10; // 10 mV steps (coarse)

    // Build frequency list
    std::vector<uint16_t> frequencies;
    for (uint16_t f = scan_freq_start; f <= scan_freq_end; f += freq_step) {
        frequencies.push_back(f);
    }
    
    m_total_estimated_steps = frequencies.size();
    m_step_counter = 0;

    log("Starting coarse sweep: %d frequencies from %d to %d MHz", 
        frequencies.size(), scan_freq_start, scan_freq_end);

    // COARSE SWEEP: Frequency-first with voltage optimization
    AutotuneResult global_best;
    global_best.valid = false;

    for (uint16_t freq : frequencies) {
        if (m_stop_requested) break;
        
        m_step_counter++;
        m_current_freq = freq;
        
        log("");
        log("===== COARSE F=%d MHz (step %d/%d) =====", freq, m_step_counter, m_total_estimated_steps);
        
        // Optimize voltage for this frequency
        AutotuneResult best_at_freq = optimizeVoltageForFrequency(freq, m_carry_volt, volt_step);
        
        if (!best_at_freq.valid) {
            log("No stable point at F=%d MHz", freq);
            m_bad_streak++;
            if (m_bad_streak >= BAD_STREAK_STOP) {
                log("Early stop: %d consecutive bad frequencies", m_bad_streak);
                break;
            }
            continue;
        }
        
        // Carry forward the voltage for next frequency
        m_carry_volt = best_at_freq.voltage;
        
        // Check if this is a new global best
        if (!global_best.valid || best_at_freq.hashrate_ghs > global_best.hashrate_ghs) {
            global_best = best_at_freq;
            m_bad_streak = 0; // Reset streak on improvement
            log("NEW GLOBAL BEST: F=%d V=%d HR=%.1f GH/s Eff=%.2f J/TH", 
                global_best.frequency, global_best.voltage, 
                global_best.hashrate_ghs, global_best.efficiency_j_th);
            
            // Update best hashrate immediately
            m_best_hashrate = global_best;
        } else {
            float drop = global_best.hashrate_ghs - best_at_freq.hashrate_ghs;
            log("Best@F=%d: V=%d HR=%.1f (drop vs global: %.1f GH/s)", 
                freq, best_at_freq.voltage, best_at_freq.hashrate_ghs, drop);
            
            if (drop > DROP_STOP_THRESHOLD) {
                m_bad_streak++;
            } else {
                m_bad_streak = 0;
            }
            
            if (m_bad_streak >= BAD_STREAK_STOP) {
                log("Early stop: hashrate dropped by %.1f GH/s for %d frequencies", 
                    drop, m_bad_streak);
                break;
            }
        }
        
        // Also track best efficiency
        if (!m_best_efficiency.valid || best_at_freq.efficiency_j_th < m_best_efficiency.efficiency_j_th) {
            m_best_efficiency = best_at_freq;
            log("NEW BEST EFFICIENCY: F=%d V=%d Eff=%.2f J/TH HR=%.1f GH/s", 
                m_best_efficiency.frequency, m_best_efficiency.voltage,
                m_best_efficiency.efficiency_j_th, m_best_efficiency.hashrate_ghs);
        }
    }

    // FINISHED
    if (!global_best.valid) {
        log("");
        log("No stable candidate found. Autotune failed.");
        m_state = AutotuneState::ERROR_STOPPED;
        return;
    }

    log("");
    log("=== COARSE BEST: F=%d V=%d HR=%.1f GH/s Eff=%.2f J/TH ===", 
        global_best.frequency, global_best.voltage, 
        global_best.hashrate_ghs, global_best.efficiency_j_th);

    // CONFIRMATION WITH FALLBACK
    log("");
    log("=== CONFIRMATION PHASE ===");
    
    // Build candidate list: best efficiency first, then all valid results sorted by hashrate
    std::vector<AutotuneResult> candidates;
    candidates.push_back(m_best_efficiency);
    
    // Add all other valid results sorted by hashrate (descending)
    std::vector<AutotuneResult> sorted_results = m_all_results;
    std::sort(sorted_results.begin(), sorted_results.end(),
              [](const AutotuneResult& a, const AutotuneResult& b) {
                  return a.hashrate_ghs > b.hashrate_ghs;
              });
    
    for (const auto& res : sorted_results) {
        // Skip if already in candidates (avoid duplicates)
        bool already_added = false;
        for (const auto& c : candidates) {
            if (c.frequency == res.frequency && c.voltage == res.voltage) {
                already_added = true;
                break;
            }
        }
        if (!already_added) {
            candidates.push_back(res);
        }
    }
    
    log("Testing %d candidates for confirmation...", candidates.size());
    
    AutotuneResult confirmed_best;
    confirmed_best.valid = false;
    
    for (size_t i = 0; i < candidates.size() && i < 5; i++) { // Test max 5 candidates
        if (m_stop_requested) break;
        
        const auto& candidate = candidates[i];
        log("Candidate %d: F=%d V=%d HR=%.1f Eff=%.2f", 
            i+1, candidate.frequency, candidate.voltage, 
            candidate.hashrate_ghs, candidate.efficiency_j_th);
        
        if (confirmCandidate(candidate)) {
            confirmed_best = candidate;
            log("CONFIRMED: F=%d V=%d", confirmed_best.frequency, confirmed_best.voltage);
            break;
        }
    }
    
    if (!confirmed_best.valid) {
        log("");
        log("WARNING: No candidate passed confirmation. Using best efficiency without confirmation.");
        confirmed_best = m_best_efficiency;
    }

    // Apply confirmed result
    log("");
    log("Applying Final Result: F=%d MHz @ %d mV (HR=%.1f GH/s, Eff=%.2f J/TH)", 
        confirmed_best.frequency, confirmed_best.voltage,
        confirmed_best.hashrate_ghs, confirmed_best.efficiency_j_th);
    applySettings(confirmed_best.frequency, confirmed_best.voltage);
    
    log("Autotune completed successfully.");
    m_state = AutotuneState::FINISHED;
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

AutotuneResult AutotuneTask::testCandidate(uint16_t freq, uint16_t volt, bool strict_mode) {
    AutotuneResult result;
    result.frequency = freq;
    result.voltage = volt;
    result.valid = false;
    
    Board* board = SYSTEM_MODULE.getBoard();
    if (!board) return result;
    
    // Apply settings
    if (!applySettings(freq, volt)) {
        log("Failed to apply F=%d V=%d", freq, volt);
        return result;
    }
    
    log("Testing: %d MHz @ %d mV", freq, volt);
    
    // Wait for settling
    uint64_t settle_start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000 - settle_start) < SETTLE_TIME_MS) {
        if (!checkLimits()) {
            log("Limit exceeded during settling");
            return result;
        }
        if (m_stop_requested) return result;
        vTaskDelay(pdMS_TO_TICKS(POLLING_INTERVAL_MS));
    }
    
    // Measure
    std::vector<float> samples_hr;
    std::vector<float> samples_pwr;
    uint64_t measure_start = esp_timer_get_time() / 1000;
    
    while ((esp_timer_get_time() / 1000 - measure_start) < MEASURE_TIME_MS) {
        if (!checkLimits()) {
            log("Limit exceeded during measurement");
            return result;
        }
        if (m_stop_requested) return result;
        
        float hr = HASHRATE_MONITOR.getSmoothedTotalChipHashrate();
        float pwr = board->getPin();
        
        if (hr > 0) samples_hr.push_back(hr);
        if (pwr > 0) samples_pwr.push_back(pwr);
        
        vTaskDelay(pdMS_TO_TICKS(POLLING_INTERVAL_MS));
    }
    
    // Analyze
    if (samples_hr.empty() || samples_pwr.empty()) {
        log("No valid samples collected");
        return result;
    }
    
    float median_hr = calculate_median(samples_hr);
    float avg_pwr = 0;
    for (float p : samples_pwr) avg_pwr += p;
    avg_pwr /= samples_pwr.size();
    
    float temp = board->getVRTemp();
    for (int i = 0; i < board->getNumTempSensors(); i++) {
        float t = board->getChipTemp(i);
        if (!std::isnan(t) && t > temp) temp = t;
    }
    
    // Validate
    bool valid = true;
    if (median_hr < 1.0f) valid = false;
    if (avg_pwr > m_config.max_power) valid = false;
    if (temp > m_config.max_temp) valid = false;
    
    if (valid) {
        result.hashrate_ghs = median_hr;
        result.power_w = avg_pwr;
        result.efficiency_j_th = (median_hr > 0) ? (avg_pwr / (median_hr / 1000.0f)) : 999.0f;
        result.temp_c = temp;
        result.valid = true;
        
        log("PASS: F=%d V=%d HR=%.1f GH/s Pwr=%.1f W Eff=%.2f J/TH", 
            freq, volt, median_hr, avg_pwr, result.efficiency_j_th);
        
        // Cache result
        m_all_results.push_back(result);
    } else {
        log("FAIL: F=%d V=%d (hr=%.1f pwr=%.1f temp=%.1f)", 
            freq, volt, median_hr, avg_pwr, temp);
    }
    
    return result;
}

AutotuneResult AutotuneTask::optimizeVoltageForFrequency(uint16_t freq, uint16_t v_center, uint16_t v_step) {
    log("=== Optimizing voltage for F=%d MHz (center=%d mV, step=%d mV) ===", freq, v_center, v_step);
    
    const int MAX_ITERATIONS = 10; // Prevent infinite loops
    int iteration = 0;
    
    while (iteration < MAX_ITERATIONS && !m_stop_requested) {
        iteration++;
        
        // Test voltage window: center ± 2 steps
        std::vector<uint16_t> test_voltages;
        for (int offset = -2; offset <= 2; offset++) {
            uint16_t v = v_center + (offset * v_step);
            // Clamp to valid range
            if (v < 1100) v = 1100;
            if (v > m_config.max_voltage) v = m_config.max_voltage;
            
            // Avoid duplicates
            if (std::find(test_voltages.begin(), test_voltages.end(), v) == test_voltages.end()) {
                test_voltages.push_back(v);
            }
        }
        
        // Test all candidates in window
        std::vector<AutotuneResult> candidates;
        for (uint16_t v : test_voltages) {
            if (m_stop_requested) return {};
            
            AutotuneResult res = testCandidate(freq, v, false);
            if (res.valid) {
                candidates.push_back(res);
            }
        }
        
        // No stable point found
        if (candidates.empty()) {
            log("No stable voltage found for F=%d MHz", freq);
            return {};
        }
        
        // Find best (prefer lower voltage for same hashrate, so sort by efficiency)
        std::sort(candidates.begin(), candidates.end(), 
                  [](const AutotuneResult& a, const AutotuneResult& b) {
                      return a.efficiency_j_th < b.efficiency_j_th; // Lower is better
                  });
        
        AutotuneResult best = candidates[0];
        
        // Find min/max voltages tested
        uint16_t min_v = test_voltages[0];
        uint16_t max_v = test_voltages[0];
        for (uint16_t v : test_voltages) {
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
        }
        
        // Check if best is at edge of window
        if (best.voltage <= min_v && best.voltage > 1100) {
            // Best is at lower edge → search lower
            v_center = best.voltage - v_step;
            if (v_center < 1100) v_center = 1100;
            log("Best at lower edge (V=%d), expanding search down to %d mV", best.voltage, v_center);
            continue;
        }
        
        if (best.voltage >= max_v && best.voltage < m_config.max_voltage) {
            // Best is at upper edge → search higher
            v_center = best.voltage + v_step;
            if (v_center > m_config.max_voltage) v_center = m_config.max_voltage;
            log("Best at upper edge (V=%d), expanding search up to %d mV", best.voltage, v_center);
            continue;
        }
        
        // Best is inside window → we found it!
        log("Voltage optimized for F=%d MHz: V=%d mV (HR=%.1f GH/s, Eff=%.2f J/TH)", 
            freq, best.voltage, best.hashrate_ghs, best.efficiency_j_th);
        return best;
    }
    
    log("Max iterations reached for F=%d MHz", freq);
    return {};
}

bool AutotuneTask::confirmCandidate(const AutotuneResult& candidate) {
    log("=== Confirming candidate F=%d V=%d ===", candidate.frequency, candidate.voltage);
    
    // Test twice with strict mode
    AutotuneResult test1 = testCandidate(candidate.frequency, candidate.voltage, true);
    if (!test1.valid) {
        log("Confirmation FAILED (test 1): candidate not stable");
        return false;
    }
    
    AutotuneResult test2 = testCandidate(candidate.frequency, candidate.voltage, true);
    if (!test2.valid) {
        log("Confirmation FAILED (test 2): candidate not stable");
        return false;
    }
    
    // Check if results are consistent (within 5% hashrate variance)
    float variance = std::abs(test1.hashrate_ghs - test2.hashrate_ghs) / test1.hashrate_ghs;
    if (variance > 0.05f) {
        log("Confirmation FAILED: hashrate variance %.1f%% (test1=%.1f, test2=%.1f)", 
            variance * 100, test1.hashrate_ghs, test2.hashrate_ghs);
        return false;
    }
    
    log("Confirmation PASSED: F=%d V=%d (HR1=%.1f HR2=%.1f variance=%.1f%%)", 
        candidate.frequency, candidate.voltage, 
        test1.hashrate_ghs, test2.hashrate_ghs, variance * 100);
    return true;
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
