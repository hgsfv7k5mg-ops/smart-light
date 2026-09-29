# Smart Light 构建脚本：自行准备 ESP-IDF 工具链环境变量，然后调用 idf.py
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -ExtraArgs "-p COM3 flash monitor"
#
# 脚本顶部的路径按本机 ESP-IDF 安装位置填写；
# 若已在环境变量中导出 IDF_PATH / IDF_TOOLS_PATH 等，脚本会直接沿用。
param(
    [string]$ExtraArgs = "build"
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
#  工具链路径 —— 请按本机实际安装位置修改（或通过环境变量覆盖）
# ---------------------------------------------------------------------------
if (-not $env:IDF_PATH)            { $env:IDF_PATH            = "D:\espv5\esp\v5.2.7\esp-idf" }
if (-not $env:IDF_TOOLS_PATH)      { $env:IDF_TOOLS_PATH      = "C:\Espressif" }
if (-not $env:IDF_TARGET)          { $env:IDF_TARGET          = "esp32s3" }
if (-not $env:IDF_PYTHON_ENV_PATH) { $env:IDF_PYTHON_ENV_PATH = "$env:USERPROFILE\.espressif\python_env\idf5.2_py3.11_env" }
$env:IDF_PYTHON_CHECK = "0"

# kconfgen 读 sdkconfig 时用系统默认编码（中文 Windows 上是 GBK），
# 而 Kconfig 里的中文 menu 标题会被写成 UTF-8 注释存进 sdkconfig，
# 下一轮读取就 UnicodeDecodeError: 'gbk' codec can't decode byte 0xab。
# 强制 Python 走 UTF-8 模式即可根治（3.7+ 支持）。
$env:PYTHONUTF8          = "1"
$env:PYTHONIOENCODING    = "utf-8"

$py    = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
$tools = Join-Path $env:IDF_TOOLS_PATH "tools"

# 工具版本目录随 ESP-IDF 安装版本变化，这里在 tools\<name>\ 下递归定位可执行文件，
# 避免把具体版本号写死（换机器、升级工具链后无需改脚本）。找不到的工具直接跳过。
function Find-ToolBin {
    param([string]$Name, [string]$Exe)
    $dir = Join-Path $tools $Name
    if (-not (Test-Path $dir)) { return $null }
    $hit = Get-ChildItem -Path $dir -Recurse -File -Filter $Exe -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($null -eq $hit) { return $null }
    return $hit.DirectoryName
}

$cmakeDir  = Find-ToolBin "cmake"          "cmake.exe"
$ninjaDir  = Find-ToolBin "ninja"          "ninja.exe"
$ccacheDir = Find-ToolBin "ccache"         "ccache.exe"
$xtensaDir = Find-ToolBin "xtensa-esp-elf" "xtensa-esp32s3-elf-gcc.exe"
$ulpDir    = Find-ToolBin "esp32ulp-elf"   "esp32ulp-elf-ld.exe"
$ocdDir    = Find-ToolBin "openocd-esp32"  "openocd.exe"

# 版本目录本身即根目录的工具（目录下没有可执行文件）
$romDir = if (Test-Path (Join-Path $tools "esp-rom-elfs")) {
    (Get-ChildItem (Join-Path $tools "esp-rom-elfs") -Directory |
     Sort-Object Name -Descending | Select-Object -First 1).FullName
} else { $null }

$env:PATH = (@(
    "$env:IDF_PYTHON_ENV_PATH\Scripts"
    $cmakeDir, $ninjaDir, $ccacheDir, $xtensaDir, $ulpDir, $ocdDir
    $env:PATH
) | Where-Object { $_ }) -join ";"

if ($romDir) { $env:ESP_ROM_ELF_DIR = $romDir }
if ($ocdDir) {
    # openocd.exe 位于 <ver>\openocd-esp32\bin，scripts 在同级 share 目录下
    $env:OPENOCD_SCRIPTS = Join-Path (Split-Path (Split-Path $ocdDir -Parent) -Parent) "openocd-esp32\share\openocd\scripts"
}

# CMake 必须能写临时目录。注意：不要用 build\tmp —— idf.py fullclean 会把整个
# build 目录删掉，下次 cmake 就报 "Cannot create temporary file"。指到系统 temp。
$tmp = Join-Path $env:TEMP "idfbuild"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP  = $tmp
$env:TEMP = $tmp

# 全新 build 时 bootloader 子项目由 ninja 派生 cmd.exe 重新 cmake，那个子进程
# 里的 cmake 找不到 ninja，会报 "CMAKE_MAKE_PROGRAM is not set"。
# 解决方法：先把 bootloader 手动配置好再 build（见下方 PreCfg）。
$root   = $PSScriptRoot
$rootFwd = $root -replace '\\', '/'
$blDir  = "$root\build\bootloader"
$blStamp = "$root\build\bootloader-prefix\src\bootloader-stamp\bootloader-configure"

# 传给 cmake 的 IDF_PATH 必须是【正斜杠】。用反斜杠的话 bootloader 子项目里
# include($ENV{IDF_PATH}/tools/cmake/...) 会拼坏，报
# "Unknown CMake command idf_build_set_property"。
$idfFwd = $env:IDF_PATH -replace '\\', '/'

Set-Location $root

# cmake 由上面的 Find-ToolBin 定位；找不到就退回 PATH 里的 cmake
$cmake = if ($cmakeDir) { Join-Path $cmakeDir "cmake.exe" } else { "cmake.exe" }
if (-not (Test-Path $cmake)) {
    Write-Host "[ERROR] 未找到 cmake.exe，请检查 IDF_TOOLS_PATH: $env:IDF_TOOLS_PATH"
    exit 1
}

# PreCfg：build 目录是全新、或 sdkconfig 被改过的时候，先手动配置 bootloader
# 再补上 stamp（否则 ninja 会自己派发 cmd 去 configure，然后在那里面失败）。
# 仅当 bootloader 子项目尚未配置（如 fullclean 之后 CMakeCache 不存在）时才预配置。
# 不要因 sdkconfig 改了几个字符串就删整个 build\bootloader 目录——那会触发
# 安全删除拦截，而且没必要（bootloader 配置跟 app 层 WiFi 凭据无关）。
$needCfg = $false
if (-not (Test-Path "$blDir\CMakeCache.txt") -or -not (Test-Path $blStamp)) {
    $needCfg = $true
}
if ($needCfg) {
    if (Test-Path $blDir) { Remove-Item $blDir -Recurse -Force }
    if (Test-Path $blStamp) { Remove-Item $blStamp -Force }
    Write-Host "预配置 bootloader 子项目 ..."
    # 注意：-D 参数必须写成带引号的单个字符串，否则 PS5.1 会把
    # -DIDF_PATH=$idfFwd 里的变量当字面量传进去，cmake 收到的是 "$idfFwd/..."
    & $cmake "-DSDKCONFIG=$rootFwd/sdkconfig" "-DIDF_PATH=$idfFwd" "-DIDF_TARGET=esp32s3" `
        "-DPYTHON_DEPS_CHECKED=1" "-DPYTHON=$py" `
        "-DEXTRA_COMPONENT_DIRS=$idfFwd/components/bootloader" `
        "-DPROJECT_SOURCE_DIR=$rootFwd" "-DIGNORE_EXTRA_COMPONENT=" "-GNinja" `
        "-S" "$idfFwd/components/bootloader/subproject" "-B" $blDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    New-Item -ItemType Directory -Force -Path (Split-Path $blStamp) | Out-Null
    New-Item -ItemType File -Path $blStamp -Force | Out-Null
}

Write-Host "IDF_PATH = $env:IDF_PATH"
Write-Host "python   = $py"
& $py "$env:IDF_PATH\tools\idf.py" $ExtraArgs.Split(" ")
exit $LASTEXITCODE

