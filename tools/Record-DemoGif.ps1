# Records docs-site\assets\demo.gif for the Compareel README on Windows.
# Uses an isolated --data-dir per run, drives only the Compareel window via UI Automation,
# captures only the Compareel window via PrintWindow (never the desktop), and encodes
# a two-pass palette GIF with FFmpeg.

param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'release\v1.1.0\Compareel\Compareel.exe'),
    [string]$Demo = 'C:\programs\compareel-demo',
    [string]$OutFile = (Join-Path (Split-Path -Parent $PSScriptRoot) 'docs-site\assets\demo.gif'),
    [string]$Ffmpeg = (Join-Path (Split-Path -Parent $PSScriptRoot) '.deps\ffmpeg-8.1\bin\ffmpeg.exe'),
    [int]$Width = 1280,
    [int]$Height = 800,
    [int]$GifWidth = 960,
    [double]$Fps = 12.5
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "Compareel executable not found at '$Exe'."
}
if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) {
    throw "FFmpeg executable not found at '$Ffmpeg'."
}
$photosDir = Join-Path $Demo 'Photos'
$videosDir = Join-Path $Demo 'Videos'
foreach ($dir in @($photosDir, $videosDir)) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        throw "Demo media directory not found at '$dir'."
    }
}
$outDir = Split-Path -Parent $OutFile
if (-not (Test-Path -LiteralPath $outDir -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

if (-not ('CompareelGifWin32' -as [type])) {
    Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class CompareelGifWin32 {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
        public int Width { get { return Right - Left; } }
        public int Height { get { return Bottom - Top; } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT {
        public int X;
        public int Y;
    }

    private const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    private const uint PW_RENDERFULLCONTENT = 2;
    private const uint WM_LBUTTONDOWN = 0x0201;
    private const uint WM_LBUTTONUP = 0x0202;

    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool MoveWindow(IntPtr hWnd, int X, int Y, int nWidth, int nHeight, bool bRepaint);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool ScreenToClient(IntPtr hWnd, ref POINT lpPoint);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr ChildWindowFromPointEx(IntPtr hwndParent, POINT pt, uint uFlags);

    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);

    public static RECT GetVisibleBounds(IntPtr hWnd) {
        RECT eb;
        int hr = DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, out eb, Marshal.SizeOf(typeof(RECT)));
        if (hr == 0 && eb.Width > 0 && eb.Height > 0) {
            return eb;
        }
        RECT wr;
        GetWindowRect(hWnd, out wr);
        return wr;
    }

    public static void ResizeVisibleWindow(IntPtr hWnd, int x, int y, int targetWidth, int targetHeight) {
        RECT wr;
        GetWindowRect(hWnd, out wr);
        RECT eb = GetVisibleBounds(hWnd);
        int padW = Math.Max(0, wr.Width - eb.Width);
        int padH = Math.Max(0, wr.Height - eb.Height);
        MoveWindow(hWnd, x, y, targetWidth + padW, targetHeight + padH, true);
    }

    public static void ClickScreenPoint(IntPtr hWnd, int screenX, int screenY) {
        POINT pt = new POINT { X = screenX, Y = screenY };
        ScreenToClient(hWnd, ref pt);
        IntPtr lParam = (IntPtr)((pt.Y << 16) | (pt.X & 0xFFFF));
        PostMessage(hWnd, WM_LBUTTONDOWN, (IntPtr)1, lParam);
        PostMessage(hWnd, WM_LBUTTONUP, IntPtr.Zero, lParam);
    }

    public static Bitmap CaptureWindow(IntPtr hWnd) {
        RECT wr;
        if (!GetWindowRect(hWnd, out wr) || wr.Width <= 0 || wr.Height <= 0) {
            throw new InvalidOperationException("Failed to read Compareel window bounds.");
        }
        RECT eb = GetVisibleBounds(hWnd);
        int cropX = Math.Max(0, eb.Left - wr.Left);
        int cropY = Math.Max(0, eb.Top - wr.Top);
        int cropW = Math.Min(wr.Width - cropX, eb.Width);
        int cropH = Math.Min(wr.Height - cropY, eb.Height);
        if (cropW <= 0 || cropH <= 0) {
            cropX = 0; cropY = 0; cropW = wr.Width; cropH = wr.Height;
        }

        using (Bitmap raw = new Bitmap(wr.Width, wr.Height, PixelFormat.Format32bppRgb)) {
            using (Graphics g = Graphics.FromImage(raw)) {
                IntPtr hdc = g.GetHdc();
                bool ok = false;
                try {
                    ok = PrintWindow(hWnd, hdc, PW_RENDERFULLCONTENT);
                } finally {
                    g.ReleaseHdc(hdc);
                }
                if (!ok) {
                    throw new InvalidOperationException("PrintWindow failed for Compareel window.");
                }
            }
            Bitmap cropped = new Bitmap(cropW, cropH, PixelFormat.Format24bppRgb);
            using (Graphics cg = Graphics.FromImage(cropped)) {
                cg.DrawImage(raw, new Rectangle(0, 0, cropW, cropH), new Rectangle(cropX, cropY, cropW, cropH), GraphicsUnit.Pixel);
            }
            return cropped;
        }
    }

    public static double MeasureRegionStdDev(Bitmap bmp, int x, int y, int w, int h) {
        int x0 = Math.Max(0, x);
        int y0 = Math.Max(0, y);
        int x1 = Math.Min(bmp.Width, x + w);
        int y1 = Math.Min(bmp.Height, y + h);
        if (x1 - x0 < 8 || y1 - y0 < 8) return 0.0;

        double sum = 0.0, sumSq = 0.0;
        int count = 0;
        for (int py = y0; py < y1; py += 4) {
            for (int px = x0; px < x1; px += 4) {
                Color c = bmp.GetPixel(px, py);
                double lum = 0.299 * c.R + 0.587 * c.G + 0.114 * c.B;
                sum += lum;
                sumSq += lum * lum;
                count++;
            }
        }
        if (count < 2) return 0.0;
        double mean = sum / count;
        double variance = Math.Max(0.0, (sumSq / count) - (mean * mean));
        return Math.Sqrt(variance);
    }
}
"@
}

[CompareelGifWin32]::SetProcessDPIAware() | Out-Null

$uiaRoot = [System.Windows.Automation.AutomationElement]::RootElement

function Find-ElementByAid($Parent, [string]$AutomationId) {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $AutomationId)
    return $Parent.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

function Dismiss-ModalDialogs([int]$ProcessId) {
    $pidCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
    $wins = $uiaRoot.FindAll([System.Windows.Automation.TreeScope]::Children, $pidCond)
    foreach ($w in $wins) {
        $aid = $w.Current.AutomationId
        $cls = $w.Current.ClassName
        $title = $w.Current.Name
        if ($aid -eq 'QApplication.CompareelWindow' -or $cls -match '^UAC|SysShadow|QComboBoxPrivateContainer|QMenu') {
            continue
        }
        if ($title -match 'update|Compareel' -or $cls -match 'QMessageBox|QDialog') {
            foreach ($btnText in @('No', '&No', 'Later', 'Cancel', 'Close')) {
                $btnCond = New-Object System.Windows.Automation.AndCondition(
                    (New-Object System.Windows.Automation.PropertyCondition(
                        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                        [System.Windows.Automation.ControlType]::Button)),
                    (New-Object System.Windows.Automation.PropertyCondition(
                        [System.Windows.Automation.AutomationElement]::NameProperty, $btnText))
                )
                $btn = $w.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $btnCond)
                if ($null -ne $btn) {
                    try {
                        $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                        Start-Sleep -Milliseconds 150
                        break
                    } catch {}
                }
            }
        }
    }
}

function Start-CompareelInstance([string]$DataDir, [string[]]$ExtraArgs = @()) {
    $argList = @('--data-dir', $DataDir) + $ExtraArgs
    $quoted = $argList | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    $proc = Start-Process -FilePath $Exe -ArgumentList $quoted -PassThru

    $winCond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)),
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'QApplication.CompareelWindow'))
    )

    $win = $null
    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Milliseconds 125
        Dismiss-ModalDialogs -ProcessId $proc.Id
        $win = $uiaRoot.FindFirst([System.Windows.Automation.TreeScope]::Children, $winCond)
        if ($null -ne $win -and $win.Current.NativeWindowHandle -ne 0) {
            break
        }
    }
    if ($null -eq $win) {
        if (-not $proc.HasExited) { $proc.Kill() }
        throw "Timed out waiting for Compareel main window."
    }

    $hwnd = [IntPtr]$win.Current.NativeWindowHandle
    Start-Sleep -Milliseconds 250
    [CompareelGifWin32]::ResizeVisibleWindow($hwnd, 120, 80, $Width, $Height)
    Start-Sleep -Milliseconds 250

    return [PSCustomObject]@{
        Process = $proc
        Window  = $win
        Hwnd    = $hwnd
    }
}

function Stop-CompareelInstance($Instance) {
    if ($null -eq $Instance) { return }
    try {
        Dismiss-ModalDialogs -ProcessId $Instance.Process.Id
        if ($null -ne $Instance.Window -and -not $Instance.Process.HasExited) {
            $wp = $Instance.Window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
            $wp.Close()
        }
    } catch {}
    if ($null -ne $Instance.Process -and -not $Instance.Process.HasExited) {
        if (-not $Instance.Process.WaitForExit(8000)) {
            $Instance.Process.Kill()
            $null = $Instance.Process.WaitForExit(3000)
        }
    }
}

function Wait-ScanIdle($Instance, [int]$TimeoutMs = 45000) {
    $sb = Find-ElementByAid $Instance.Window 'QApplication.CompareelWindow.QStatusBar'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        Dismiss-ModalDialogs -ProcessId $Instance.Process.Id
        $msg = if ($null -ne $sb) { $sb.Current.Name } else { '' }
        if ($msg -match '^(Ready|No matches found|No files)') {
            return $msg
        }
        Start-Sleep -Milliseconds 120
    }
    throw "Timed out waiting for Compareel scan to finish (last status: '$($sb.Current.Name)')."
}

function Select-ComboOption($Instance, [string]$ComboAid, [string]$OptionName) {
    $combo = Find-ElementByAid $Instance.Window $ComboAid
    if ($null -eq $combo) {
        throw "ComboBox '$ComboAid' not found."
    }
    $vp = $combo.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    if ($vp.Current.Value -eq $OptionName) { return }

    $rect = $combo.Current.BoundingRectangle
    [CompareelGifWin32]::ClickScreenPoint(
        $Instance.Hwnd,
        [int]($rect.Left + $rect.Width / 2),
        [int]($rect.Top + $rect.Height / 2)
    )

    $popCond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $Instance.Process.Id)),
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'QComboBoxPrivateContainer'))
    )
    $popup = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 60
        $popup = $uiaRoot.FindFirst([System.Windows.Automation.TreeScope]::Children, $popCond)
        if ($null -ne $popup) { break }
    }
    if ($null -eq $popup) {
        throw "Popup container did not open for '$ComboAid'."
    }

    $nameCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $OptionName)
    $item = $Instance.Window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $nameCond)
    if ($null -eq $item) {
        $item = $popup.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $nameCond)
    }
    if ($null -eq $item) {
        throw "Combo option '$OptionName' not found in '$ComboAid'."
    }

    $itemRect = $item.Current.BoundingRectangle
    [CompareelGifWin32]::ClickScreenPoint(
        [IntPtr]$popup.Current.NativeWindowHandle,
        [int]($itemRect.Left + $itemRect.Width / 2),
        [int]($itemRect.Top + $itemRect.Height / 2)
    )
    Start-Sleep -Milliseconds 180
}

function Invoke-ButtonByAid($Instance, [string]$ButtonAid) {
    $btn = Find-ElementByAid $Instance.Window $ButtonAid
    if ($null -eq $btn) {
        throw "Button '$ButtonAid' not found."
    }
    $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Invoke-ButtonByName($Instance, [string]$ButtonName) {
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)),
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, $ButtonName))
    )
    $btn = $Instance.Window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
    if ($null -eq $btn) {
        throw "Button named '$ButtonName' not found."
    }
    $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Set-ComparisonView($Instance, [string]$ModeName) {
    $pbCond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::MenuItem)),
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, 'Playback'))
    )
    $pb = $Instance.Window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $pbCond)
    if ($null -eq $pb) { throw "Playback menu item not found." }

    $pbEc = $pb.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $pbEc.Expand()
    Start-Sleep -Milliseconds 120

    $cv = $Instance.Window.FindFirst(
        [System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, 'Comparison view'))
    )
    if ($null -eq $cv) { throw "Comparison view submenu not found." }

    $cvEc = $cv.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $cvEc.Expand()
    Start-Sleep -Milliseconds 120

    $modeItem = $Instance.Window.FindFirst(
        [System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, $ModeName))
    )
    if ($null -eq $modeItem) { throw "Comparison view mode '$ModeName' not found." }

    $modeItem.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Milliseconds 80

    try {
        if ($cvEc.Current.ExpandCollapseState -ne [System.Windows.Automation.ExpandCollapseState]::Collapsed) {
            $cvEc.Collapse()
        }
    } catch {}
    try {
        if ($pbEc.Current.ExpandCollapseState -ne [System.Windows.Automation.ExpandCollapseState]::Collapsed) {
            $pbEc.Collapse()
        }
    } catch {}
    Start-Sleep -Milliseconds 120
}

function Get-CanvasRects($Instance) {
    $canvases = $Instance.Window.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
            'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.pairArea.playerPanel.QWidget'))
    )
    $rects = @()
    foreach ($c in $canvases) {
        if (-not $c.Current.IsOffscreen -and $c.Current.Name -match '^(Left|Right)\b') {
            $rects += $c.Current.BoundingRectangle
        }
    }
    if ($rects.Count -lt 2) { return $null }
    $sorted = $rects | Sort-Object Left
    return [PSCustomObject]@{
        Left  = $sorted[0]
        Right = $sorted[1]
    }
}

function Set-WipeRatio($Instance, [double]$Ratio) {
    $rects = Get-CanvasRects -Instance $Instance
    if ($null -eq $rects) { return }
    $r = $rects.Right
    $clamped = [Math]::Max(0.05, [Math]::Min(0.95, $Ratio))
    $sx = [int]($r.Left + $r.Width * $clamped)
    $sy = [int]($r.Top + $r.Height * 0.5)
    [CompareelGifWin32]::ClickScreenPoint($Instance.Hwnd, $sx, $sy)
}

function Wait-PairReady($Instance, [int]$SettleMs = 200) {
    $verdict = Find-ElementByAid $Instance.Window 'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.pairInfo.pairSimilarity'
    for ($i = 0; $i -lt 50; $i++) {
        if ($null -ne $verdict -and $verdict.Current.Name -notmatch 'Measuring|No pair') { break }
        Start-Sleep -Milliseconds 100
    }

    $visBounds = [CompareelGifWin32]::GetVisibleBounds($Instance.Hwnd)
    $rects = Get-CanvasRects -Instance $Instance
    if ($null -ne $rects) {
        for ($attempt = 0; $attempt -lt 30; $attempt++) {
            $bmp = [CompareelGifWin32]::CaptureWindow($Instance.Hwnd)
            try {
                $lx = [int]($rects.Left.Left - $visBounds.Left + 16)
                $ly = [int]($rects.Left.Top - $visBounds.Top + 16)
                $lw = [int]([Math]::Max(16, $rects.Left.Width - 32))
                $lh = [int]([Math]::Max(16, $rects.Left.Height - 32))

                $rx = [int]($rects.Right.Left - $visBounds.Left + 16)
                $ry = [int]($rects.Right.Top - $visBounds.Top + 16)
                $rw = [int]([Math]::Max(16, $rects.Right.Width - 32))
                $rh = [int]([Math]::Max(16, $rects.Right.Height - 32))

                $stdL = [CompareelGifWin32]::MeasureRegionStdDev($bmp, $lx, $ly, $lw, $lh)
                $stdR = [CompareelGifWin32]::MeasureRegionStdDev($bmp, $rx, $ry, $rw, $rh)
                if ($stdL -ge 12.0 -and $stdR -ge 12.0) {
                    break
                }
            } finally {
                $bmp.Dispose()
            }
            Start-Sleep -Milliseconds 150
        }
    }

    if ($SettleMs -gt 0) {
        Start-Sleep -Milliseconds $SettleMs
    }
}

$script:frameIndex = 0

function Save-FrameBitmap([System.Drawing.Bitmap]$Bitmap, [string]$FramesDir, [int]$RepeatCount = 1) {
    $firstPath = Join-Path $FramesDir ('frame_{0:D5}.png' -f $script:frameIndex)
    $Bitmap.Save($firstPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $script:frameIndex++
    for ($r = 1; $r -lt $RepeatCount; $r++) {
        $dupPath = Join-Path $FramesDir ('frame_{0:D5}.png' -f $script:frameIndex)
        Copy-Item -LiteralPath $firstPath -Destination $dupPath
        $script:frameIndex++
    }
}

function Add-HoldFrames($Instance, [string]$FramesDir, [double]$Seconds) {
    $count = [Math]::Max(1, [int][Math]::Round($Seconds * $Fps))
    $bmp = [CompareelGifWin32]::CaptureWindow($Instance.Hwnd)
    try {
        Save-FrameBitmap -Bitmap $bmp -FramesDir $FramesDir -RepeatCount $count
    } finally {
        $bmp.Dispose()
    }
}

function Capture-RealtimeBitmaps($Instance, [double]$Seconds) {
    $count = [Math]::Max(1, [int][Math]::Round($Seconds * $Fps))
    $intervalMs = 1000.0 / $Fps
    $captured = New-Object 'System.Collections.Generic.List[System.Drawing.Bitmap]' $count
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $count; $i++) {
        $targetMs = $i * $intervalMs
        $remaining = $targetMs - $sw.Elapsed.TotalMilliseconds
        if ($remaining -gt 2.0) {
            Start-Sleep -Milliseconds ([int][Math]::Floor($remaining))
        }
        while ($sw.Elapsed.TotalMilliseconds -lt $targetMs) {
            [System.Threading.Thread]::SpinWait(200)
        }
        $captured.Add([CompareelGifWin32]::CaptureWindow($Instance.Hwnd))
    }
    return ,$captured
}

function Save-CapturedBitmaps($Bitmaps, [string]$FramesDir) {
    try {
        foreach ($bmp in $Bitmaps) {
            Save-FrameBitmap -Bitmap $bmp -FramesDir $FramesDir -RepeatCount 1
        }
    } finally {
        foreach ($bmp in $Bitmaps) {
            if ($null -ne $bmp) { $bmp.Dispose() }
        }
    }
}

$totalTimer = [System.Diagnostics.Stopwatch]::StartNew()
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("compareel-demogif-" + [guid]::NewGuid().ToString('N'))
$framesDir = Join-Path $tempRoot 'frames'
New-Item -ItemType Directory -Force -Path $framesDir | Out-Null

try {
    # Part 1: Similar images scan of Photos, stepping pairs and demonstrating Wipe view.
    Write-Host '[1/3] Recording Similar images workflow on Photos...'
    $photosData = Join-Path $tempRoot 'photos-data'
    New-Item -ItemType Directory -Force -Path $photosData | Out-Null
    $app = $null
    try {
        $app = Start-CompareelInstance -DataDir $photosData -ExtraArgs @($photosDir)
        $null = Wait-ScanIdle -Instance $app
        Select-ComboOption -Instance $app `
            -ComboAid 'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.emptyState.emptyCard.emptyMediaMode' `
            -OptionName 'Images'
        Select-ComboOption -Instance $app `
            -ComboAid 'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.emptyState.emptyCard.emptyScanProfile' `
            -OptionName 'Similar images'
        Invoke-ButtonByAid -Instance $app `
            -ButtonAid 'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.emptyState.emptyCard.emptyScanButton'
        Start-Sleep -Milliseconds 200
        $null = Wait-ScanIdle -Instance $app

        # Pair 1: Spiral Galaxy Shared.png vs IMG_20250315_221409.jpg (PNG vs JPEG)
        Wait-PairReady -Instance $app -SettleMs 500
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 1.8

        # Pair 2: IMG_20250709_101533.jpg vs Wildlife Bear Blurred.jpg (Candidate 1/2)
        Invoke-ButtonByName -Instance $app -ButtonName 'Next pair'
        Wait-PairReady -Instance $app -SettleMs 250
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 1.6

        # Switch to Wipe comparison view on the bear pair, sweep gently across the image, then hold
        Set-ComparisonView -Instance $app -ModeName 'Wipe'
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 0.8
        foreach ($ratio in @(0.46, 0.41, 0.36, 0.42, 0.48, 0.55, 0.62)) {
            Set-WipeRatio -Instance $app -Ratio $ratio
            Start-Sleep -Milliseconds 50
            $bmp = [CompareelGifWin32]::CaptureWindow($app.Hwnd)
            try {
                Save-FrameBitmap -Bitmap $bmp -FramesDir $framesDir -RepeatCount 1
            } finally {
                $bmp.Dispose()
            }
        }
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 0.9

        # Switch back to Side by side
        Set-ComparisonView -Instance $app -ModeName 'Side by side'
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 1.4

        # Pair 3: IMG_20250709_101533.jpg vs Wildlife Bear Small.jpg (Candidate 2/2, downscaled)
        Invoke-ButtonByName -Instance $app -ButtonName 'Next pair'
        Wait-PairReady -Instance $app -SettleMs 250
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 1.6

        # Pair 4: IMG_20250618_075130.jpg vs Mountain River Cropped.jpg (Group 3, cropped landscape)
        Invoke-ButtonByName -Instance $app -ButtonName 'Next pair'
        Wait-PairReady -Instance $app -SettleMs 250
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 1.6
    } finally {
        Stop-CompareelInstance -Instance $app
    }

    # Part 2: Similar videos scan of Videos and synchronized linked playback.
    Write-Host '[2/3] Recording Similar videos workflow and linked playback on Videos...'
    $videosData = Join-Path $tempRoot 'videos-data'
    New-Item -ItemType Directory -Force -Path $videosData | Out-Null
    $app = $null
    try {
        $app = Start-CompareelInstance -DataDir $videosData -ExtraArgs @($videosDir)
        $null = Wait-ScanIdle -Instance $app
        Select-ComboOption -Instance $app `
            -ComboAid 'QApplication.CompareelWindow.QWidget.scanProfile' `
            -OptionName 'Similar videos'
        Invoke-ButtonByAid -Instance $app `
            -ButtonAid 'QApplication.CompareelWindow.QWidget.primaryButton'
        Start-Sleep -Milliseconds 200
        $null = Wait-ScanIdle -Instance $app

        # Select Group 2 (VID_20250412_104218.mp4 color vs Meadow Grayscale.mp4 grayscale)
        Invoke-ButtonByAid -Instance $app `
            -ButtonAid 'QApplication.CompareelWindow.QWidget.QSplitter.resultArea.nextGroupButton'
        Start-Sleep -Milliseconds 400

        $timeCond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
            'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.pairArea.playerPanel.playerTransport.timeLabel')
        for ($i = 0; $i -lt 40; $i++) {
            $times = $app.Window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $timeCond)
            if ($times.Count -ge 2 -and $times[0].Current.Name -notmatch '/ 00:00\.0$' -and $times[1].Current.Name -notmatch '/ 00:00\.0$') {
                break
            }
            Start-Sleep -Milliseconds 150
        }

        $seekSlider = Find-ElementByAid $app.Window 'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.pairArea.playerPanel.playerTransport.playerSeek'
        if ($null -ne $seekSlider) {
            $rv = $seekSlider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
            $rv.SetValue(3450)
        }

        Wait-PairReady -Instance $app -SettleMs 500
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 1.6

        $playCond = New-Object System.Windows.Automation.AndCondition(
            (New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::Button)),
            (New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::NameProperty, 'Play / pause'))
        )
        $playBtn = $app.Window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $playCond)
        if ($null -eq $playBtn) { throw "Button named 'Play / pause' not found." }
        $playRect = $playBtn.Current.BoundingRectangle
        $playX = [int]($playRect.Left + $playRect.Width / 2)
        $playY = [int]($playRect.Top + $playRect.Height / 2)

        # Play both linked videos in step for ~3.2 seconds, pause immediately via PostMessage, then flush frames
        [CompareelGifWin32]::ClickScreenPoint($app.Hwnd, $playX, $playY)
        $liveBitmaps = Capture-RealtimeBitmaps -Instance $app -Seconds 3.2
        try {
            [CompareelGifWin32]::ClickScreenPoint($app.Hwnd, $playX, $playY)
        } finally {
            Save-CapturedBitmaps -Bitmaps $liveBitmaps -FramesDir $framesDir
        }
        Start-Sleep -Milliseconds 120
        Add-HoldFrames -Instance $app -FramesDir $framesDir -Seconds 1.5
    } finally {
        Stop-CompareelInstance -Instance $app
    }

    # Part 3: Two-pass FFmpeg palette generation and GIF encoding.
    Write-Host ("[3/3] Encoding {0} frames at {1} fps to {2}..." -f $script:frameIndex, $Fps, $OutFile)
    $framePattern = Join-Path $framesDir 'frame_%05d.png'
    $palettePath = Join-Path $tempRoot 'palette.png'
    $fpsArg = [string]::Format([System.Globalization.CultureInfo]::InvariantCulture, '{0}', $Fps)

    & $Ffmpeg -y -hide_banner -loglevel error `
        -framerate $fpsArg -i $framePattern `
        -vf "scale=${GifWidth}:-1:flags=lanczos,palettegen=max_colors=256:stats_mode=full" `
        $palettePath
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg palettegen failed with exit code $LASTEXITCODE."
    }

    & $Ffmpeg -y -hide_banner -loglevel error `
        -framerate $fpsArg -i $framePattern -i $palettePath `
        -lavfi "scale=${GifWidth}:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=sierra2_4a:diff_mode=rectangle" `
        -loop 0 $OutFile
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg paletteuse failed with exit code $LASTEXITCODE."
    }

    $outItem = Get-Item -LiteralPath $OutFile
    $durationSec = $script:frameIndex / $Fps
    Write-Host ("Saved {0} ({1:N2} MB, ~{2:N1} s, {3} frames @ {4} fps) in {5:N1} s." -f `
        $OutFile, ($outItem.Length / 1MB), $durationSec, $script:frameIndex, $Fps, $totalTimer.Elapsed.TotalSeconds)
} finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
