Remove-Item -Recurse -Force 'C:\Users\aazam\Desktop\xbox360_controller\build' -ErrorAction SilentlyContinue
cd 'C:\Users\aazam\Desktop\xbox360_controller'
$env:IDF_PATH='C:\esp\v6.0.2\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.0.2\venv'
. 'C:\esp\v6.0.2\esp-idf\export.ps1' 2>$null
idf.py build 2>&1 | Out-File -FilePath 'C:\Users\aazam\AppData\Local\Temp\bld_clean.log' -Encoding utf8
Write-Output "EXIT=$env:LASTEXITCODE"
