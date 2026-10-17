# Run from an elevated PowerShell. Only explicit Start enables collection.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Start','Save','Status','Cancel')][string]$Action,
    [string]$OutputDirectory = "$env:LOCALAPPDATA\DeCrusher\Diagnostics"
)
$ErrorActionPreference = 'Stop'
$instance = 'DeCrusherDiagnostics'
switch ($Action) {
    'Start' {
        # Memory mode is circular; deliberately do not use -filemode.
        & wpr.exe -start GeneralProfile -start Audio -start Power -instancename $instance
        if ($LASTEXITCODE -ne 0) { throw 'Trace start failed. Run elevated; check for conflicting recordings. No existing trace was cancelled.' }
        Write-Host 'Recording in circular memory buffers. Save immediately after the glitch; older events are overwritten. Exiting DeCrusher does not stop this trace.'
    }
    'Save' {
        New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
        $path = Join-Path $OutputDirectory "incident-$stamp.etl"
        # Stop first to preserve the recent trace before exporting other evidence.
        & wpr.exe -stop $path 'Focusrite audio distortion incident' -instancename $instance
        if ($LASTEXITCODE -ne 0) { throw 'Trace save failed; inspect WPR status before retrying.' }
        $logs = @('System','Application','Microsoft-Windows-Audio/Operational',
            'Microsoft-Windows-Kernel-PnP/Device Management',
            'Microsoft-Windows-TaskScheduler/Maintenance',
            'Microsoft-Windows-Windows Defender/Operational')
        foreach ($log in $logs) {
            $safeName = $log -replace '[/\\ ]','_'
            & wevtutil.exe epl $log (Join-Path $OutputDirectory "$stamp-$safeName.evtx") '/q:*[System[TimeCreated[timediff(@SystemTime) <= 3600000]]]'
            if ($LASTEXITCODE -ne 0) { Write-Warning "Could not export $log" }
        }
        foreach ($name in @('events.log','events.log.1')) {
            $source = Join-Path "$env:LOCALAPPDATA\DeCrusher\Diagnostics" $name
            if (Test-Path -LiteralPath $source) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDirectory "$stamp-$name")
            }
        }
        Write-Host "Saved $path and event logs. Recording is stopped; use Start to arm again."
    }
    'Status' { & wpr.exe -status -instancename $instance; if ($LASTEXITCODE -ne 0) { throw 'Trace status failed or no trace is running.' } }
    'Cancel' { & wpr.exe -cancel -instancename $instance; if ($LASTEXITCODE -ne 0) { throw 'Trace cancellation failed or no trace is running.' } }
}
