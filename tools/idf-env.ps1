# Loads the ESP-IDF v4.4.8 environment into the current PowerShell session.
# Usage (dot-source it):   . .\tools\idf-env.ps1
# Then:                    idf.py build | idf.py -p COMx flash monitor
#
# Python 3.11 is pinned on purpose: ESP-IDF v4.4 was never validated on 3.12+,
# and the system default here is 3.14.

$env:IDF_PATH       = "C:\Espressif\frameworks\esp-idf-v4.4.8"
$env:IDF_TOOLS_PATH = "C:\Espressif"
$py311 = "$env:LOCALAPPDATA\Python\pythoncore-3.11-64\python.exe"

$kv = & $py311 "$env:IDF_PATH\tools\idf_tools.py" export --format key-value
foreach ($line in $kv) {
    $k, $v = $line -split '=', 2
    if ($k -eq 'PATH') { $v = $v -replace '%PATH%', $env:PATH }
    Set-Item "env:$k" $v
}

$env:PATH = "C:\Espressif\tools\sjasmplus\sjasmplus-1.24.0.win;$env:PATH"

function global:idf.py { python "$env:IDF_PATH\tools\idf.py" @args }

Write-Host "ESP-IDF v4.4.8 ready (python: $((python --version) 2>&1))"
