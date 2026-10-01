# Starts MadCraft: the Minecraft client (Fabric dev run of fabric/) and Mad Max with the plugin.
# The desktop shortcut "MadCraft" runs this. Either game can come up first; they link on their own.

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$game = 'C:\Program Files\GOG Galaxy\Games\Mad Max'
$javaHome = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot'

# Minecraft: unless the MadCraft client is already up.
$mcRunning = Get-CimInstance Win32_Process -Filter "Name = 'java.exe' OR Name = 'javaw.exe'" |
	Where-Object { $_.CommandLine -match 'KnotClient|net\.fabricmc' }
if (-not $mcRunning) {
	Write-Host 'Starting Minecraft...'
	$cmd = "`$env:JAVA_HOME = '$javaHome'; Set-Location '$repo\fabric'; .\gradlew.bat runClient --no-daemon"
	Start-Process powershell -ArgumentList '-NoProfile', '-WindowStyle', 'Minimized', '-Command', $cmd -WindowStyle Minimized
} else {
	Write-Host 'Minecraft is already running.'
}

# Mad Max (the plugin, dinput8.dll, loads with it).
if (-not (Get-Process MadMax -ErrorAction SilentlyContinue)) {
	Write-Host 'Starting Mad Max...'
	Start-Process -FilePath "$game\MadMax.exe" -WorkingDirectory $game
} else {
	Write-Host 'Mad Max is already running.'
}
Start-Sleep -Seconds 2
