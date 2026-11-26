param([string]$Path, [string]$Out)

# Windows.Media.Ocr 取字。
# ZPIX_OCR_VERSION=4
#
# 关键点（按踩过的坑排序）：
#   1) 单行小图（如 425x55）原样喂给引擎经常返回空或乱码，必须放大 + 白边补边；
#   2) 本机只装了 zh-Hans-CN 这一个识别引擎，它的拉丁字母模型很弱：重度展示字体
#      （Inter Black 之类）里 "rt" 这种粘连字会被它当成汉字（戕/俨/酽）或者直接丢掉 ——
#      实测 "Backdoor Part 2" 只能读到 "Backdoor Pa 2"。把"词间空隙加宽 + 纵向拉伸 1.6 倍"
#      再喂给引擎，它就能正确切成 Part；字与字之间的空隙不能动（动了反而更糟）。
#   3) 打分必须按语言算，不能只数有效字符数，否则 "Bac d00 俨 Pa 2" 会和正确结果打平。
#   4) 深色背景文字：整体反色后再走同一套流程。
#   5) 中文引擎会在每个字之间插空格，输出前要清掉。
#   6) 图像处理优先走编译版（快，逐像素在 C# 里循环）；编译不了（比如 %TEMP% 不可写）
#      就退回纯 PowerShell 实现，效果一致只是慢一些 —— 绝不能因为编译失败就退化成
#      "只放大不切词"，那正是识别出 "Pa" 的原因。

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

[Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime] | Out-Null
[Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics.Imaging, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Ocr.OcrResult, Windows.Foundation, ContentType = WindowsRuntime] | Out-Null
[Windows.Globalization.Language, Windows.Globalization, ContentType = WindowsRuntime] | Out-Null

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

function Await($op, $type) {
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $t.Wait(-1) | Out-Null
    return $t.Result
}

function Write-Out([string]$text) {
    [IO.File]::WriteAllText($Out, $text, [Text.Encoding]::UTF8)
}

if (-not (Test-Path -LiteralPath $Path)) { Write-Out ''; exit 2 }

# ---------------------------------------------------------------- 图像处理（编译版）
$csSrc = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class ZpixOcrImg {
    // 每列"墨量"（暗像素数）。invert=true 时统计亮像素（深色背景）。
    public static int[] InkColumns(Bitmap b, bool invert) {
        int w = b.Width, h = b.Height;
        var ink = new int[w];
        var bd = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        var buf = new byte[bd.Stride * h];
        Marshal.Copy(bd.Scan0, buf, 0, buf.Length);
        b.UnlockBits(bd);
        for (int x = 0; x < w; x++) {
            int c = 0;
            for (int y = 0; y < h; y++) {
                int o = y * bd.Stride + x * 3;
                int v = (buf[o + 2] * 299 + buf[o + 1] * 587 + buf[o] * 114) / 1000;
                if (invert ? v > 128 : v < 128) c++;
            }
            ink[x] = c;
        }
        return ink;
    }

    // 整图反色（深色背景 -> 白底黑字）
    public static Bitmap Invert(Bitmap src) {
        var attr = new ImageAttributes();
        var cm = new ColorMatrix();
        cm.Matrix00 = -1; cm.Matrix11 = -1; cm.Matrix22 = -1; cm.Matrix33 = 1;
        cm.Matrix40 = 1; cm.Matrix41 = 1; cm.Matrix42 = 1;
        attr.SetColorMatrix(cm);
        var dst = new Bitmap(src.Width, src.Height, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(dst)) {
            g.Clear(Color.Black);
            g.DrawImage(src, new Rectangle(0, 0, src.Width, src.Height), 0, 0, src.Width, src.Height,
                        GraphicsUnit.Pixel, attr);
        }
        attr.Dispose();
        return dst;
    }

    // 在每个词间空隙处插入 extra 个空白列（只动词与词之间，字与字之间不动）
    public static Bitmap Widen(Bitmap src, int[] gapStarts, int extra) {
        int w = src.Width, h = src.Height;
        if (extra <= 0 || gapStarts.Length == 0) return src;
        int nw = w + gapStarts.Length * extra;
        var dst = new Bitmap(nw, h, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(dst)) {
            g.Clear(Color.White);
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
            int sx = 0, dx = 0;
            foreach (int gs in gapStarts) {
                int segw = gs - sx;
                if (segw > 0)
                    g.DrawImage(src, new Rectangle(dx, 0, segw, h), new Rectangle(sx, 0, segw, h),
                                GraphicsUnit.Pixel);
                dx += segw + extra;
                sx = gs;
            }
            if (sx < w) {
                int segw = w - sx;
                g.DrawImage(src, new Rectangle(dx, 0, segw, h), new Rectangle(sx, 0, segw, h),
                            GraphicsUnit.Pixel);
            }
        }
        return dst;
    }

    // 缩放 + 纵向拉伸 + 补白边
    public static Bitmap Render(Bitmap src, double scale, double stretchY, int pad) {
        int tw = (int)Math.Max(1, Math.Round(src.Width * scale));
        int th = (int)Math.Max(1, Math.Round(src.Height * scale * stretchY));
        int maxDim = 4000;
        if (tw + pad * 2 > maxDim || th + pad * 2 > maxDim) {
            double k = Math.Min((maxDim - pad * 2) / (double)tw, (maxDim - pad * 2) / (double)th);
            tw = (int)Math.Max(1, tw * k);
            th = (int)Math.Max(1, th * k);
        }
        var dst = new Bitmap(tw + pad * 2, th + pad * 2, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(dst)) {
            g.Clear(Color.White);
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
            g.DrawImage(src, new Rectangle(pad, pad, tw, th));
        }
        return dst;
    }
}
'@

# 编译失败的两个常见原因：%TEMP% 不可写、宿主没有 C# 编译器。
# 先把临时目录指到输出目录（一定是可写的），再退回默认。
$imgOk = $false
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$tryDirs = @()
if ($Out) { $tryDirs += (Split-Path -Parent $Out) }
$tryDirs += $oldTemp
foreach ($d in $tryDirs) {
    if ($d -and (Test-Path -LiteralPath $d)) { $env:TEMP = $d; $env:TMP = $d }
    try {
        Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition $csSrc -ErrorAction Stop
        $imgOk = $true
        break
    } catch {
        $imgOk = $false
    }
}
$env:TEMP = $oldTemp
$env:TMP = $oldTmp

# ---------------------------------------------------------------- 纯 PowerShell 后备实现
function Ps-Invert($bmp) {
    $attr = New-Object System.Drawing.Imaging.ImageAttributes
    $cm = New-Object System.Drawing.Imaging.ColorMatrix
    $cm.Matrix00 = -1; $cm.Matrix11 = -1; $cm.Matrix22 = -1; $cm.Matrix33 = 1
    $cm.Matrix40 = 1; $cm.Matrix41 = 1; $cm.Matrix42 = 1
    $attr.SetColorMatrix($cm)
    $dst = New-Object System.Drawing.Bitmap($bmp.Width, $bmp.Height)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.Clear([System.Drawing.Color]::Black)
    $g.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)), 0, 0,
        $bmp.Width, $bmp.Height, [System.Drawing.GraphicsUnit]::Pixel, $attr)
    $g.Dispose(); $attr.Dispose()
    return $dst
}

function Ps-InkColumns($bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    # 纯 PS 逐像素很慢，图太大就放弃（大图通常是整段文字，不需要切词）
    if (($w * $h) -gt 200000) { return $null }
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $stride = $bd.Stride
    $buf = [byte[]]::new($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($bd)
    $ink = [int[]]::new($w)
    for ($x = 0; $x -lt $w; $x++) {
        $c = 0
        for ($y = 0; $y -lt $h; $y++) {
            $o = $y * $stride + $x * 3
            $v = ([int]$buf[$o + 2] * 299 + [int]$buf[$o + 1] * 587 + [int]$buf[$o] * 114) / 1000
            if ($v -lt 128) { $c++ }
        }
        $ink[$x] = $c
    }
    return $ink
}

function Ps-Widen($bmp, $gaps, [int]$extra) {
    if ($extra -le 0 -or $gaps.Count -eq 0) { return $bmp }
    $w = $bmp.Width; $h = $bmp.Height
    $dst = New-Object System.Drawing.Bitmap(($w + $gaps.Count * $extra), $h)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.Clear([System.Drawing.Color]::White)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $sx = 0; $dx = 0
    foreach ($gs in $gaps) {
        $segw = $gs - $sx
        if ($segw -gt 0) {
            $g.DrawImage($bmp, (New-Object System.Drawing.Rectangle($dx, 0, $segw, $h)),
                (New-Object System.Drawing.Rectangle($sx, 0, $segw, $h)),
                [System.Drawing.GraphicsUnit]::Pixel)
        }
        $dx += $segw + $extra
        $sx = $gs
    }
    if ($sx -lt $w) {
        $segw = $w - $sx
        $g.DrawImage($bmp, (New-Object System.Drawing.Rectangle($dx, 0, $segw, $h)),
            (New-Object System.Drawing.Rectangle($sx, 0, $segw, $h)),
            [System.Drawing.GraphicsUnit]::Pixel)
    }
    $g.Dispose()
    return $dst
}

function Ps-Render($bmp, [double]$scale, [double]$stretchY, [int]$pad) {
    $tw = [int][Math]::Max(1, [Math]::Round($bmp.Width * $scale))
    $th = [int][Math]::Max(1, [Math]::Round($bmp.Height * $scale * $stretchY))
    $maxDim = 4000
    if (($tw + $pad * 2) -gt $maxDim -or ($th + $pad * 2) -gt $maxDim) {
        $k = [Math]::Min(($maxDim - $pad * 2) / $tw, ($maxDim - $pad * 2) / $th)
        $tw = [int][Math]::Max(1, $tw * $k)
        $th = [int][Math]::Max(1, $th * $k)
    }
    $dst = New-Object System.Drawing.Bitmap(($tw + $pad * 2), ($th + $pad * 2))
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.Clear([System.Drawing.Color]::White)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($bmp, (New-Object System.Drawing.Rectangle($pad, $pad, $tw, $th)))
    $g.Dispose()
    return $dst
}

# ---------------------------------------------------------------- 识别引擎
$engines = @()
foreach ($tag in @('zh-Hans-CN', 'zh-Hans', 'zh-CN', 'en-US', 'en')) {
    try {
        $lang = New-Object Windows.Globalization.Language $tag
        $e = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($lang)
        if ($e) {
            if (-not ($engines | Where-Object { $_[0] -eq $tag })) { $engines += , @($tag, $e) }
        }
    } catch { }
}
if ($engines.Count -eq 0) {
    $e = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    if ($e) { $engines += , @('user', $e) }
}
if ($engines.Count -eq 0) { Write-Out ''; exit 3 }

# 最多两个引擎：英文类优先（拉丁字母准），中文类兜底
$pick = @()
$en = $engines | Where-Object { $_[0] -like 'en*' } | Select-Object -First 1
$zh = $engines | Where-Object { $_[0] -like 'zh*' } | Select-Object -First 1
if ($en) { $pick += , $en }
if ($zh) { $pick += , $zh }
if ($pick.Count -eq 0) { $pick = $engines | Select-Object -First 1 }

try { $src = [System.Drawing.Bitmap]::FromFile($Path) } catch { Write-Out ''; exit 4 }

# ---------------------------------------------------------------- 单次识别
function Ocr-Bitmap($engine, $bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $ms.Position = 0
    $ras = [System.IO.WindowsRuntimeStreamExtensions]::AsRandomAccessStream($ms)
    $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($ras)) ([Windows.Graphics.Imaging.BitmapDecoder])
    $sb = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
    $res = Await ($engine.RecognizeAsync($sb)) ([Windows.Media.Ocr.OcrResult])
    $lines = @()
    foreach ($l in $res.Lines) { $lines += $l.Text }
    $ras.Dispose(); $ms.Dispose()
    return ($lines -join "`r`n")
}

# ---------------------------------------------------------------- 语言感知打分
function Score-Text([string]$t) {
    if (-not $t) { return 0.0 }
    $latin = ([regex]::Matches($t, '[A-Za-z]')).Count
    $cjk = ([regex]::Matches($t, '[\u3400-\u4dbf\u4e00-\u9fff]')).Count
    $words = ([regex]::Matches($t, '\b[A-Za-z]{2,}\b')).Count
    # 形似乱码的 token：字母和数字糊在一起（d00 / 00r / Backd00r），或者孤立单字母
    $junk = ([regex]::Matches($t, '\b[A-Za-z]+\d|\d+[A-Za-z]')).Count
    $lone = ([regex]::Matches($t, '(^|\s)[A-Za-z](\s|$)')).Count
    $spaces = ([regex]::Matches($t, ' ')).Count
    return [double]($latin + 1.2 * $cjk + 2.5 * $words - 3.0 * $junk - 1.0 * $lone + 0.1 * $spaces)
}

# ---------------------------------------------------------------- 预处理准备
$norm = $src
$imgH = $src.Height
$imgW = $src.Width
$gapStarts = @()
$darkBg = $false
try {
    $p1 = $src.GetPixel(2, 2); $p2 = $src.GetPixel($src.Width - 3, 2)
    $p3 = $src.GetPixel(2, $src.Height - 3); $p4 = $src.GetPixel($src.Width - 3, $src.Height - 3)
    $lum = (($p1.R + $p2.R + $p3.R + $p4.R) + ($p1.G + $p2.G + $p3.G + $p4.G) + ($p1.B + $p2.B + $p3.B + $p4.B)) / 12
    $darkBg = ($lum -lt 110)
} catch { }

if ($darkBg) {
    if ($imgOk) { $norm = [ZpixOcrImg]::Invert($src) } else { $norm = Ps-Invert $src }
}

$ink = $null
if ($imgOk) { $ink = [ZpixOcrImg]::InkColumns($norm, $false) } else { $ink = Ps-InkColumns $norm }
if ($ink) {
    # 词间空隙：连续空列 >= max(4, 12% 图高)，且不贴左右边缘（那是留白不是空格）
    $minGap = [int][Math]::Max(4, [Math]::Round($imgH * 0.12))
    $runStart = -1
    for ($x = 0; $x -lt $imgW; $x++) {
        if ($ink[$x] -eq 0) {
            if ($runStart -lt 0) { $runStart = $x }
        } else {
            if ($runStart -ge 0) {
                $len = $x - $runStart
                if ($len -ge $minGap -and $runStart -gt 0 -and $len -lt ($imgW * 0.35)) {
                    $gapStarts += $runStart
                }
                $runStart = -1
            }
        }
    }
}

# ---------------------------------------------------------------- 候选组合
$baseScale = 3
if ($imgH -lt 24) { $baseScale = 8 }
elseif ($imgH -lt 40) { $baseScale = 6 }
elseif ($imgH -lt 80) { $baseScale = 4 }
# 整屏截图那种大图别再放大到几千像素：既慢（超时了就整段没结果）也没必要，
# 大图里的字本来就已经足够大。
while ($baseScale -gt 1 -and (($imgW * $baseScale) * ($imgH * $baseScale * 1.6)) -gt 12000000) { $baseScale-- }

# (scale, stretchY, 词间空隙加宽, 留白系数)
# 前两条是救粘连字（展示字体），中间几条救小字号的普通 UI 文本（"to" 被读成 "t0"
# 那种要靠 base+2 倍纯放大才能读对），最后一条跟老逻辑等价做兜底。
$variants = @(
    @($baseScale, 1.6, 8, 0.45),
    @(($baseScale + 2), 1.0, 0, 0.45),
    @($baseScale, 1.0, 0, 0.10),
    @(3, 1.6, 8, 0.45),
    @(($baseScale + 2), 1.6, 8, 0.45),
    @(6, 1.6, 14, 0.45),
    @($baseScale, 1.0, 0, 0.45)
)

# 大图只跑前几条（大图里不存在"小字号"的问题，多跑只是浪费时间）
$useVariants = $variants
if (($imgW * $imgH) -gt 400000) { $useVariants = $variants[0..2] }

$results = @()
foreach ($pair in $pick) {
    $engine = $pair[1]
    foreach ($v in $useVariants) {
        $scale = [int]$v[0]
        $stretch = [double]$v[1]
        $extra = [int]$v[2]
        $padKoef = [double]$v[3]
        $bmp = $null
        $work = $null
        try {
            $work = $norm
            if ($extra -gt 0 -and $gapStarts.Count -gt 0) {
                if ($imgOk) { $work = [ZpixOcrImg]::Widen($norm, $gapStarts, $extra) }
                else { $work = Ps-Widen $norm $gapStarts $extra }
            }
            $pad = [int][Math]::Max(16, [Math]::Round($imgH * $scale * $padKoef))
            if ($imgOk) { $bmp = [ZpixOcrImg]::Render($work, $scale, $stretch, $pad) }
            else { $bmp = Ps-Render $work $scale $stretch $pad }
            if (-not [object]::ReferenceEquals($work, $norm)) { $work.Dispose() }
            $t = Ocr-Bitmap $engine $bmp
        } catch {
            $t = ''
        }
        if ($t) { $results += [pscustomobject]@{ Text = $t; Stretch = $stretch } }
    }
}

# 行结构要保留：只把每行内部的多余空白压掉，行数不能变（多行文本被并成一行就废了）
function Norm-Key([string]$t) {
    $lines = $t -split "`r?`n"
    $out = @()
    foreach ($l in $lines) {
        $x = ([regex]::Replace($l, '[ \t]+', ' ')).Trim()
        if ($x) { $out += $x }
    }
    return ($out -join "`n")
}

$counts = @{}
$syMin = @{}
foreach ($r in $results) {
    $k = Norm-Key $r.Text
    if (-not $k) { continue }
    if ($counts.ContainsKey($k)) {
        $counts[$k] = $counts[$k] + 1
        $syMin[$k] = [Math]::Min($syMin[$k], $r.Stretch)
    } else {
        $counts[$k] = 1
        $syMin[$k] = $r.Stretch
    }
}

$best = ''
$bestScore = -1.0
foreach ($k in $counts.Keys) {
    $t = $k -replace "`n", "`r`n"
    $latin = ([regex]::Matches($t, '[A-Za-z]')).Count
    $cjk = ([regex]::Matches($t, '[\u3400-\u4dbf\u4e00-\u9fff]')).Count
    $sc = (Score-Text $k) + 0.8 * ($counts[$k] - 1)
    # 中文为主的内容不吃纵向拉伸这一套，优先不拉伸的结果
    if ($cjk -gt $latin -and $syMin[$k] -lt 1.2) { $sc += 1.5 }
    if ($sc -gt $bestScore) { $bestScore = $sc; $best = $t }
}
if (-not $best -and $results.Count -gt 0) { $best = $results[0].Text }

if ($norm -ne $null -and -not [object]::ReferenceEquals($norm, $src)) { $norm.Dispose() }
$src.Dispose()

# 中文 / 全角标点之间的空格去掉；保留西文词间空格
$best = [regex]::Replace($best, '(?<=[\u3000-\u303f\u4e00-\u9fff\uff00-\uffef])[ \t]+(?=[\u3000-\u303f\u4e00-\u9fff\uff00-\uffef])', '')
$best = [regex]::Replace($best, '[ \t]+(?=[，。！？；：、）】」』])', '')
$best = [regex]::Replace($best, '(?<=[（【「『])[ \t]+', '')
$best = $best.Trim()

Write-Out $best