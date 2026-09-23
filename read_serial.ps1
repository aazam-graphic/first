$port = New-Object System.IO.Ports.SerialPort('COM7',115200,'None',8,'One')
$port.ReadTimeout = 300
$port.Open()
$port.DtrEnable = $false
$port.RtsEnable = $true
Start-Sleep -Milliseconds 150
$port.RtsEnable = $false
$port.DiscardInBuffer()
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$out = New-Object System.Text.StringBuilder
while ($sw.Elapsed.TotalSeconds -lt 25) {
    try { $line = $port.ReadLine(); [void]$out.AppendLine($line) } catch {}
}
$port.Close()
[System.IO.File]::WriteAllText('C:\Users\aazam\Desktop\xbox360_controller\boot_os_log.txt', $out.ToString())
'DONE' | Out-File -FilePath 'C:\Users\aazam\Desktop\xbox360_controller\boot_os_log.done' -Encoding utf8
