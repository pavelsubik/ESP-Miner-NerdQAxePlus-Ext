import { Component, OnInit, OnDestroy, ViewChild, ElementRef, AfterViewInit } from '@angular/core';
import { SystemService } from '../../services/system.service';
import { interval, Subscription, shareReplay, startWith, switchMap, catchError, of, take } from 'rxjs';
import Chart from 'chart.js/auto';

interface AutotuneState {
  isRunning: boolean;
  currentStep: number;
  totalSteps: number;
  currentFrequency: number;
  currentVoltage: number;
  bestFrequency: number;
  bestVoltage: number;
  bestHashrate: number;
  // New fields
  bestEfficiency: number;
  bestEffFrequency: number;
  bestEffVoltage: number;
  maxFrequency: number;
  maxVoltage: number;
  maxPower: number;
  maxVrTemp: number;
}

interface AutotuneDataPoint {
  timestamp: number;
  hashrate: number;
  voltage: number;
  frequency: number;
  temp: number;
  vrTemp: number;
}

@Component({
  selector: 'app-autotune',
  templateUrl: './autotune.component.html',
  styleUrls: ['./autotune.component.scss']
})
export class AutotuneComponent implements OnInit, OnDestroy, AfterViewInit {
  @ViewChild('autotuneChart') chartCanvas!: ElementRef<HTMLCanvasElement>;

  private chart: Chart | null = null;
  private dataSubscription: Subscription | null = null;
  private autotuneStatusSubscription: Subscription | null = null;

  // localStorage keys for persistence
  private readonly CHART_DATA_KEY = 'autotuneChartData';
  private readonly LOG_DATA_KEY = 'autotuneLogData';
  private readonly STATE_KEY = 'autotuneState';

  // Chart data arrays (persisted)
  private chartLabels: number[] = [];
  private chartHashrate: number[] = [];
  private chartVoltage: number[] = [];
  private chartFrequency: number[] = [];
  private chartTemp: number[] = [];
  private chartVrTemp: number[] = [];

  // Max data points to keep (10 minutes at 2s interval = 300 points)
  private readonly MAX_DATA_POINTS = 300;

  // Autotune state
  // Autotune state
  autotuneState: AutotuneState = {
    isRunning: false,
    currentStep: 0,
    totalSteps: 0,
    currentFrequency: 0,
    currentVoltage: 0,
    bestFrequency: 0,
    bestVoltage: 0,
    bestHashrate: 0,
    bestEfficiency: 0,
    bestEffFrequency: 0,
    bestEffVoltage: 0,
    maxFrequency: 800,
    maxVoltage: 1400,
    maxPower: 500,
    maxVrTemp: 100
  };

  // System defaults
  private defaultMaxPower = 500;
  private defaultMaxTemp = 100;

  // Log content (persisted)
  logLines: string[] = [];

  constructor(private systemService: SystemService) {}

  ngOnInit(): void {
    // Load persisted data first
    this.loadPersistedData();

    // Start chart data subscription (always runs)
    this.subscribeToChartData();

    // Check initial autotune status once
    this.checkInitialAutotuneStatus();

    // Load system defaults
    this.loadSystemDefaults();
  }

  private loadSystemDefaults(): void {
    this.systemService.getInfo(Date.now()).pipe(
      take(1),
      catchError(err => {
        console.warn('Failed to load system defaults', err);
        return of(null);
      })
    ).subscribe(info => {
      if (info) {
        if (info.maxPower) this.defaultMaxPower = info.maxPower;
        if (info.overheat_temp) this.defaultMaxTemp = info.overheat_temp;
        
        // If state has legacy defaults (500/100), update them to system defaults
        if (this.autotuneState.maxPower === 500 && this.autotuneState.maxVrTemp === 100) {
           this.autotuneState.maxPower = this.defaultMaxPower;
           this.autotuneState.maxVrTemp = this.defaultMaxTemp;
        }
      }
    });
  }

  ngAfterViewInit(): void {
    setTimeout(() => this.initChart(), 100);
  }

  ngOnDestroy(): void {
    // Save data before leaving
    this.savePersistedData();

    if (this.dataSubscription) {
      this.dataSubscription.unsubscribe();
    }
    if (this.autotuneStatusSubscription) {
      this.autotuneStatusSubscription.unsubscribe();
    }
    if (this.chart) {
      this.chart.destroy();
    }
  }

  private loadPersistedData(): void {
    // Load chart data
    const chartDataStr = localStorage.getItem(this.CHART_DATA_KEY);
    if (chartDataStr) {
      try {
        const data = JSON.parse(chartDataStr);
        this.chartLabels = data.labels || [];
        this.chartHashrate = data.hashrate || [];
        this.chartVoltage = data.voltage || [];
        this.chartFrequency = data.frequency || [];
        this.chartTemp = data.temp || [];
        this.chartVrTemp = data.vrTemp || [];

        // Filter out old data (older than 10 minutes)
        this.filterOldData();
      } catch (e) {
        console.warn('Failed to load autotune chart data', e);
      }
    }

    // Load log data
    const logDataStr = localStorage.getItem(this.LOG_DATA_KEY);
    if (logDataStr) {
      try {
        this.logLines = JSON.parse(logDataStr) || [];
      } catch (e) {
        console.warn('Failed to load autotune log data', e);
      }
    }

    // Initialize log if empty
    if (this.logLines.length === 0) {
      this.logLines = [
        '[INFO] Autotune module initialized',
        '[INFO] Waiting for user to start autotune process...'
      ];
    }

    // Load state
    const stateStr = localStorage.getItem(this.STATE_KEY);
    if (stateStr) {
      try {
        const state = JSON.parse(stateStr);
        this.autotuneState = { ...this.autotuneState, ...state };
      } catch (e) {
        console.warn('Failed to load autotune state', e);
      }
    }
  }

  private savePersistedData(): void {
    // Save chart data
    const chartData = {
      labels: this.chartLabels,
      hashrate: this.chartHashrate,
      voltage: this.chartVoltage,
      frequency: this.chartFrequency,
      temp: this.chartTemp,
      vrTemp: this.chartVrTemp
    };
    localStorage.setItem(this.CHART_DATA_KEY, JSON.stringify(chartData));

    // Save log data (keep last 100 lines)
    const logToSave = this.logLines.slice(-100);
    localStorage.setItem(this.LOG_DATA_KEY, JSON.stringify(logToSave));

    // Save state (but not isRunning - we don't want to resume)
    const stateToSave = { ...this.autotuneState, isRunning: false };
    localStorage.setItem(this.STATE_KEY, JSON.stringify(stateToSave));
  }

  private filterOldData(): void {
    const now = Date.now();
    const cutoff = now - 10 * 60 * 1000; // 10 minutes ago

    while (this.chartLabels.length > 0 && this.chartLabels[0] < cutoff) {
      this.chartLabels.shift();
      this.chartHashrate.shift();
      this.chartVoltage.shift();
      this.chartFrequency.shift();
      this.chartTemp.shift();
      this.chartVrTemp.shift();
    }
  }

  private subscribeToChartData(): void {
    const timer$ = interval(5000).pipe(startWith(0));

    // Chart Data Subscription (Global System Info) - always runs
    this.dataSubscription = timer$.pipe(
      switchMap(() => this.systemService.getInfo(Date.now()))
    ).subscribe(info => {
      if (info) {
        // Always update current freq/volt from system info
        this.autotuneState.currentFrequency = info.frequency || 0;
        this.autotuneState.currentVoltage = info.coreVoltage || 0;

        // Add data point for chart
        const timestamp = Date.now();
        this.addChartDataPoint({
          timestamp,
          hashrate: info.hashRate || 0,
          voltage: info.coreVoltage || 0,
          frequency: info.frequency || 0,
          temp: info.temp || 0,
          vrTemp: info.vrTemp || 0
        });

        // Periodically save data
        if (this.chartLabels.length % 10 === 0) {
          this.savePersistedData();
        }
      }
    });
  }

  private checkInitialAutotuneStatus(): void {
    // One-time check on page load
    this.systemService.getAutotuneStatus().pipe(
      catchError(err => {
        console.warn('Initial autotune status check error:', err);
        return of(null);
      })
    ).subscribe((status: any) => {
      if (status) {
        this.updateAutotuneState(status);
        // If autotune is already running, start polling
        if (status.isRunning) {
          this.startAutotunePolling();
        }
      }
    });
  }

  private startAutotunePolling(): void {
    // Don't start if already polling
    if (this.autotuneStatusSubscription) {
      return;
    }

    const timer$ = interval(5000).pipe(startWith(0));

    this.autotuneStatusSubscription = timer$.pipe(
      switchMap(() => this.systemService.getAutotuneStatus().pipe(
        catchError(err => {
          console.warn('Autotune status fetch error:', err);
          return of(null);
        })
      ))
    ).subscribe((status: any) => {
      if (status) {
        this.updateAutotuneState(status);

        // If autotune stopped, stop polling
        if (!status.isRunning) {
          this.stopAutotunePolling();
          this.addLogLine('[INFO] Autotune session ended');
        }
      }
    });
  }

  private stopAutotunePolling(): void {
    if (this.autotuneStatusSubscription) {
      this.autotuneStatusSubscription.unsubscribe();
      this.autotuneStatusSubscription = null;
    }
  }

  private updateAutotuneState(status: any): void {
    this.autotuneState.isRunning = status.isRunning;
    this.autotuneState.currentStep = status.currentStep;
    this.autotuneState.totalSteps = status.totalSteps;
    
    // Note: currentFrequency/currentVoltage come from system info, not autotune status

    this.autotuneState.bestHashrate = status.bestHashrate;
    this.autotuneState.bestFrequency = status.bestFrequency;
    this.autotuneState.bestVoltage = status.bestVoltage;
    
    this.autotuneState.bestEfficiency = status.bestEfficiency;
    this.autotuneState.bestEffFrequency = status.bestEffFrequency;
    this.autotuneState.bestEffVoltage = status.bestEffVoltage;
    
    // Logs - append backend logs if available
    if (status.logs && Array.isArray(status.logs) && status.logs.length > 0) {
      // Merge backend logs with local logs, avoiding duplicates
      for (const log of status.logs) {
        if (!this.logLines.includes(log)) {
          this.logLines.push(log);
        }
      }
      // Keep max 100 lines
      while (this.logLines.length > 100) {
        this.logLines.shift();
      }
    }
  }

  private initChart(): void {
    if (!this.chartCanvas?.nativeElement) return;

    const ctx = this.chartCanvas.nativeElement.getContext('2d');
    if (!ctx) return;

    this.chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: this.chartLabels.map(ts => new Date(ts).toLocaleTimeString()),
        datasets: [
          {
            label: 'Hashrate (GH/s)',
            data: this.chartHashrate,
            borderColor: '#00d68f',
            backgroundColor: 'rgba(0, 214, 143, 0.1)',
            yAxisID: 'y-hashrate',
            tension: 0.3,
            fill: true,
            pointRadius: 0,
            borderWidth: 1.5
          },
          {
            label: 'Voltage (mV)',
            data: this.chartVoltage,
            borderColor: '#0095ff',
            backgroundColor: 'transparent',
            yAxisID: 'y-voltage',
            tension: 0.3,
            pointRadius: 0,
            borderWidth: 1.5
          },
          {
            label: 'Frequency (MHz)',
            data: this.chartFrequency,
            borderColor: '#ff9500',
            backgroundColor: 'transparent',
            yAxisID: 'y-frequency',
            tension: 0.3,
            pointRadius: 0,
            borderWidth: 1.5
          },
          {
            label: 'ASIC Temp (°C)',
            data: this.chartTemp,
            borderColor: '#ff3d71',
            backgroundColor: 'transparent',
            yAxisID: 'y-temp',
            tension: 0.3,
            pointRadius: 0,
            borderWidth: 1.5
          },
          {
            label: 'VR Temp (°C)',
            data: this.chartVrTemp,
            borderColor: '#ffaa00',
            backgroundColor: 'transparent',
            yAxisID: 'y-temp',
            tension: 0.3,

            pointRadius: 0,
            borderWidth: 1.5
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        interaction: {
          mode: 'index',
          intersect: false
        },
        plugins: {
          legend: {
            position: 'top',
            labels: {
              color: '#a4abb3',
              usePointStyle: true,
              pointStyle: 'line'
            }
          }
        },
        scales: {
          x: {
            display: true,
            grid: {
              color: 'rgba(255, 255, 255, 0.1)'
            },
            ticks: {
              color: '#a4abb3',
              maxRotation: 0,
              maxTicksLimit: 13
            }
          },
          'y-hashrate': {
            type: 'linear',
            position: 'left',
            weight: 100,
            title: {
              display: true,
              text: 'Hashrate (GH/s)',
              color: '#00d68f'
            },
            grid: {
              color: 'rgba(255, 255, 255, 0.1)'
            },
            ticks: {
              color: '#00d68f'
            }
          },
          'y-voltage': {
            type: 'linear',
            position: 'right',
            title: {
              display: true,
              text: 'Voltage (mV)',
              color: '#0095ff'
            },
            grid: {
              display: false
            },
            ticks: {
              color: '#0095ff'
            }
          },
          'y-frequency': {
            type: 'linear',
            position: 'left',
            weight: 10,
            title: {
              display: true,
              text: 'Freq (MHz)',
              color: '#ff9500'
            },
            grid: {
              display: false
            },
            ticks: {
              color: '#ff9500'
            },
            suggestedMin: 400,
            suggestedMax: 900
          },
          'y-temp': {
            type: 'linear',
            position: 'right',
            title: {
              display: true,
              text: 'Temp (°C)',
              color: '#ff3d71'
            },
            grid: {
              display: false
            },
            ticks: {
              color: '#ff3d71'
            },
            // Dynamic range - will auto-scale based on data
            suggestedMin: 40,
            suggestedMax: 80
          }
        }
      }
    });
  }

  private addChartDataPoint(dataPoint: AutotuneDataPoint): void {
    // Add to data arrays
    this.chartLabels.push(dataPoint.timestamp);
    this.chartHashrate.push(dataPoint.hashrate);
    this.chartVoltage.push(dataPoint.voltage);
    this.chartFrequency.push(dataPoint.frequency);
    this.chartTemp.push(dataPoint.temp);
    this.chartVrTemp.push(dataPoint.vrTemp);

    // Keep exactly 12 points (1 minute at 5s interval)
    while (this.chartLabels.length > 12) {
      this.chartLabels.shift();
      this.chartHashrate.shift();
      this.chartVoltage.shift();
      this.chartFrequency.shift();
      this.chartTemp.shift();
      this.chartVrTemp.shift();
    }

    // Update chart if it exists
    if (this.chart) {
      const timeLabel = new Date(dataPoint.timestamp).toLocaleTimeString();

      this.chart.data.labels = this.chartLabels.map(ts => new Date(ts).toLocaleTimeString());
      this.chart.data.datasets[0].data = this.chartHashrate;
      this.chart.data.datasets[1].data = this.chartVoltage;
      this.chart.data.datasets[2].data = this.chartFrequency;
      this.chart.data.datasets[3].data = this.chartTemp;
      this.chart.data.datasets[4].data = this.chartVrTemp;

      this.chart.update('none');
    }
  }

  // Autotune control methods
  startAutotune(): void {
    // Validate inputs
    if (this.autotuneState.maxFrequency < 300 || this.autotuneState.maxFrequency > 1200) {
      this.addLogLine('[ERROR] Invalid Max Frequency');
      return;
    }
    if (this.autotuneState.maxVoltage < 1000 || this.autotuneState.maxVoltage > 1600) {
      this.addLogLine('[ERROR] Invalid Max Voltage');
      return;
    }

    // Clear log for new session
    this.logLines = [];
    this.addLogLine('[INFO] Starting new autotune session...');
    
    this.systemService.startAutotune(this.autotuneState).subscribe({
        next: () => {
             this.autotuneState.isRunning = true;
             this.addLogLine('[SUCCESS] Autotune started');
             // Start polling for status updates
             this.startAutotunePolling();
        },
        error: (err) => {
             this.autotuneState.isRunning = false;
             this.addLogLine('[ERROR] Failed to start: ' + err.message);
        }
    });
    
    this.savePersistedData();
  }

  stopAutotune(): void {
    this.systemService.stopAutotune().subscribe({
        next: () => {
            this.addLogLine('[WARN] Stop command sent');
        },
        error: (err) => {
             this.addLogLine('[ERROR] Failed to stop: ' + err.message);
        }
    });
  }

  resetAutotune(): void {
    this.autotuneState = {
      isRunning: false,
      currentStep: 0,
      totalSteps: 0,
      currentFrequency: 0,
      currentVoltage: 0,
      bestFrequency: 0,
      bestVoltage: 0,
      bestHashrate: 0,
      bestEfficiency: 0,
      bestEffFrequency: 0,
      bestEffVoltage: 0,
      maxFrequency: 800,
      maxVoltage: 1400,
      maxPower: this.defaultMaxPower,
      maxVrTemp: this.defaultMaxTemp
    };

    // Clear chart data
    this.chartLabels = [];
    this.chartHashrate = [];
    this.chartVoltage = [];
    this.chartFrequency = [];
    this.chartTemp = [];
    this.chartVrTemp = [];

    if (this.chart) {
      this.chart.data.labels = [];
      this.chart.data.datasets.forEach(ds => ds.data = []);
      this.chart.update();
    }

    // Clear log
    this.logLines = ['[INFO] Autotune reset'];

    // Clear localStorage
    localStorage.removeItem(this.CHART_DATA_KEY);
    localStorage.removeItem(this.LOG_DATA_KEY);
    localStorage.removeItem(this.STATE_KEY);
  }

  private addLogLine(message: string): void {
    const timestamp = new Date().toLocaleTimeString();
    this.logLines.push(`[${timestamp}] ${message}`);

    // Keep max 100 lines
    if (this.logLines.length > 100) {
      this.logLines.shift();
    }
  }

  getLogClass(line: string): string {
    if (line.includes('[ERROR]')) return 'log-error';
    if (line.includes('[WARN]')) return 'log-warning';
    if (line.includes('[SUCCESS]')) return 'log-success';
    return 'log-info';
  }
}
