# Runs build\cmake\wg3d.exe for a bounded time and captures its output (Phase 3 bring-up).
# Usage: tools\run_wg3d.ps1 [-Seconds 30] [-Name run] [-Trace <file>] [extra wg3d.exe args...]
# Writes build\<Name>.out.txt / build\<Name>.err.txt and prints their tails.
param(
    [int]$Seconds = 30,
    [string]$Name = "run",
    [string]$Trace = "",
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\cmake\wg3d.exe"
$out = Join-Path $root "build\$Name.out.txt"
$err = Join-Path $root "build\$Name.err.txt"
if ($Trace) { $env:WG3D_TRACE_FILE = $Trace } else { Remove-Item Env:WG3D_TRACE_FILE -ErrorAction SilentlyContinue }
$args_ = @{ FilePath = $exe; WorkingDirectory = (Split-Path $exe); RedirectStandardOutput = $out; RedirectStandardError = $err; PassThru = $true; NoNewWindow = $true }
if ($Rest) { $args_.ArgumentList = $Rest }
$p = Start-Process @args_
if (-not $p.WaitForExit($Seconds * 1000)) {
    Stop-Process -Id $p.Id -Force
    "[run_wg3d] still running after $Seconds s (killed)"
} else {
    "[run_wg3d] exited, code 0x{0:X8}" -f $p.ExitCode
}
"---- stdout (tail)"; Get-Content $out -Tail 20
"---- stderr (tail)"; Get-Content $err -Tail 60
