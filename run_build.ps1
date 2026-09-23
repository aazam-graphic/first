$env:IDF_PATH = 'C:\esp\v6.0.2\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0.2\venv'
Set-Location 'C:\Users\aazam\Desktop\xbox360_controller'
& 'C:\esp\v6.0.2\esp-idf\export.ps1' $null
Set-Location 'C:\Users\aazam\Desktop\xbox360_controller'
idf.py -p COM7 flash 2>&1 | Out-File -FilePath 'C:\Users\aazam\AppData\Local\Temp\flash15.log' -Encoding utf8
Write-Output "FLASH_EXIT=$env:LASTEXITCODE"
