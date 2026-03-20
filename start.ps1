# Load env vars from Env/.env
Get-Content "Env/.env" | ForEach-Object {
    if ($_ -match "^\s*([^#][^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1].Trim(), $matches[2].Trim(), "Process")
    }
}

# Start backend
Start-Process powershell -ArgumentList "-NoExit", "-Command", "C:\Users\garre\AppData\Local\Python\bin\python3.exe backEnd/server.py"

# Serve frontend
Start-Process powershell -ArgumentList "-NoExit", "-Command", "C:\Users\garre\AppData\Local\Python\bin\python3.exe -m http.server 5500 --directory frontEnd"

Start-Sleep -Seconds 2
Start-Process "http://127.0.0.1:5500"
