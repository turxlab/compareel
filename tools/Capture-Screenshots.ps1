# Captures README screenshots of Compareel on Windows into docs-site\assets\.
# Uses an isolated --data-dir per run, drives only the Compareel window via UI Automation,
# and captures only the Compareel window via PrintWindow (never the desktop).

param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'release\v1.1.0\Compareel\Compareel.exe'),
    [string]$Demo = 'C:\programs\compareel-demo',
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'docs-site\assets'),
    [int]$Width = 1280,
    [int]$Height = 800
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "Compareel executable not found at '$Exe'."
}
$photosDir = Join-Path $Demo 'Photos'
$videosDir = Join-Path $Demo 'Videos'
foreach ($dir in @($photosDir, $videosDir)) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        throw "Demo media directory not found at '$dir'."
    }
}
if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

if (-not ('CompareelWin32' -as [type])) {
    Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class CompareelWin32 {
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

[CompareelWin32]::SetProcessDPIAware() | Out-Null

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
        if ($aid -eq 'QApplication.CompareelWindow' -or $cls -match '^UAC|SysShadow|QComboBoxPrivateContainer') {
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
    [CompareelWin32]::ResizeVisibleWindow($hwnd, 120, 80, $Width, $Height)
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
    [CompareelWin32]::ClickScreenPoint(
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
    [CompareelWin32]::ClickScreenPoint(
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

function Save-CompareelScreenshot($Instance, [string]$FileName, [switch]$RequireBothPreviews) {
    $outPath = Join-Path $OutDir $FileName
    $visBounds = [CompareelWin32]::GetVisibleBounds($Instance.Hwnd)

    # Locate left and right VideoCanvas rectangles relative to the visible window if preview check is requested.
    $leftRel = $null
    $rightRel = $null
    if ($RequireBothPreviews) {
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
        if ($rects.Count -ge 2) {
            $sorted = $rects | Sort-Object Left
            $leftRel = $sorted[0]
            $rightRel = $sorted[1]
        }
    }

    $bmp = $null
    for ($attempt = 0; $attempt -lt 25; $attempt++) {
        if ($null -ne $bmp) { $bmp.Dispose(); $bmp = $null }
        $bmp = [CompareelWin32]::CaptureWindow($Instance.Hwnd)
        if (-not $RequireBothPreviews -or $null -eq $leftRel -or $null -eq $rightRel) {
            break
        }
        $lx = [int]($leftRel.Left - $visBounds.Left + 16)
        $ly = [int]($leftRel.Top - $visBounds.Top + 16)
        $lw = [int]([Math]::Max(16, $leftRel.Width - 32))
        $lh = [int]([Math]::Max(16, $leftRel.Height - 32))

        $rx = [int]($rightRel.Left - $visBounds.Left + 16)
        $ry = [int]($rightRel.Top - $visBounds.Top + 16)
        $rw = [int]([Math]::Max(16, $rightRel.Width - 32))
        $rh = [int]([Math]::Max(16, $rightRel.Height - 32))

        $stdL = [CompareelWin32]::MeasureRegionStdDev($bmp, $lx, $ly, $lw, $lh)
        $stdR = [CompareelWin32]::MeasureRegionStdDev($bmp, $rx, $ry, $rw, $rh)
        if ($stdL -ge 12.0 -and $stdR -ge 12.0) {
            break
        }
        Start-Sleep -Milliseconds 250
    }

    try {
        $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
        $item = Get-Item -LiteralPath $outPath
        Write-Host ("Saved {0} ({1}x{2}, {3:N1} KB)" -f $FileName, $bmp.Width, $bmp.Height, ($item.Length / 1KB))
    } finally {
        if ($null -ne $bmp) { $bmp.Dispose() }
    }
}

$totalTimer = [System.Diagnostics.Stopwatch]::StartNew()
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("compareel-screenshots-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null

try {
    # 1. Welcome screenshot: right after start, nothing scanned.
    Write-Host '[1/3] Capturing screenshot-welcome.png...'
    $welcomeData = Join-Path $tempRoot 'welcome'
    New-Item -ItemType Directory -Force -Path $welcomeData | Out-Null
    $app = $null
    try {
        $app = Start-CompareelInstance -DataDir $welcomeData
        $null = Wait-ScanIdle -Instance $app -TimeoutMs 10000
        Start-Sleep -Milliseconds 350
        Save-CompareelScreenshot -Instance $app -FileName 'screenshot-welcome.png'
    } finally {
        Stop-CompareelInstance -Instance $app
    }

    # 2. Results screenshot: Photos in Similar images mode, first group selected so both previews show.
    Write-Host '[2/3] Capturing screenshot-results.png...'
    $resultsData = Join-Path $tempRoot 'results'
    New-Item -ItemType Directory -Force -Path $resultsData | Out-Null
    $app = $null
    try {
        $app = Start-CompareelInstance -DataDir $resultsData -ExtraArgs @($photosDir)
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

        $verdict = Find-ElementByAid $app.Window 'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.pairInfo.pairSimilarity'
        for ($i = 0; $i -lt 40; $i++) {
            if ($null -ne $verdict -and $verdict.Current.Name -notmatch 'Measuring|No pair') { break }
            Start-Sleep -Milliseconds 150
        }
        Start-Sleep -Milliseconds 600
        Save-CompareelScreenshot -Instance $app -FileName 'screenshot-results.png' -RequireBothPreviews
    } finally {
        Stop-CompareelInstance -Instance $app
    }

    # 3. Compare screenshot: video pair from Videos, side by side, paused on a frame where both previews show a picture.
    Write-Host '[3/3] Capturing screenshot-compare.png...'
    $compareData = Join-Path $tempRoot 'compare'
    New-Item -ItemType Directory -Force -Path $compareData | Out-Null
    $app = $null
    try {
        $app = Start-CompareelInstance -DataDir $compareData -ExtraArgs @($videosDir)
        $null = Wait-ScanIdle -Instance $app

        Select-ComboOption -Instance $app `
            -ComboAid 'QApplication.CompareelWindow.QWidget.scanProfile' `
            -OptionName 'Similar videos'
        Invoke-ButtonByAid -Instance $app `
            -ButtonAid 'QApplication.CompareelWindow.QWidget.primaryButton'
        Start-Sleep -Milliseconds 200
        $null = Wait-ScanIdle -Instance $app

        # Select Group 2 (a bright outdoor scene with resolution/encoding differences) so both side-by-side previews
        # and the similarity verdict are clearly demonstrated.
        Invoke-ButtonByAid -Instance $app `
            -ButtonAid 'QApplication.CompareelWindow.QWidget.QSplitter.resultArea.nextGroupButton'
        Start-Sleep -Milliseconds 500

        # Wait for both players to load (non-zero duration on timeLabel), then seek while paused to 35% of duration.
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
            $rv.SetValue(3500)
        }

        $verdict = Find-ElementByAid $app.Window 'QApplication.CompareelWindow.QWidget.QSplitter.QWidget.pairInfo.pairSimilarity'
        for ($i = 0; $i -lt 40; $i++) {
            if ($null -ne $verdict -and $verdict.Current.Name -notmatch 'Measuring|No pair') { break }
            Start-Sleep -Milliseconds 150
        }
        Start-Sleep -Milliseconds 700
        Save-CompareelScreenshot -Instance $app -FileName 'screenshot-compare.png' -RequireBothPreviews
    } finally {
        Stop-CompareelInstance -Instance $app
    }

    Write-Host ("Completed in {0:N1} s." -f $totalTimer.Elapsed.TotalSeconds)
} finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
