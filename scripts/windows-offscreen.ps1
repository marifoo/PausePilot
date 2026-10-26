# Run from an x64 Visual Studio developer shell. The generated plugin is only
# for tests; the portable release uses Qt's unmodified qwindows plugin.
param(
    [Parameter(Mandatory = $true)][string]$QtDir,
    [Parameter(Mandatory = $true)][string]$QtBaseSource
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo 'build/windows-x64/offscreen'
$source = Join-Path $output 'src'
$qt = (Resolve-Path -LiteralPath $QtDir).Path
$upstream = Join-Path (Resolve-Path -LiteralPath $QtBaseSource).Path 'src/plugins/platforms/offscreen'
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Use an x64 Visual Studio developer shell.' }
$version = & (Join-Path $qt 'bin/qmake.exe') -query QT_VERSION
if ($LASTEXITCODE -ne 0 -or $version -ne '5.14.2') {
    throw 'This source correction is for the matching Qt 5.14.2 kit and sources.'
}
$qtVersionHeader = Join-Path $QtBaseSource '.qmake.conf'
if (-not ([IO.File]::ReadAllText($qtVersionHeader).Contains('MODULE_VERSION = 5.14.2'))) {
    throw 'Qt sources must match the 5.14.2 binaries and private headers.'
}
New-Item -ItemType Directory -Force $source | Out-Null
foreach ($file in @('main.cpp', 'qoffscreenintegration.cpp', 'qoffscreenintegration.h',
                    'qoffscreenwindow.cpp', 'qoffscreenwindow.h',
                    'qoffscreencommon.cpp', 'qoffscreencommon.h', 'offscreen.json')) {
    Copy-Item -LiteralPath (Join-Path $upstream $file) -Destination $source -Force
}

# Qt 5.14.2's QMessageBox::showEvent calls nativeResourceForWindow("handle")
# through platformNativeInterface() without checking for null. Offscreen's
# inherited implementation returns null on Windows. Supply the ordinary base
# interface: it correctly returns null for resources that do not exist, so the
# message box proceeds without a Windows system menu and keeps all its normal
# button, escape, accessibility and nested-event-loop behavior.
$header = Join-Path $source 'qoffscreenintegration.h'
$text = [IO.File]::ReadAllText($header)
$anchor = '    void initialize() override;'
if (-not $text.Contains($anchor) -or -not $text.Contains('private:')) {
    throw 'Unexpected Qt offscreen source; correction was not applied.'
}
$text = $text.Replace($anchor,
    '    QPlatformNativeInterface *nativeInterface() const override { return &m_nativeInterface; }' + "`n" + $anchor)
$text = $text.Replace('private:', "private:`n    mutable QPlatformNativeInterface m_nativeInterface;")
[IO.File]::WriteAllText($header, $text)
@'
TEMPLATE = lib
TARGET = qoffscreen
CONFIG += plugin c++11 debug_and_release
CONFIG(debug, debug|release): TARGET = qoffscreend
QT += core-private gui-private eventdispatcher_support-private fontdatabase_support-private
DEFINES += QT_NO_FOREACH
SOURCES = main.cpp qoffscreenintegration.cpp qoffscreenwindow.cpp qoffscreencommon.cpp
HEADERS = qoffscreenintegration.h qoffscreenwindow.h qoffscreencommon.h
DESTDIR = ../plugins/platforms
'@ | Set-Content -LiteralPath (Join-Path $source 'test-offscreen.pro') -Encoding ascii
Push-Location $source
try {
    & (Join-Path $qt 'bin/qmake.exe') test-offscreen.pro
    if ($LASTEXITCODE -ne 0) { throw 'Offscreen qmake failed.' }
    & nmake /NOLOGO release debug
    if ($LASTEXITCODE -ne 0) { throw 'Offscreen build failed.' }
} finally { Pop-Location }
Write-Output "Test plugin built. Set QT_QPA_PLATFORM_PLUGIN_PATH=$output/plugins/platforms"
