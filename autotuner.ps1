#requires -Version 7.0

param(
  [string]$Ip = "192.168.0.20",

  # Start point
  [int]$StartFreq = 600,
  [int]$StartVolt = 1150,

  # Search bounds
  [int]$MinFreq = 650,
  [int]$MaxFreq = 800,
  [int]$MinVolt = 1100,
  [int]$MaxVolt = 1300,

  # Coarse steps
  [int]$CoarseFreqStep = 10,
  [int]$CoarseVoltStep = 10,

  # Timing
  [int]$SettleSec = 90,          # user requested 1.5 min
  [int]$SampleIntervalSec = 5,
  [int]$CoarseSamples = 12,      # 12*5s = 60s sampling
  [int]$FineSamples = 18,        # 18*5s = 90s sampling
  [int]$MicroSamples = 12,       # 60s sampling
  [int]$ConfirmSamples = 18,     # 90s sampling

  # Limits (set what you really want; in your latest run you were around 120W)
  [double]$MaxWatts = 999,
  [double]$MaxTemp = 80,
  [double]$MaxVRTemp = 85,

  # Stability tolerances
  [int]$RejectTolerance = 0,
  [int]$DupTolerance = 0,

  # Early stopping
  [double]$DropStopThreshold = 150, # if best@F drops by > threshold GH/s, count as "bad"
  [int]$BadStreakStop = 2,

  # Logging
  [string]$LogPrefix = "autotune"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# -------------------------
# Helpers
# -------------------------
$BaseUrl = "http://$Ip"
$script:allResults = New-Object System.Collections.Generic.List[object]
$script:cache = @{}  # key -> result object

function Clamp([int]$x, [int]$min, [int]$max) {
  if ($x -lt $min) { return $min }
  if ($x -gt $max) { return $max }
  return $x
}

function Get-Median([double[]]$vals) {
  if (-not $vals -or $vals.Count -eq 0) { return [double]::NaN }
  $s = $vals | Sort-Object
  $n = $s.Count
  if ($n % 2 -eq 1) { return $s[($n - 1) / 2] }
  return (($s[$n / 2 - 1] + $s[$n / 2]) / 2.0)
}

function Get-Avg([double[]]$vals) {
  if (-not $vals -or $vals.Count -eq 0) { return [double]::NaN }
  return ($vals | Measure-Object -Average).Average
}

function Get-StdDev([double[]]$vals) {
  if (-not $vals -or $vals.Count -lt 2) { return 0.0 }
  $avg = Get-Avg $vals
  $sum = 0.0
  foreach ($v in $vals) { $sum += [math]::Pow(($v - $avg), 2) }
  return [math]::Sqrt($sum / ($vals.Count - 1))
}

function Invoke-Json([string]$Method, [string]$Url, $Body = $null, [int]$Retries = 3, [int]$TimeoutSec = 5) {
  for ($i = 1; $i -le $Retries; $i++) {
    try {
      if ($null -ne $Body) {
        $json = $Body | ConvertTo-Json -Depth 10 -Compress
        return Invoke-RestMethod -Method $Method -Uri $Url -ContentType "application/json" -Body $json -TimeoutSec $TimeoutSec
      }
      else {
        return Invoke-RestMethod -Method $Method -Uri $Url -TimeoutSec $TimeoutSec
      }
    }
    catch {
      if ($i -ge $Retries) { throw }
      Start-Sleep -Milliseconds (250 * $i)
    }
  }
}

function Get-Info() {
  return Invoke-Json -Method "GET" -Url "$BaseUrl/api/system/info" -Retries 3 -TimeoutSec 5
}

function Set-Miner([int]$F, [int]$V) {
  $payload = @{ coreVoltage = $V; frequency = $F }
  [void](Invoke-Json -Method "PATCH" -Url "$BaseUrl/api/system" -Body $payload -Retries 3 -TimeoutSec 5)
}

function Get-CacheKey([int]$F, [int]$V, [int]$Settle, [int]$Samples, [int]$Interval, [double]$MaxW, [double]$MaxT, [double]$MaxVRT, [int]$RejTol, [int]$DupTol, [string]$Mode) {
  return "$Mode|F=$F|V=$V|settle=$Settle|n=$Samples|dt=$Interval|maxW=$MaxW|maxT=$MaxT|maxVR=$MaxVRT|rej=$RejTol|dup=$DupTol"
}

function Test-Candidate {
  param(
    [int]$F,
    [int]$V,
    [string]$Mode = "coarse",
    [int]$Settle = $SettleSec,
    [int]$Samples = $CoarseSamples,
    [int]$Interval = $SampleIntervalSec,
    [double]$MaxW = $MaxWatts,
    [double]$MaxT = $MaxTemp,
    [double]$MaxVRT = $MaxVRTemp,
    [int]$RejTol = $RejectTolerance,
    [int]$DupTol = $DupTolerance,
    [switch]$UseCache
  )

  $key = Get-CacheKey -F $F -V $V -Settle $Settle -Samples $Samples -Interval $Interval -MaxW $MaxW -MaxT $MaxT -MaxVRT $MaxVRT -RejTol $RejTol -DupTol $DupTol -Mode $Mode
  if ($UseCache -and $script:cache.ContainsKey($key)) {
    $cached = $script:cache[$key]
    return $cached
  }

  Write-Host ("`n=== Test F={0} MHz, V={1} mV (mode={2}) ===" -f $F, $V, $Mode)
  Write-Host "Applying settings via PATCH /api/system ..."
  Set-Miner -F $F -V $V

  Write-Host ("Settling {0}s ..." -f $Settle)
  Start-Sleep -Seconds $Settle

  # Baseline counters
  $base = $null
  try { $base = Get-Info } catch { $base = $null }
  if ($null -eq $base) {
    $res = [pscustomobject]@{
      ts              = (Get-Date).ToString("HH:mm:ss")
      mode            = $Mode
      F               = $F
      Vset            = $V
      Vactual         = $null
      pass            = $false
      reason          = "api"
      med_hashRate    = $null
      med_hashRate_1m = $null
      avg_power_W     = $null
      max_temp_C      = $null
      max_vrTemp_C    = $null
      rej_delta       = $null
      dup_delta       = $null
      cached          = $false
    }
    $script:allResults.Add($res)
    if ($UseCache) { $script:cache[$key] = $res }
    Write-Host "Sampling failed immediately (API not reachable)."
    return $res
  }

  $rej0 = [int]$base.sharesRejected
  $dup0 = [int]$base.duplicateHWNonces
  $upt0 = [int]$base.uptimeSeconds

  $hr = New-Object System.Collections.Generic.List[double]
  $hr1m = New-Object System.Collections.Generic.List[double]
  $pwr = New-Object System.Collections.Generic.List[double]
  $tmp = New-Object System.Collections.Generic.List[double]
  $vrt = New-Object System.Collections.Generic.List[double]

  $apiErrors = 0
  $restarted = $false

  Write-Host "Sampling..."
  for ($i = 1; $i -le $Samples; $i++) {
    try {
      $s = Get-Info
      $upt = [int]$s.uptimeSeconds
      if ($upt -lt $upt0) { $restarted = $true }

      $hr.Add([double]$s.hashRate)
      $hr1m.Add([double]$s.hashRate_1m)
      $pwr.Add([double]$s.power)
      $tmp.Add([double]$s.temp)
      $vrt.Add([double]$s.vrTemp)
    }
    catch {
      $apiErrors++
      if ($apiErrors -ge 2) {
        break
      }
    }

    if ($i -lt $Samples) { Start-Sleep -Seconds $Interval }
  }

  $end = $null
  try { $end = Get-Info } catch { $end = $null }

  $rej1 = if ($end) { [int]$end.sharesRejected } else { $rej0 }
  $dup1 = if ($end) { [int]$end.duplicateHWNonces } else { $dup0 }
  $vact = if ($end) { [int]$end.coreVoltageActual } else { [int]$base.coreVoltageActual }

  $rejD = $rej1 - $rej0
  $dupD = $dup1 - $dup0

  $medHr = Get-Median $hr.ToArray()
  $medHr1 = Get-Median $hr1m.ToArray()
  $avgP = Get-Avg $pwr.ToArray()
  $maxT = ($tmp | Measure-Object -Maximum).Maximum
  $maxVR = ($vrt | Measure-Object -Maximum).Maximum

  $pass = $true
  $reason = "ok"

  if ($apiErrors -ge 2) { $pass = $false; $reason = "api" }
  elseif ($restarted) { $pass = $false; $reason = "restart" }
  elseif ($dupD -gt $DupTol) { $pass = $false; $reason = "dupNonces" }
  elseif ($rejD -gt $RejTol) { $pass = $false; $reason = "rejects" }
  elseif ($avgP -gt $MaxW) { $pass = $false; $reason = "limit:power" }
  elseif ($maxT -gt $MaxT) { $pass = $false; $reason = "limit:temp" }
  elseif ($maxVR -gt $MaxVRT) { $pass = $false; $reason = "limit:vrtemp" }

  $res = [pscustomobject]@{
    ts              = (Get-Date).ToString("HH:mm:ss")
    mode            = $Mode
    F               = $F
    Vset            = $V
    Vactual         = $vact
    pass            = $pass
    reason          = $reason
    med_hashRate    = [math]::Round($medHr, 2)
    med_hashRate_1m = [math]::Round($medHr1, 2)
    avg_power_W     = [math]::Round($avgP, 2)
    max_temp_C      = $maxT
    max_vrTemp_C    = $maxVR
    rej_delta       = $rejD
    dup_delta       = $dupD
    cached          = $false
  }

  $script:allResults.Add($res)
  if ($UseCache) { $script:cache[$key] = $res }

  Write-Host ("Done. PASS={0} reason={1} | medHR1m={2:N2} | Pavg={3:N2}W | rejΔ={4} dupΔ={5}" -f $pass, $reason, $res.med_hashRate_1m, $res.avg_power_W, $rejD, $dupD)

  return $res
}

function Optimize-VoltForFreq {
  param(
    [int]$F,
    [int]$Vcenter,
    [int]$Vstep,
    [int]$Rounds = 3,
    [int]$SpanSteps = 2
  )

  $center = Clamp $Vcenter $MinVolt $MaxVolt
  $best = $null

  for ($r = 1; $r -le $Rounds; $r++) {
    $cands = @()
    for ($k = - $SpanSteps; $k -le $SpanSteps; $k++) {
      $v = Clamp ($center + $k * $Vstep) $MinVolt $MaxVolt
      $cands += $v
    }
    $cands = $cands | Sort-Object -Unique

    Write-Host ("Voltage sweep @ F={0}: {1}" -f $F, ($cands -join ", "))

    $roundBest = $null
    foreach ($v in $cands) {
      $t = Test-Candidate -F $F -V $v -Mode "coarse" -Samples $CoarseSamples -Settle $SettleSec -UseCache
      if ($t.pass) {
        if (($null -eq $roundBest) -or ($t.med_hashRate_1m -gt $roundBest.med_hashRate_1m)) {
          $roundBest = $t
        }
      }
    }

    if ($null -eq $roundBest) {
      return $null
    }

    if (($null -eq $best) -or ($roundBest.med_hashRate_1m -gt $best.med_hashRate_1m)) {
      $best = $roundBest
    }

    # If best is near the edge, shift center and try again
    $minV = ($cands | Measure-Object -Minimum).Minimum
    $maxV = ($cands | Measure-Object -Maximum).Maximum
    if ($roundBest.Vset -le $minV) {
      $center = Clamp ($roundBest.Vset - $Vstep) $MinVolt $MaxVolt
      continue
    }
    if ($roundBest.Vset -ge $maxV) {
      $center = Clamp ($roundBest.Vset + $Vstep) $MinVolt $MaxVolt
      continue
    }

    # Best is inside tested window -> stop
    break
  }

  return $best
}

function Fine-Refine {
  param([object]$BestIn)

  $best = $BestIn
  for ($iter = 1; $iter -le 3; $iter++) {
    Write-Host ("`n===== FINE ITER {0} around F={1} V={2} =====" -f $iter, $best.F, $best.Vset)

    $improved = $false

    # Try +/-5 MHz
    $left = Test-Candidate -F (Clamp ($best.F - 5) $MinFreq $MaxFreq) -V $best.Vset -Mode "fine" -Samples $FineSamples -Settle $SettleSec -UseCache
    $right = Test-Candidate -F (Clamp ($best.F + 5) $MinFreq $MaxFreq) -V $best.Vset -Mode "fine" -Samples $FineSamples -Settle $SettleSec -UseCache

    foreach ($t in @($left, $right)) {
      if ($t.pass -and $t.med_hashRate_1m -gt $best.med_hashRate_1m) {
        $best = $t; $improved = $true
        Write-Host ("FINE IMPROVED (F): F={0} V={1} medHR1m={2}" -f $best.F, $best.Vset, $best.med_hashRate_1m)
      }
    }

    # Try +/-5 mV
    $down = Test-Candidate -F $best.F -V (Clamp ($best.Vset - 5) $MinVolt $MaxVolt) -Mode "fine" -Samples $FineSamples -Settle $SettleSec -UseCache
    $up = Test-Candidate -F $best.F -V (Clamp ($best.Vset + 5) $MinVolt $MaxVolt) -Mode "fine" -Samples $FineSamples -Settle $SettleSec -UseCache

    foreach ($t in @($down, $up)) {
      if ($t.pass -and $t.med_hashRate_1m -gt $best.med_hashRate_1m) {
        $best = $t; $improved = $true
        Write-Host ("FINE IMPROVED (V): F={0} V={1} medHR1m={2}" -f $best.F, $best.Vset, $best.med_hashRate_1m)
      }
    }

    if (-not $improved) { break }
  }

  return $best
}

function Micro-Refine {
  param([object]$BestIn)

  $best = $BestIn
  $neighbors = @()

  Write-Host ("`n===== MICRO around F={0} V={1} (step 1/1) =====" -f $best.F, $best.Vset)

  $Fs = @($best.F - 1, $best.F, $best.F + 1) | ForEach-Object { Clamp $_ $MinFreq $MaxFreq } | Sort-Object -Unique
  $Vs = @($best.Vset - 1, $best.Vset, $best.Vset + 1) | ForEach-Object { Clamp $_ $MinVolt $MaxVolt } | Sort-Object -Unique

  foreach ($f in $Fs) {
    foreach ($v in $Vs) {
      $t = Test-Candidate -F $f -V $v -Mode "micro" -Samples $MicroSamples -Settle $SettleSec -UseCache
      $neighbors += $t
      if ($t.pass -and $t.med_hashRate_1m -gt $best.med_hashRate_1m) {
        $best = $t
      }
    }
  }

  return [pscustomobject]@{ best = $best; tested = $neighbors }
}

function Get-ConfirmCandidates([object]$PrimaryBest) {
  # Build candidate list:
  # 1) Primary best
  # 2) All PASS results sorted by medHR1m desc (dedup by F+V)
  $cand = New-Object System.Collections.Generic.List[object]
  $cand.Add($PrimaryBest)

  $passAll = $script:allResults | Where-Object { $_.pass -eq $true } |
  Sort-Object -Property med_hashRate_1m -Descending

  $seen = @{}
  foreach ($t in $passAll) {
    $k = "F=$($t.F)|V=$($t.Vset)"
    if (-not $seen.ContainsKey($k)) {
      $seen[$k] = $true
      $cand.Add($t)
    }
  }

  # Dedup primary too
  $final = @()
  $seen2 = @{}
  foreach ($t in $cand) {
    $k = "F=$($t.F)|V=$($t.Vset)"
    if (-not $seen2.ContainsKey($k)) {
      $seen2[$k] = $true
      $final += $t
    }
  }
  return $final
}

function Confirm-BestWithFallback {
  param([object]$PrimaryBest)

  Write-Host "`n===== CONFIRM (fallback enabled, strict rejects/dup = 0) ====="

  $cands = Get-ConfirmCandidates -PrimaryBest $PrimaryBest

  foreach ($c in $cands) {
    Write-Host ("`n-- Confirming candidate F={0} V={1} (from {2}) --" -f $c.F, $c.Vset, $c.mode)

    $c1 = Test-Candidate -F $c.F -V $c.Vset -Mode "confirm" -Samples $ConfirmSamples -Settle $SettleSec `
      -RejTol 0 -DupTol 0 -UseCache:$false
    if (-not $c1.pass) {
      Write-Host ("CONFIRM FAIL: reason={0} (dupΔ={1} rejΔ={2})" -f $c1.reason, $c1.dup_delta, $c1.rej_delta)
      continue
    }

    $c2 = Test-Candidate -F $c.F -V $c.Vset -Mode "confirm" -Samples $ConfirmSamples -Settle $SettleSec `
      -RejTol 0 -DupTol 0 -UseCache:$false
    if (-not $c2.pass) {
      Write-Host ("CONFIRM FAIL (run2): reason={0} (dupΔ={1} rejΔ={2})" -f $c2.reason, $c2.dup_delta, $c2.rej_delta)
      continue
    }

    Write-Host ("CONFIRM PASS (2/2): F={0} V={1}" -f $c.F, $c.Vset)
    return [pscustomobject]@{ best = $c; conf1 = $c1; conf2 = $c2 }
  }

  return $null
}

# -------------------------
# Main
# -------------------------
Write-Host "Starting AutoTune (Coarse 2D -> Fine -> Micro -> Confirm+Fallback)"
Write-Host ("Bounds: F {0}-{1} (step {2}), V {3}-{4} (step {5})" -f $MinFreq, $MaxFreq, $CoarseFreqStep, $MinVolt, $MaxVolt, $CoarseVoltStep)
Write-Host ("Start:  F {0}, V {1} | Limits: {2}W, T {3}C, VR {4}C" -f $StartFreq, $StartVolt, $MaxWatts, $MaxTemp, $MaxVRTemp)
Write-Host "======================================================================"

$init = Get-Info
Write-Host ("Initial miner: F={0} Vset={1} Vact={2} HR1m={3:N0} P={4:N1}W T={5:N1}C VR={6:N1}C uptime={7}" -f `
    $init.frequency, $init.coreVoltage, $init.coreVoltageActual, $init.hashRate_1m, $init.power, $init.temp, $init.vrTemp, $init.uptimeSeconds)

Write-Host ""
Write-Host ("Applying start point: F={0} V={1}" -f $StartFreq, $StartVolt)
Set-Miner -F $StartFreq -V $StartVolt
Start-Sleep -Seconds $SettleSec

$globalBest = $null
$badStreak = 0
$carryV = Clamp $StartVolt $MinVolt $MaxVolt

# Build frequency list
$freqs = @()
for ($f = $MinFreq; $f -le $MaxFreq; $f += $CoarseFreqStep) { $freqs += $f }

foreach ($F in $freqs) {
  Write-Host ""
  Write-Host ("===== COARSE F={0} MHz =====" -f $F)

  $bestF = Optimize-VoltForFreq -F $F -Vcenter $carryV -Vstep $CoarseVoltStep

  if ($bestF -eq $null) {
    Write-Host "COARSE: no stable point at this frequency."
    $badStreak++
    if ($badStreak -ge $BadStreakStop) { break }
    continue
  }

  $carryV = $bestF.Vset

  if (($globalBest -eq $null) -or ($bestF.med_hashRate_1m -gt $globalBest.med_hashRate_1m)) {
    $globalBest = $bestF
    $badStreak = 0
    Write-Host ("NEW GLOBAL BEST: F={0} V={1} medHR1m={2}" -f $globalBest.F, $globalBest.Vset, $globalBest.med_hashRate_1m)
  }
  else {
    $drop = $globalBest.med_hashRate_1m - $bestF.med_hashRate_1m
    Write-Host ("Best@F: V={0} medHR1m={1} (drop vs best: {2:N2})" -f $bestF.Vset, $bestF.med_hashRate_1m, $drop)
    if ($drop -gt $DropStopThreshold) { $badStreak++ } else { $badStreak = 0 }
    if ($badStreak -ge $BadStreakStop) { break }
  }
}

if ($globalBest -eq $null) {
  Write-Host ""
  Write-Host "No stable candidate found in the search range. Restoring start point."
  Set-Miner -F $StartFreq -V $StartVolt
  exit 1
}

Write-Host ""
Write-Host ("=== COARSE BEST: F={0} V={1} medHR1m={2} ===" -f $globalBest.F, $globalBest.Vset, $globalBest.med_hashRate_1m)

# Fine refinement
$best = Fine-Refine -BestIn $globalBest

Write-Host ""
Write-Host ("=== PRE-MICRO BEST: F={0} V={1} medHR1m={2} ===" -f $best.F, $best.Vset, $best.med_hashRate_1m)

# Micro refinement (step 1 for F and V)
$micro = Micro-Refine -BestIn $best
$best = $micro.best

Write-Host ""
Write-Host ("=== POST-MICRO BEST: F={0} V={1} medHR1m={2} ===" -f $best.F, $best.Vset, $best.med_hashRate_1m)

# Confirm with fallback
$confirmed = Confirm-BestWithFallback -PrimaryBest $best
if ($null -eq $confirmed) {
  Write-Host ""
  Write-Host "No candidate passed strict confirmation. Restoring start point."
  Set-Miner -F $StartFreq -V $StartVolt
  exit 2
}

$final = $confirmed.best

Write-Host ""
Write-Host ("APPLYING FINAL BEST: F={0} V={1}" -f $final.F, $final.Vset)
Set-Miner -F $final.F -V $final.Vset

# Export log
$logPath = Join-Path $PWD ("{0}_{1}.csv" -f $LogPrefix, (Get-Date).ToString("yyyyMMdd_HHmmss"))
$script:allResults | Export-Csv -NoTypeInformation -Encoding UTF8 $logPath

Write-Host ""
Write-Host "===== FINAL RESULT ====="
[pscustomobject]@{
  best_F           = $final.F
  best_V           = $final.Vset
  confirm1_medHR1m = $confirmed.conf1.med_hashRate_1m
  confirm2_medHR1m = $confirmed.conf2.med_hashRate_1m
  log              = $logPath
} | Format-List
