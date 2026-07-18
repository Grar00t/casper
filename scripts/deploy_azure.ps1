#!/usr/bin/env pwsh
# deploy_azure.ps1 — نشر NIYAH Engine على Azure VM
# الاستخدام: .\scripts\deploy_azure.ps1 -KeyFile "C:\path\to\key.pem"

param(
    [string]$VM = "20.91.208.59",
    [string]$User = "azureuser",
    [string]$KeyFile = "$env:USERPROFILE\.ssh\casper_vm.pem",
    [string]$RemotePath = "/opt/casper-agent"
)

$src = "$PSScriptRoot\..\niyah_engine_local"

Write-Host "🚀 Deploying NIYAH Engine to $User@$VM" -ForegroundColor Cyan
Write-Host "📁 Source: $src"
Write-Host "📁 Target: $RemotePath"

# تحقق من وجود ملف الـ key
if (-not (Test-Path $KeyFile)) {
    Write-Host "❌ SSH key not found: $KeyFile" -ForegroundColor Red
    Write-Host "   Provide key path: -KeyFile C:\path\to\key.pem"
    exit 1
}

# إنشاء المجلد على VM
Write-Host "`n1. Creating remote directory..." -ForegroundColor Yellow
ssh -i $KeyFile -o StrictHostKeyChecking=no "$User@$VM" "sudo mkdir -p $RemotePath && sudo chown $User`:$User $RemotePath"

# نسخ الملفات (بدون node_modules)
Write-Host "`n2. Copying files..." -ForegroundColor Yellow
$files = @("server.js", "package.json", "lib/niyahEngine.js", "lib/searchProvider.js", 
           "lib/reasoner.js", "lib/relevance.js", "lib/memory.js", "routes/niyah.js")

foreach ($f in $files) {
    $localFile = Join-Path $src $f
    $remoteDir = "$RemotePath/$(Split-Path $f -Parent)"
    ssh -i $KeyFile "$User@$VM" "mkdir -p $remoteDir"
    scp -i $KeyFile $localFile "$User@${VM}:$RemotePath/$f"
    Write-Host "  ✅ $f" -ForegroundColor Green
}

# تثبيت dependencies وإعادة التشغيل
Write-Host "`n3. Installing dependencies..." -ForegroundColor Yellow
ssh -i $KeyFile "$User@$VM" @"
cd $RemotePath
npm install --omit=dev
echo '✅ npm install done'
"@

# إعادة تشغيل السيرفر (PM2 أو systemd)
Write-Host "`n4. Restarting service..." -ForegroundColor Yellow
ssh -i $KeyFile "$User@$VM" @"
if command -v pm2 &> /dev/null; then
    pm2 delete casper-agent 2>/dev/null || true
    cd $RemotePath && pm2 start server.js --name casper-agent --env production
    pm2 save
    echo '✅ PM2 restarted'
elif systemctl is-active --quiet casper-agent; then
    sudo systemctl restart casper-agent
    echo '✅ systemd restarted'
else
    cd $RemotePath && nohup node server.js > /tmp/casper.log 2>&1 &
    echo "✅ Started (PID: $!)"
fi
"@

# اختبار
Write-Host "`n5. Testing..." -ForegroundColor Yellow
Start-Sleep 3
try {
    $r = Invoke-RestMethod "http://$VM/health" -TimeoutSec 5
    Write-Host "  ✅ /health: $($r.service) - $($r.time)" -ForegroundColor Green
} catch {
    Write-Host "  ⚠️  Health check failed (nginx may need restart): $_" -ForegroundColor Yellow
    ssh -i $KeyFile "$User@$VM" "sudo nginx -t && sudo systemctl reload nginx"
}

Write-Host "`n✅ Deployment complete!" -ForegroundColor Green
Write-Host "   Test: curl -X POST http://$VM/api/v1/niyah/ask -H 'Content-Type: application/json' -d '{`"query`":`"ما هو الذكاء الاصطناعي؟`"}'"
