[CmdletBinding()]
param(
    [string]$PackageRoot,
    [ValidateRange(1, 50000)] [int[]]$CatalogCounts = @(100, 1000),
    [switch]$PerformanceOnly
)
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'WindowsUi.cs') -ReferencedAssemblies System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$project = Split-Path $PSScriptRoot
if (!$PackageRoot) { $PackageRoot = Join-Path $project 'out/FlashFasaFiso' }
$run = Join-Path $project ('build/verification/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$processes = [Collections.Generic.List[Diagnostics.Process]]::new()
$drives = [Collections.Generic.List[string]]::new()
$env:RUST_LOG = 'warn,ruffle=info,avm_trace=info'

function Confirm([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}
function Wait-Until([scriptblock]$Condition, [string]$Message, [int]$Timeout = 10000) {
    $time = [Diagnostics.Stopwatch]::StartNew()
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 15
    } while ($time.ElapsedMilliseconds -lt $Timeout)
    throw $Message
}
function Fixture([string]$Root, [int]$Count, [bool]$Runtime = $false) {
    New-Item -ItemType Directory -Path $Root | Out-Null
    Copy-Item -LiteralPath (Join-Path $PackageRoot 'FlashFasaFiso.exe') -Destination $Root
    if ($Runtime) {
        New-Item -ItemType Directory -Path (Join-Path $Root 'runtime') | Out-Null
        Copy-Item -LiteralPath (Join-Path $PackageRoot 'runtime/ruffle.exe') -Destination (Join-Path $Root 'runtime')
    }
    if ($Count -gt 0) {
        & python (Join-Path $PSScriptRoot 'make_test_games.py') $Root --count $Count
        Confirm ($LASTEXITCODE -eq 0) 'SWF fixture generation failed'
    }
}
function Start-Launcher([string]$Root) {
    $time = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath (Join-Path $Root 'FlashFasaFiso.exe') -WorkingDirectory $project -PassThru
    $processes.Add($p)
    Wait-Until { $p.Refresh(); $p.MainWindowHandle -ne 0 } 'Launcher window did not become visible'
    $visible = $time.Elapsed.TotalMilliseconds
    $hwnd = $p.MainWindowHandle
    $edit = [WindowsUi]::FindWindowEx($hwnd, [IntPtr]::Zero, 'Edit', $null)
    Confirm ($edit -ne 0) 'Native search control is missing'
    [pscustomobject]@{ Process = $p; Hwnd = $hwnd; Edit = $edit; VisibleMs = $visible; Time = $time; Root = $Root }
}
function Catalog([object]$App, [int]$Count) {
    $file = Join-Path $App.Root 'data/cache/catalog.ini'
    Wait-Until {
        (Test-Path -LiteralPath $file) -and @(Select-String -LiteralPath $file -Pattern '^G\t').Count -eq $Count
    } "Expected $Count cached games"
    [void][WindowsUi]::UpdateWindow($App.Hwnd)
    $App.Time.Elapsed.TotalMilliseconds
}
function Dip([object]$App, [double]$Value) {
    [int][Math]::Round($Value * [WindowsUi]::GetDpiForWindow($App.Hwnd) / 96.0)
}
function Size-Dip([object]$App) {
    $rect = New-Object WindowsUi+Rect
    [void][WindowsUi]::GetClientRect($App.Hwnd, [ref]$rect)
    $scale = [WindowsUi]::GetDpiForWindow($App.Hwnd) / 96.0
    @(($rect.Right / $scale), ($rect.Bottom / $scale))
}
function Click([object]$App, [double]$X, [double]$Y, [switch]$Synchronously) {
    [void][WindowsUi]::UpdateWindow($App.Hwnd)
    $coords = [WindowsUi]::XY((Dip $App $X), (Dip $App $Y))
    if ($Synchronously) {
        [void][WindowsUi]::Send($App.Hwnd, 0x201, 1, $coords)
        [void][WindowsUi]::Send($App.Hwnd, 0x202, 0, $coords)
        return
    }
    Confirm ([WindowsUi]::PostMessage($App.Hwnd, 0x201, [IntPtr]1, [IntPtr]$coords)) 'Could not post mouse down'
    Confirm ([WindowsUi]::PostMessage($App.Hwnd, 0x202, [IntPtr]0, [IntPtr]$coords)) 'Could not post mouse up'
}
function Key([IntPtr]$Window, [int]$Code) {
    Confirm ([WindowsUi]::PostMessage($Window, 0x100, [IntPtr]$Code, [IntPtr]0)) 'Could not post key'
}
function Query([object]$App, [string]$Text) {
    [WindowsUi]::Query($App.Edit, $Text)
    [void][WindowsUi]::UpdateWindow($App.Hwnd)
}
function Close-Process([Diagnostics.Process]$Process) {
    $Process.Refresh()
    if (!$Process.HasExited) {
        $null = $Process.Handle
        Confirm ($Process.CloseMainWindow()) "Could not close $($Process.ProcessName)"
        Confirm ($Process.WaitForExit(8000)) "Timed out closing $($Process.ProcessName)"
    }
    Confirm ($Process.ExitCode -eq 0) "Process $($Process.Id) exited with code $($Process.ExitCode)"
}
function Screenshot([object]$App, [string]$Name) {
    [void][WindowsUi]::UpdateWindow($App.Hwnd)
    Start-Sleep -Milliseconds 100
    [WindowsUi]::Capture($App.Hwnd, (Join-Path $run "$Name.png"))
}
function Active-Dialog([object]$App) {
    $script:popup = [IntPtr]::Zero
    Wait-Until {
        $dialog = [WindowsUi]::GetLastActivePopup($App.Hwnd)
        if ($dialog -ne 0 -and $dialog -ne $App.Hwnd -and [WindowsUi]::GetWindow($dialog, 4) -eq $App.Hwnd -and
            ![WindowsUi]::IsWindowEnabled($App.Hwnd) -and [WindowsUi]::Caption($dialog)) {
            $script:popup = $dialog
            return $true
        }
        return $false
    } 'Expected launcher dialog did not open'
    $script:popup
}
function Dialog-Command([IntPtr]$Dialog, [int]$Command) {
    Confirm ([WindowsUi]::PostMessage($Dialog, 0x111, [IntPtr]$Command, [IntPtr]0)) 'Could not post dialog command'
}
function Confirm-CatalogReady([object]$App, [string]$ExpectedCaption) {
    $w, $h = Size-Dip $App
    Query $App "$([char]0xc7)oklu"
    Click $App ($w - 116) ($h - 71)
    $dialog = Active-Dialog $App
    if ($ExpectedCaption) {
        Confirm ([WindowsUi]::Caption($dialog) -ceq $ExpectedCaption) "Expected chooser language: $ExpectedCaption"
    }
    Dialog-Command $dialog 2
    Wait-Until { [WindowsUi]::IsWindowEnabled($App.Hwnd) } 'The populated entry chooser did not close'
}
function Measure-Idle([Diagnostics.Process]$Process, [string]$Name) {
    $start = $Process.TotalProcessorTime
    $time = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds 5
    $Process.Refresh()
    $cpu = ($Process.TotalProcessorTime - $start).TotalMilliseconds
    '{0}: CPU {1:F1} ms / {2:F0} ms wall; WS {3:F2} MiB; private {4:F2} MiB' -f $Name, $cpu, $time.Elapsed.TotalMilliseconds, ($Process.WorkingSet64 / 1MB), ($Process.PrivateMemorySize64 / 1MB)
}
function Resources([Diagnostics.Process]$Process) {
    $Process.Refresh()
    @($Process.HandleCount, [WindowsUi]::GetGuiResources($Process.Handle, 0),
        [WindowsUi]::GetGuiResources($Process.Handle, 1))
}

function Measure-ResizeCycles([object]$App) {
    $rect = New-Object WindowsUi+Rect
    Confirm ([WindowsUi]::GetWindowRect($App.Hwnd, [ref]$rect)) 'Could not read launcher bounds'
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $before = $null
    foreach ($cycle in 1..21) {
        foreach ($size in @(@((Dip $App 760), (Dip $App 480)), @($width, $height))) {
            Confirm ([WindowsUi]::SetWindowPos($App.Hwnd, [IntPtr]::Zero, 0, 0, $size[0], $size[1], 0x16)) 'Resize cycle failed'
        }
        Query $App 'Oyun'
        Query $App ''
        if ($cycle -eq 1) { $before = Resources $App.Process }
    }
    $after = Resources $App.Process
    Confirm ($before[1] -gt 0 -and $before[2] -gt 0 -and $after[1] -gt 0 -and $after[2] -gt 0) 'Could not sample GUI resources'
    "20 resize/search cycles (handles/GDI/USER): $($before -join '/') -> $($after -join '/')"
}
function Mount-Fixture([string]$Path) {
    $used = [IO.DriveInfo]::GetDrives().Name
    $letter = @('Z:', 'Y:', 'X:', 'W:', 'V:') | Where-Object { ($used -notcontains "$_\") -and !$drives.Contains($_) } |
        Select-Object -First 1
    Confirm (![string]::IsNullOrEmpty($letter)) 'No free test drive letter'
    & subst.exe $letter $Path
    Confirm ($LASTEXITCODE -eq 0) "subst failed for $letter"
    $drives.Add($letter)
    "$letter\"
}
function Log-Contents([string]$Root) {
    $contents = @{}
    $logs = Join-Path $Root 'data/cache/ruffle/log'
    if (Test-Path -LiteralPath $logs) {
        foreach ($file in Get-ChildItem -LiteralPath $logs -File) {
            $contents[$file.Name] = [string](Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8)
        }
    }
    $contents
}
function Logs([string]$Root) {
    (Log-Contents $Root).Values -join [Environment]::NewLine
}
function Find-Player([object]$App) {
    $parent = $App.Process.Id
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$parent AND Name='ruffle.exe'" |
        Select-Object -First 1
    if ($child) { Get-Process -Id $child.ProcessId -ErrorAction SilentlyContinue }
}
function Game-Hashes([string]$Root) {
    Get-ChildItem -LiteralPath (Join-Path $Root 'games') -Recurse -File |
        Sort-Object FullName | Get-FileHash | ForEach-Object Hash
}
function Play-MiniGame([object]$App, [int]$ExpectedOpens) {
    Query $App "I$([char]0x15f)$([char]0x131)k"
    $previous = Log-Contents $App.Root
    $started = [Diagnostics.Stopwatch]::StartNew()
    Key $App.Edit 13
    $script:ruffle = $null
    Wait-Until {
        $script:ruffle = Find-Player $App
        $null -ne $script:ruffle
    } 'Ruffle child process did not start'
    $processes.Add($script:ruffle)
    $null = $script:ruffle.Handle
    Wait-Until {
        foreach ($log in (Log-Contents $App.Root).GetEnumerator()) {
            $trace = $log.Value
            if ([string]::IsNullOrEmpty($trace)) { continue }
            $old = $previous[$log.Key]
            if ($old -and $trace.StartsWith($old, [StringComparison]::Ordinal)) {
                $trace = $trace.Substring($old.Length)
            }
            if ($trace.Contains("FFF portable opens=$ExpectedOpens") -and $trace.Contains('FFF relative asset loaded')) {
                return $true
            }
        }
        $false
    } 'Ruffle did not report the expected saved count and relative asset' 20000
    'Ruffle launch to saved state and relative asset trace: {0:F0} ms' -f $started.Elapsed.TotalMilliseconds | Write-Host
    Wait-Until { $script:ruffle.Refresh(); $script:ruffle.MainWindowHandle -ne 0 } 'Ruffle window missing'
    $script:ruffle
}

function Test-PortableDataErrors {
    $root = Join-Path $run 'locked-favorites'
    Fixture $root 1
    $data = New-Item -ItemType Directory -Path (Join-Path $root 'data')
    $file = Join-Path $data.FullName 'favorites.ini'
    [IO.File]::WriteAllText($file, "FFF favorites 1`nArchived game`n")
    $before = (Get-FileHash -LiteralPath $file).Hash
    $lock = [IO.File]::Open($file, 'Open', 'Read', 'None')
    try {
        $app = Start-Launcher $root
        [void](Catalog $app 1)
    } finally { $lock.Dispose() }
    $w, $h = Size-Dip $app
    Click $app ($w - 78) ($h - 108) -Synchronously
    Screenshot $app 'unreadable-favorites-preserved'
    Confirm ((Get-FileHash -LiteralPath $file).Hash -eq $before) 'A temporarily unreadable favorites file was overwritten'
    Close-Process $app.Process

    $root = Join-Path $run 'readonly-data'
    Fixture $root 2 $true
    $app = Start-Launcher $root
    [void](Catalog $app 2)
    Confirm-CatalogReady $app
    $w, $h = Size-Dip $app
    Click $app ($w - 78) ($h - 108) -Synchronously
    Close-Process $app.Process
    $data = Join-Path $root 'data'
    $file = Join-Path $data 'favorites.ini'
    $before = (Get-FileHash -LiteralPath $file).Hash
    $originalAcl = Get-Acl -LiteralPath $data
    $readOnlyAcl = Get-Acl -LiteralPath $data
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $rule = [Security.AccessControl.FileSystemAccessRule]::new($identity,
        'CreateFiles, CreateDirectories', 'Deny')
    $readOnlyAcl.AddAccessRule($rule)
    Set-Acl -LiteralPath $data -AclObject $readOnlyAcl
    try {
        $denied = $false
        try { [IO.File]::WriteAllText((Join-Path $data 'write-probe.txt'), '') }
        catch [UnauthorizedAccessException] { $denied = $true }
        Confirm $denied 'The test ACL did not deny creation under data'
        $app = Start-Launcher $root
        [void](Catalog $app 2)
        Confirm-CatalogReady $app
        Query $app "I$([char]0x15f)$([char]0x131)k"
        $w, $h = Size-Dip $app
        Click $app ($w - 78) ($h - 108) -Synchronously
        Confirm ((Get-FileHash -LiteralPath $file).Hash -eq $before) 'Denied write changed favorites'
        [void][WindowsUi]::Send($app.Edit, 0x100, 13, 0)
        $unexpected = Find-Player $app
        if ($unexpected) { $processes.Add($unexpected) }
        Confirm (!$unexpected) 'The player started despite the denied creation of portable save and log folders'
        Confirm (!(Test-Path -LiteralPath (Join-Path $data 'saves'))) 'The player created a save directory despite denied access'
        Screenshot $app 'readonly-data-play-error'
        Close-Process $app.Process
    } finally { Set-Acl -LiteralPath $data -AclObject $originalAcl }
    'Data errors: a sharing-denied favorites file and an ACL-denied data directory preserved existing favorites'
}

function Start-BrokenBody([object]$App) {
    Query $App 'Broken Body'
    Key $App.Edit 13
    $script:ruffle = $null
    Wait-Until { $script:ruffle = Find-Player $App; $null -ne $script:ruffle } 'No Ruffle process for the valid-header fixture'
    $processes.Add($script:ruffle)
    $null = $script:ruffle.Handle
    Wait-Until { (Logs $App.Root) -match 'Invalid SWF|Decompression|decompress' } 'Ruffle did not report the corrupt SWF body'
    $script:ruffle
}

function Confirm-StartupStopped([object]$App, [Diagnostics.Process]$Player, [switch]$CloseLauncher) {
    if ($Player.WaitForExit(5200)) {
        Confirm ($Player.ExitCode -eq 0) 'Graceful startup cancellation reported an error'
        return
    }
    if ($CloseLauncher) {
        Confirm ([WindowsUi]::PostMessage($App.Hwnd, 0x10, [IntPtr]0, [IntPtr]0)) 'Could not retry launcher closure'
    } else {
        $w, $h = Size-Dip $App
        Click $App ($w - 110) ($h - 150)
    }
    $dialog = Active-Dialog $App
    Confirm ([WindowsUi]::Caption($dialog) -eq 'End Ruffle Process') 'Forced termination did not require separate consent in English'
    Confirm ([WindowsUi]::Caption([WindowsUi]::GetDlgItem($dialog, 1)) -eq 'OK' -and
        [WindowsUi]::Caption([WindowsUi]::GetDlgItem($dialog, 2)) -eq 'Cancel') 'Startup dialog buttons did not follow the launcher language'
    if (!$CloseLauncher) { [WindowsUi]::Capture($dialog, (Join-Path $run 'ruffle-force-confirmation.png')) }
    Dialog-Command $dialog 1
    Confirm ($Player.WaitForExit(8000) -and $Player.ExitCode -eq 1223) 'Ruffle did not terminate after explicit consent'
    'Unresponsive Ruffle exited with ERROR_CANCELLED after separate confirmation'
}

function Test-InvalidSwf {
    $root = Join-Path $run 'invalid-swf'
    Fixture $root 0 $true
    $bodyDir = New-Item -ItemType Directory -Path (Join-Path $root 'games/Broken Body') -Force
    $bodySwf = Join-Path $bodyDir.FullName 'body.swf'
    [IO.File]::WriteAllBytes($bodySwf, [byte[]]@(0x43,0x57,0x53,7,48,0,0,0,0,0,0,0))
    $headerDir = New-Item -ItemType Directory -Path (Join-Path $root 'games/Broken Header') -Force
    $headerSwf = Join-Path $headerDir.FullName 'header.swf'
    [IO.File]::WriteAllText($headerSwf, 'Invalid Flash file fixture.')
    $before = (Game-Hashes $root) -join ','
    $app = Start-Launcher $root
    [void](Catalog $app 2)
    $player = Start-BrokenBody $app
    $w, $h = Size-Dip $app
    Click $app ($w - 110) ($h - 150)
    $dialog = Active-Dialog $app
    [WindowsUi]::Capture($dialog, (Join-Path $run 'ruffle-cancel-confirmation.png'))
    Dialog-Command $dialog 2
    Wait-Until { [WindowsUi]::IsWindowEnabled($app.Hwnd) } 'Startup cancellation dialog did not close'
    $player.Refresh()
    Confirm (!$player.HasExited) 'Ruffle exited after declining startup cancellation'
    Click $app ($w - 110) ($h - 150)
    Dialog-Command (Active-Dialog $app) 1
    Wait-Until { [WindowsUi]::IsWindowEnabled($app.Hwnd) } 'Startup cancellation acceptance did not close the dialog'
    Confirm-StartupStopped $app $player
    Screenshot $app 'ruffle-corrupt-body-cancelled'

    Query $app 'Broken Header'
    [void][WindowsUi]::Send($app.Edit, 0x100, 13, 0)
    $unexpected = Find-Player $app
    if ($unexpected) { $processes.Add($unexpected) }
    Confirm (!$unexpected) 'A malformed SWF header reached the player'
    Screenshot $app 'malformed-swf-rejected'
    Confirm (((Game-Hashes $root) -join ',') -eq $before) 'The malformed game files were changed'
    & python (Join-Path $PSScriptRoot 'make_test_games.py') $root --count 1 --compress
    Confirm ($LASTEXITCODE -eq 0) 'Recovery fixture generation failed'
    Key $app.Hwnd 116
    [void](Catalog $app 3)
    $player = Play-MiniGame $app 1
    Click $app ($w - 110) ($h - 150) -Synchronously
    $active = Find-Player $app
    Confirm ([WindowsUi]::IsWindowEnabled($app.Hwnd) -and $active -and $active.Id -eq $player.Id) 'A second play action disturbed a working game'
    Close-Process $player
    $player = Start-BrokenBody $app
    Confirm ([WindowsUi]::PostMessage($app.Hwnd, 0x10, [IntPtr]0, [IntPtr]0)) 'Could not request launcher closure during startup'
    Dialog-Command (Active-Dialog $app) 1
    Confirm-StartupStopped $app $player -CloseLauncher
    Confirm ($app.Process.WaitForExit(8000) -and $app.Process.ExitCode -eq 0) 'Launcher did not finish closing after the Ruffle process exited'
    'Malformed SWFs: invalid header rejected, corrupt body recovered with consent, valid CWS played, closing after consent left no player'
}

function Test-Workflows {
    Test-PortableDataErrors
    Test-InvalidSwf
    $small = Join-Path $run 'empty'
    Fixture $small 0
    $app = Start-Launcher $small
    $emptyScan = Catalog $app 0
    if ([int](Get-CimInstance Win32_OperatingSystem).BuildNumber -ge 22000) {
        Confirm ([WindowsUi]::CornerPreference($app.Hwnd) -eq 3) 'Small DWM corners were not registered'
    }
    Screenshot $app 'empty-missing-ruffle'
    "0 games: visible {0:F0} ms; empty cache observed {1:F0} ms" -f $app.VisibleMs, $emptyScan
    Measure-Idle $app.Process 'Launcher empty'
    Close-Process $app.Process

    $first = Join-Path $run 'original'
    Fixture $first 4 $true
    $unchanged = (Game-Hashes $first) -join ','
    $mounted = Mount-Fixture $first
    $app = Start-Launcher $mounted
    [void](Catalog $app 4)
    $turkishChooser = "Giri$([char]0x15f) Dosyas$([char]0x131)n$([char]0x131) Se$([char]0xe7)"
    $settings = Join-Path $first 'data/settings.ini'
    Confirm (!(Test-Path -LiteralPath $settings)) 'The clean language fixture already contains preferences'
    Confirm-CatalogReady $app 'Choose Entry File'
    $originalWidth, $originalHeight = Size-Dip $app
    Confirm ([WindowsUi]::SetWindowPos($app.Hwnd, [IntPtr]::Zero, 0, 0, (Dip $app 760), (Dip $app 480), 0x16)) 'Language fixture resize failed'
    Click $app 74 176 -Synchronously
    Click $app 331 159 -Synchronously
    Wait-Until { (Test-Path -LiteralPath $settings) -and (Get-Content -LiteralPath $settings -Raw).Contains('language=tr') } 'Turkish was not persisted'
    Click $app 72 86 -Synchronously
    Confirm ([WindowsUi]::SetWindowPos($app.Hwnd, [IntPtr]::Zero, 0, 0, (Dip $app $originalWidth), (Dip $app $originalHeight), 0x16)) 'Language fixture size restore failed'
    Query $app "I$([char]0x15f)$([char]0x131)k"
    Screenshot $app 'unicode-cover'
    $w, $h = Size-Dip $app
    Click $app ($w - 78) ($h - 108)
    Wait-Until { Test-Path -LiteralPath (Join-Path $first 'data/favorites.ini') } 'Favorite was not persisted'
    Confirm ((Get-Content -LiteralPath (Join-Path $first 'data/favorites.ini') -Raw -Encoding UTF8).Contains([string][char]0x96ea)) 'Unicode favorite was not persisted'

    Query $app "$([char]0xc7)oklu"
    Screenshot $app 'ambiguous-corrupt-cover'
    Click $app ($w - 116) ($h - 71)
    $chooser = Active-Dialog $app
    Confirm ([WindowsUi]::Caption($chooser) -eq $turkishChooser) 'Turkish entry dialog title was not localized'
    [WindowsUi]::Capture($chooser, (Join-Path $run 'swf-chooser.png'))
    $items = [WindowsUi]::FindWindowEx($chooser, [IntPtr]::Zero, 'ListBox', $null)
    Confirm ($items -ne 0 -and [WindowsUi]::Send($items, 0x18b, 0, 0).ToInt32() -eq 2) 'SWF chooser choices missing'
    [void][WindowsUi]::Send($items, 0x186, 1, 0)
    Dialog-Command $chooser 1
    $library = Join-Path $first 'data/library.ini'
    Wait-Until { Test-Path -LiteralPath $library } 'SWF entry was not persisted'
    Confirm ((Get-Content -LiteralPath $library -Raw -Encoding UTF8).Contains("giri$([char]0x15f) 2.swf")) 'Wrong SWF persisted'

    $hit = New-Object WindowsUi+Point
    $hit.X = Dip $app ($w - 69)
    $hit.Y = Dip $app 22
    [void][WindowsUi]::ClientToScreen($app.Hwnd, [ref]$hit)
    Confirm ([WindowsUi]::Send($app.Hwnd, 0x84, 0, [WindowsUi]::XY($hit.X,$hit.Y)).ToInt32() -eq 9) 'Maximize button hit test failed'
    [void][WindowsUi]::Send($app.Hwnd, 0xa1, 9, [WindowsUi]::XY($hit.X,$hit.Y))
    [void][WindowsUi]::Send($app.Hwnd, 0x202, 0, [WindowsUi]::XY((Dip $app ($w - 69)), (Dip $app 22)))
    Confirm ([WindowsUi]::IsZoomed($app.Hwnd)) 'Custom maximize click failed'
    $w, $h = Size-Dip $app
    Click $app ($w - 115) 22
    Wait-Until { [WindowsUi]::IsIconic($app.Hwnd) } 'Custom minimize click failed'
    [void][WindowsUi]::ShowWindow($app.Hwnd, 9)
    [void][WindowsUi]::ShowWindow($app.Hwnd, 9)
    Confirm ([WindowsUi]::SetWindowPos($app.Hwnd, [IntPtr]::Zero, 100, 100, (Dip $app 760), (Dip $app 480), 0x14)) 'Compact resize failed'
    Screenshot $app 'compact-760x480'
    $w, $h = Size-Dip $app
    Confirm ($w -ge 759 -and $h -ge 479) 'Compact dimensions failed'
    $titlePoint = New-Object WindowsUi+Point
    $titlePoint.X = Dip $app 200
    $titlePoint.Y = Dip $app 22
    [void][WindowsUi]::ClientToScreen($app.Hwnd, [ref]$titlePoint)
    Confirm ([WindowsUi]::PostMessage($app.Hwnd, 0xa3, [IntPtr]2, [IntPtr]([WindowsUi]::XY($titlePoint.X, $titlePoint.Y)))) 'Could not post caption double-click'
    Wait-Until { [WindowsUi]::IsZoomed($app.Hwnd) } 'Caption double click failed'
    [void][WindowsUi]::ShowWindow($app.Hwnd, 9)
    $originalMonitor = [WindowsUi]::MonitorFromWindow($app.Hwnd, 2)
    $current = [Windows.Forms.Screen]::FromHandle($app.Hwnd)
    $second = [Windows.Forms.Screen]::AllScreens | Where-Object DeviceName -ne $current.DeviceName | Select-Object -First 1
    if ($second) {
        $target = $second.WorkingArea
        Confirm ([WindowsUi]::SetWindowPos($app.Hwnd, [IntPtr]::Zero, ($target.Left + 70),
            ($target.Top + 70), (Dip $app 760), (Dip $app 480), 0x14)) 'Move to second monitor failed'
        Confirm ([WindowsUi]::MonitorFromWindow($app.Hwnd, 2) -ne $originalMonitor) 'Window did not reach the second monitor'
        [void][WindowsUi]::ShowWindow($app.Hwnd, 3)
        Confirm ([WindowsUi]::IsZoomed($app.Hwnd)) 'Maximize on second monitor failed'
        $point = New-Object WindowsUi+Point
        [void][WindowsUi]::ClientToScreen($app.Hwnd, [ref]$point)
        Confirm ($point.X -eq $target.Left -and $point.Y -eq $target.Top) 'Maximized client escaped the second work area'
        [void][WindowsUi]::ShowWindow($app.Hwnd, 9)
        "Second monitor move/maximize: $($current.DeviceName) to $($second.DeviceName), $([WindowsUi]::GetDpiForWindow($app.Hwnd)) DPI"
    }
    'UI: Unicode search and WIC cover, saved favorites and SWF selection, custom maximize/minimize, caption double-click and compact resize'

    $player = Play-MiniGame $app 1
    $ruffleWindow = $player.MainWindowHandle
    $gamePoint = [WindowsUi]::XY(100, 118)
    [void][WindowsUi]::PostMessage($ruffleWindow, 0x200, [IntPtr]0, [IntPtr]$gamePoint)
    [void][WindowsUi]::PostMessage($ruffleWindow, 0x201, [IntPtr]1, [IntPtr]$gamePoint)
    [void][WindowsUi]::PostMessage($ruffleWindow, 0x202, [IntPtr]0, [IntPtr]$gamePoint)
    Wait-Until { (Logs $app.Root).Contains('FFF hits=1') } 'Mini Hedef button did not register the click'
    [WindowsUi]::Capture($player.MainWindowHandle, (Join-Path $run 'ruffle-first.png'))
    Measure-Idle $player 'Ruffle Mini Hedef'
    Close-Process $app.Process
    Confirm (!$player.HasExited) 'Closing launcher ended the separate player'
    $competitor = Start-Launcher $mounted
    [void](Catalog $competitor 4)
    Confirm-CatalogReady $competitor $turkishChooser
    'Language: English on a clean profile, live switch to Turkish, and Turkish retained after restart'
    Query $competitor "I$([char]0x15f)$([char]0x131)k"
    [void][WindowsUi]::Send($competitor.Edit, 0x100, 13, 0)
    $duplicate = Find-Player $competitor
    if ($duplicate) { $processes.Add($duplicate) }
    Confirm (!$duplicate -and !$player.HasExited) 'Another launcher opened a second player for the same portable profile'
    Screenshot $competitor 'ruffle-existing-session'
    Close-Process $player
    $sol = @(Get-ChildItem -LiteralPath (Join-Path $first 'data/saves') -Filter '*.sol' -Recurse -File)
    Confirm ($sol.Count -eq 1 -and $sol[0].Length -gt 32) 'SharedObject was not written below data/saves'
    $saveBeforeMove = (Get-FileHash -LiteralPath $sol[0].FullName).Hash

    $other = Join-Path $run 'relocated folder'
    Copy-Item -LiteralPath $first -Destination $other -Recurse
    $reopened = Play-MiniGame $competitor 2
    Close-Process $reopened
    Close-Process $competitor.Process
    'Player isolation: restart kept the existing player and allowed a new one after it exited'
    $secondDrive = Mount-Fixture $other
    $app = Start-Launcher $secondDrive
    [void](Catalog $app 4)
    Query $app "$([char]0xc7)oklu"
    Screenshot $app 'relocated-retained-entry'
    Query $app ''
    Click $app 76 132
    Start-Sleep -Milliseconds 100
    Screenshot $app 'relocated-favorites'
    $player = Play-MiniGame $app 2
    [WindowsUi]::Capture($player.MainWindowHandle, (Join-Path $run 'ruffle-relocated.png'))
    Close-Process $player
    Close-Process $app.Process
    $after = @(Get-ChildItem -LiteralPath (Join-Path $other 'data/saves') -Filter '*.sol' -Recurse -File)
    Confirm ($after.Count -eq 1 -and (Get-FileHash $after[0].FullName).Hash -ne $saveBeforeMove) 'SharedObject was not updated after relocation'
    Confirm (((Game-Hashes $first) -join ',') -eq $unchanged -and ((Game-Hashes $other) -join ',') -eq $unchanged) 'Playback changed original game files'
    "Portable: $mounted to $secondDrive at a different parent directory, same SharedObject incremented 1 to 2, relative asset loaded twice"

    $edge = Join-Path $run 'refresh-edges'
    Fixture $edge 1
    $gameDir = Get-ChildItem -LiteralPath (Join-Path $edge 'games') -Directory | Select-Object -First 1
    $main = Get-ChildItem -LiteralPath $gameDir.FullName -Filter '*.swf' -File | Select-Object -First 1
    $target = New-Item -ItemType Directory -Path (Join-Path $edge 'junction-target')
    Copy-Item -LiteralPath $main.FullName -Destination $target.FullName
    New-Item -ItemType Junction -Path (Join-Path $edge 'games/not-a-game') -Target $target.FullName | Out-Null
    $app = Start-Launcher $edge
    [void](Catalog $app 1)
    Key $app.Edit 13
    Screenshot $app 'missing-runtime-play'
    $brokenRuntime = New-Item -ItemType Directory -Path (Join-Path $edge 'runtime')
    [IO.File]::WriteAllText((Join-Path $brokenRuntime.FullName 'ruffle.exe'), 'Invalid executable fixture.')
    [void][WindowsUi]::Send($app.Edit, 0x100, 13, 0)
    Confirm (!(Find-Player $app)) 'An invalid executable unexpectedly started'
    Confirm (!(Test-Path -LiteralPath (Join-Path $edge 'data/ruffle.lock'))) 'An invalid executable reached portable profile setup'
    Screenshot $app 'invalid-runtime-error'
    $extra = New-Item -ItemType Directory -Path (Join-Path $edge 'games/Added')
    Copy-Item -LiteralPath $main.FullName -Destination $extra.FullName
    Key $app.Hwnd 116
    [void](Catalog $app 2)
    $path = (Resolve-Path -LiteralPath (Join-Path $extra.FullName $main.Name)).ProviderPath
    Confirm ($path.StartsWith($run + '\', [StringComparison]::OrdinalIgnoreCase)) 'Unsafe test fixture path'
    Remove-Item -LiteralPath $path
    Key $app.Hwnd 116
    [void](Catalog $app 1)
    $w, $h = Size-Dip $app
    Click $app ($w - 23) 22
    Confirm ($app.Process.WaitForExit(5000) -and $app.Process.ExitCode -eq 0) 'Custom close failed'
    'Edge cases: junction ignored, new and removed files reflected after F5, launcher closed cleanly'

    $prefsRoot = Join-Path $run 'settings'
    Copy-Item -LiteralPath $other -Destination $prefsRoot -Recurse
    $prefsMain = Get-ChildItem -LiteralPath (Join-Path $prefsRoot 'games') -Filter '*.swf' -File -Recurse | Select-Object -First 1
    $nested = New-Item -ItemType Directory -Path (Join-Path $prefsRoot 'games/Nested Only/inner/deeper') -Force
    Copy-Item -LiteralPath $prefsMain.FullName -Destination $nested.FullName
    $prefsData = Join-Path $prefsRoot 'data'
    $prefsFile = Join-Path $prefsData 'settings.ini'
    $catalogFile = Join-Path $prefsData 'cache/catalog.ini'
    $swfFile = Join-Path $prefsData 'library.ini'
    $prefsFiles = @((Join-Path $prefsData 'favorites.ini'), $swfFile) +
        @(Get-ChildItem -LiteralPath (Join-Path $prefsData 'saves') -Recurse -File | ForEach-Object FullName) +
        @(Get-ChildItem -LiteralPath (Join-Path $prefsData 'cache/ruffle/log') -File | ForEach-Object FullName) +
        @($prefsMain.FullName)
    $beforeHashes = $prefsFiles | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash }
    $app = Start-Launcher $prefsRoot
    [void](Catalog $app 5)
    Confirm ([WindowsUi]::SetWindowPos($app.Hwnd, [IntPtr]::Zero, 115, 115, (Dip $app 760), (Dip $app 480), 0x14)) 'Compact settings resize failed'
    Click $app 74 176
    Screenshot $app 'settings-tr-compact'
    Click $app 397 159
    Wait-Until { (Get-Content -LiteralPath $prefsFile -Raw).Contains('language=en') } 'English was not persisted'
    Screenshot $app 'settings-en-compact'
    Click $app 320 191
    Click $app 320 220
    Click $app 320 248
    Click $app 641 248
    Click $app 320 396
    [void](Catalog $app 4)
    Wait-Until {
        $ini = Get-Content -LiteralPath $prefsFile -Raw
        ($ini -match 'auto_scan=0') -and ($ini -match 'remember_tab=1') -and ($ini -match 'confirm_close=1') -and
        ($ini -match 'scan_nested=0') -and ($ini -match 'remember_swf=0')
    } 'Settings toggles did not persist'
    Click $app 677 398
    Wait-Until { !(Test-Path -LiteralPath $catalogFile) } 'Clear Cache did not remove the launcher catalog'
    $afterHashes = $prefsFiles | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash }
    Confirm (($beforeHashes -join ',') -eq ($afterHashes -join ',')) 'Cache cleanup changed gameplay or user data'
    Key $app.Hwnd 116
    [void](Catalog $app 4)
    Confirm ((Get-Content -LiteralPath $catalogFile -TotalCount 1) -eq 'FFF catalog 2 flat') 'Flat catalog was not tagged'

    Click $app 72 86
    Query $app "$([char]0xc7)oklu"
    Key $app.Edit 13
    $dialog = Active-Dialog $app
    Confirm ([WindowsUi]::Caption($dialog) -eq 'Choose Entry File') 'English SWF dialog did not update'
    [WindowsUi]::Capture($dialog, (Join-Path $run 'swf-chooser-en.png'))
    $items = [WindowsUi]::FindWindowEx($dialog, [IntPtr]::Zero, 'ListBox', $null)
    Confirm ([WindowsUi]::Send($items, 0x188, 0, 0).ToInt32() -eq 0) 'A disabled preference reused an old selection'
    Dialog-Command $dialog 2
    Wait-Until { [WindowsUi]::IsWindowEnabled($app.Hwnd) } 'SWF chooser did not cancel'
    Click $app 648 408
    $dialog = Active-Dialog $app
    Dialog-Command $dialog 1
    Wait-Until { [WindowsUi]::IsWindowEnabled($app.Hwnd) } 'SWF chooser did not close'
    Confirm ((Get-FileHash -LiteralPath $swfFile).Hash -eq $beforeHashes[1]) 'Session SWF choice changed the stored choice'
    Query $app ''
    Click $app 72 131
    [void][WindowsUi]::PostMessage($app.Hwnd, 0x10, [IntPtr]0, [IntPtr]0)
    $dialog = Active-Dialog $app
    Confirm ([WindowsUi]::Caption($dialog) -eq 'Close FlashFasaFiso?') 'English close confirmation was not localized'
    [WindowsUi]::Capture($dialog, (Join-Path $run 'confirm-close-en.png'))
    Dialog-Command $dialog 2
    Wait-Until { [WindowsUi]::IsWindowEnabled($app.Hwnd) } 'Cancel unexpectedly closed the launcher'
    [void][WindowsUi]::PostMessage($app.Hwnd, 0x10, [IntPtr]0, [IntPtr]0)
    Dialog-Command (Active-Dialog $app) 1
    Confirm ($app.Process.WaitForExit(5000) -and $app.Process.ExitCode -eq 0) 'Confirmed close failed'
    Confirm ((Get-Content -LiteralPath $prefsFile -Raw).Contains('last_tab=favorites')) 'Last tab was not persisted'
    $savedCatalog = (Get-FileHash -LiteralPath $catalogFile).Hash
    $addedOnDisk = New-Item -ItemType Directory -Path (Join-Path $prefsRoot 'games/Added While Closed')
    Copy-Item -LiteralPath $prefsMain.FullName -Destination $addedOnDisk.FullName
    $app = Start-Launcher $prefsRoot
    Screenshot $app 'restored-favorites-en'
    Start-Sleep -Milliseconds 500
    Confirm ((Get-FileHash -LiteralPath $catalogFile).Hash -eq $savedCatalog) 'Startup rescanned when automatic scanning was disabled'
    Key $app.Hwnd 116
    [void](Catalog $app 5)
    Confirm ([WindowsUi]::SetWindowPos($app.Hwnd, [IntPtr]::Zero, 115, 115, (Dip $app 760), (Dip $app 480), 0x14)) 'Compact settings resize after restart failed'
    Click $app 74 176
    Wait-Until { ![WindowsUi]::IsWindowVisible($app.Edit) } 'Settings page did not open after restart'
    Click $app 331 159
    Wait-Until { (Get-Content -LiteralPath $prefsFile -Raw).Contains('language=tr') } 'Could not switch back to Turkish'
    Click $app 320 248
    Wait-Until { (Get-Content -LiteralPath $prefsFile -Raw).Contains('confirm_close=0') } 'Close confirmation could not be disabled'
    Confirm ([WindowsUi]::SetWindowPos($app.Hwnd, [IntPtr]::Zero, 115, 115, (Dip $app 530), (Dip $app 320), 0x414)) 'Short work-area fixture resize failed'
    $w, $h = Size-Dip $app
    Confirm ($w -ge 529 -and $w -le 531 -and $h -ge 319 -and $h -le 321) 'Short work-area fixture did not reach 530x320 DIP'
    Screenshot $app 'settings-530x320-top'
    $point = New-Object WindowsUi+Point
    $point.X = Dip $app 350
    $point.Y = Dip $app 180
    Confirm ([WindowsUi]::ClientToScreen($app.Hwnd, [ref]$point)) 'Could not position the settings scroll message'
    [void][WindowsUi]::Send($app.Hwnd, 0x20a, ((-1200 -band 0xffff) -shl 16), [WindowsUi]::XY($point.X,$point.Y))
    Confirm (Test-Path -LiteralPath $catalogFile) 'Short settings fixture is missing its catalog'
    Screenshot $app 'settings-530x320-scrolled'
    Click $app 410 254
    Wait-Until { !(Test-Path -LiteralPath $catalogFile) } 'The bottom settings action was inaccessible after scrolling'
    Click $app 72 86
    Wait-Until { [WindowsUi]::IsWindowVisible($app.Edit) } 'The short library view did not open'
    Query $app "I$([char]0x15f)$([char]0x131)k"
    Screenshot $app 'library-530x320'
    Close-Process $app.Process
    'Settings: live Turkish/English, scan options, safe cache cleanup, SWF choice in memory only, last tab and confirmation persisted, bottom action reached at 530x320 DIP'
}

try {
    if (!$PerformanceOnly) { Test-Workflows }
    foreach ($count in $CatalogCounts) {
        $root = Join-Path $run "perf-$count"
        Fixture $root $count
        $app = Start-Launcher $root
        $load = Catalog $app $count
        $searchTimes = foreach ($i in 1..12) {
            $time = [Diagnostics.Stopwatch]::StartNew()
            Query $app ("Oyun {0:0000}" -f $i)
            $time.Elapsed.TotalMilliseconds
        }
        Query $app ''
        Key $app.Hwnd 35
        Screenshot $app "list-end-$count"
        $stats = $searchTimes | Measure-Object -Minimum -Maximum -Average
        "Games {0}: visible {1:F0} ms; cold cache observed {2:F0} ms; search + paint avg/max {3:F2}/{4:F2} ms" -f $count, $app.VisibleMs, $load, $stats.Average, $stats.Maximum
        Measure-Idle $app.Process "Launcher $count"
        if ($count -eq 1000) { Measure-ResizeCycles $app }
        Close-Process $app.Process
    }
    "PASS. Evidence: $run"
} finally {
    foreach ($process in $processes) {
        $process.Refresh()
        if (!$process.HasExited) {
            [void]$process.CloseMainWindow()
            if (!$process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit() }
        }
    }
    foreach ($drive in $drives) { & subst.exe $drive /d }
}
