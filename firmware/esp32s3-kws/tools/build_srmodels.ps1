# 重新打包 srmodels.bin (MultiNet6 中文命令模型 + VAD，不含 WakeNet)
# 用法:  powershell -ExecutionPolicy Bypass -File tools\build_srmodels.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root "tools\srmodels_src"
$out  = Join-Path $root "srmodels.bin"

if (-not (Test-Path $src)) { throw "缺少模型源目录: $src" }

python (Join-Path $root "tools\pack_model.py") -m $src -o srmodels.bin
$packed = Join-Path $src "srmodels.bin"
Move-Item -LiteralPath $packed -Destination $out -Force

$len = (Get-Item -LiteralPath $out).Length
Write-Host ("已生成 {0}  ({1:N0} 字节)" -f $out, $len)
Write-Host "model 分区 offset = 0x710000, 容量 = 0x600000 (6 MB)"
if ($len -gt 0x600000) { Write-Warning "模型包超过 model 分区容量, 需要调大分区!" }
