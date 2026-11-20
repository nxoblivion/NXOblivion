using System.Diagnostics;

namespace OblivionInstaller;

public sealed class MainForm : Form
{
    readonly string _kitDir;
    Kit? _kit;
    GameSource? _source;
    GameFile? _xex;
    CancellationTokenSource? _cts;

    readonly TextBox _sourcePath = new() { ReadOnly = true, Dock = DockStyle.Fill, PlaceholderText = "Your Xbox 360 disc image (.iso) or extracted game folder" };
    readonly Button _pickIso = new() { Text = "Disc image...", AutoSize = true };
    readonly Button _pickFolder = new() { Text = "Folder...", AutoSize = true };
    readonly Label _sourceStatus = new() { AutoSize = true, MaximumSize = new Size(640, 0) };

    readonly Button _build = new() { Text = "Build oblivion.nro", AutoSize = true, Enabled = false };
    readonly ProgressBar _buildBar = new() { Dock = DockStyle.Fill, Maximum = 1000 };
    readonly Label _buildStatus = new() { AutoSize = true, MaximumSize = new Size(640, 0) };

    readonly RadioButton _toFolder = new() { Text = "SD card or folder", AutoSize = true, Checked = true };
    readonly TextBox _folderPath = new() { Dock = DockStyle.Fill, PlaceholderText = "e.g. E:\\ (the SD card) or an empty folder" };
    readonly Button _pickTarget = new() { Text = "Browse...", AutoSize = true };
    readonly RadioButton _toMtp = new() { Text = "Switch over USB (DBI: MTP responder)", AutoSize = true };
    readonly ComboBox _devices = new() { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
    readonly Button _refresh = new() { Text = "Refresh", AutoSize = true };
    readonly CheckBox _includeGame = new() { Text = "Copy the game files too", AutoSize = true, Checked = true };
    readonly Button _install = new() { Text = "Install", AutoSize = true, Enabled = false };
    readonly Button _cancel = new() { Text = "Cancel", AutoSize = true, Enabled = false };
    readonly ProgressBar _installBar = new() { Dock = DockStyle.Fill, Maximum = 1000 };
    readonly Label _installStatus = new() { AutoSize = true, MaximumSize = new Size(640, 0) };

    readonly TextBox _log = new()
    {
        Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill,
        Font = new Font("Consolas", 9f), WordWrap = true,
    };

    public MainForm(string kitDir)
    {
        _kitDir = kitDir;
        Text = "Oblivion for Switch - installer";
        Font = new Font("Segoe UI", 9.5f);
        ClientSize = new Size(760, 760);
        MinimumSize = new Size(640, 600);
        StartPosition = FormStartPosition.CenterScreen;
        BuildLayout();
        WireEvents();
        Shown += (_, _) => LoadKit();
    }

    void BuildLayout()
    {
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(14), AutoScroll = true };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        Controls.Add(root);

        var header = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Dock = DockStyle.Fill, WrapContents = false };
        header.Controls.Add(new Label { Text = "Oblivion for Nintendo Switch", AutoSize = true, Font = new Font("Segoe UI Semibold", 15f) });
        header.Controls.Add(new Label
        {
            AutoSize = true, MaximumSize = new Size(700, 0), ForeColor = SystemColors.GrayText,
            Text = "Builds the Switch version on this PC from your own Xbox 360 copy of the game. " +
                   "Nothing from the game comes with this installer; your files never leave your PC.",
        });
        root.Controls.Add(header);

        // 1. Game
        var g1 = Section("1   Your game", out var t1);
        t1.ColumnCount = 3;
        t1.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        t1.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        t1.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        t1.Controls.Add(_sourcePath, 0, 0);
        t1.Controls.Add(_pickIso, 1, 0);
        t1.Controls.Add(_pickFolder, 2, 0);
        t1.Controls.Add(_sourceStatus, 0, 1);
        t1.SetColumnSpan(_sourceStatus, 3);
        root.Controls.Add(g1);

        // 2. Build
        var g2 = Section("2   Build", out var t2);
        t2.ColumnCount = 2;
        t2.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        t2.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        t2.Controls.Add(_build, 0, 0);
        t2.Controls.Add(_buildBar, 1, 0);
        t2.Controls.Add(_buildStatus, 0, 1);
        t2.SetColumnSpan(_buildStatus, 2);
        _buildStatus.Text = "Translates and compiles the game code for the Switch. Takes a few minutes the first time.";
        root.Controls.Add(g2);

        // 3. Install
        var g3 = Section("3   Copy to your Switch", out var t3);
        t3.ColumnCount = 3;
        t3.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        t3.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        t3.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        t3.Controls.Add(_toFolder, 0, 0);
        t3.Controls.Add(_folderPath, 1, 0);
        t3.Controls.Add(_pickTarget, 2, 0);
        t3.Controls.Add(_toMtp, 0, 1);
        t3.Controls.Add(_devices, 1, 1);
        t3.Controls.Add(_refresh, 2, 1);
        t3.Controls.Add(_includeGame, 0, 2);
        t3.SetColumnSpan(_includeGame, 3);
        var buttons = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        buttons.Controls.Add(_install);
        buttons.Controls.Add(_cancel);
        t3.Controls.Add(buttons, 0, 3);
        t3.Controls.Add(_installBar, 1, 3);
        t3.SetColumnSpan(_installBar, 2);
        t3.Controls.Add(_installStatus, 0, 4);
        t3.SetColumnSpan(_installStatus, 3);
        var help = new LinkLabel { Text = "How to start it on the Switch", AutoSize = true };
        help.LinkClicked += (_, _) => ShowLaunchHelp();
        t3.Controls.Add(help, 0, 5);
        t3.SetColumnSpan(help, 3);
        root.Controls.Add(g3);

        var logBox = new GroupBox { Text = "Log", Dock = DockStyle.Fill, Padding = new Padding(8), MinimumSize = new Size(0, 120) };
        logBox.Controls.Add(_log);
        root.Controls.Add(logBox);
    }

    static GroupBox Section(string title, out TableLayoutPanel table)
    {
        var box = new GroupBox
        {
            Text = title, Dock = DockStyle.Fill, AutoSize = true, Padding = new Padding(10, 6, 10, 10),
            Font = new Font("Segoe UI Semibold", 10f), Margin = new Padding(0, 10, 0, 0),
        };
        table = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, Font = new Font("Segoe UI", 9.5f) };
        box.Controls.Add(table);
        return box;
    }

    void WireEvents()
    {
        _pickIso.Click += (_, _) =>
        {
            using var d = new OpenFileDialog { Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*", Title = "Your Oblivion (Xbox 360) disc image" };
            if (d.ShowDialog(this) == DialogResult.OK) _ = SelectSourceAsync(d.FileName);
        };
        _pickFolder.Click += (_, _) =>
        {
            using var d = new FolderBrowserDialog { Description = "Folder with default.xex, Oblivion.ini, data and Data2" };
            if (d.ShowDialog(this) == DialogResult.OK) _ = SelectSourceAsync(d.SelectedPath);
        };
        _pickTarget.Click += (_, _) =>
        {
            using var d = new FolderBrowserDialog { Description = "SD card root (or a folder to copy to the SD card later)" };
            if (d.ShowDialog(this) == DialogResult.OK) { _folderPath.Text = d.SelectedPath; _toFolder.Checked = true; }
        };
        _refresh.Click += async (_, _) => await RefreshDevicesAsync();
        _toMtp.CheckedChanged += async (_, _) => { if (_toMtp.Checked && _devices.Items.Count == 0) await RefreshDevicesAsync(); };
        _build.Click += async (_, _) => await BuildAsync(force: true);
        _install.Click += async (_, _) => await InstallAsync();
        _cancel.Click += (_, _) => _cts?.Cancel();
    }

    void Log(string s)
    {
        if (InvokeRequired) { BeginInvoke(() => Log(s)); return; }
        _log.AppendText(s.Replace("\n", Environment.NewLine) + Environment.NewLine);
    }

    void LoadKit()
    {
        try
        {
            _kit = Kit.Load(_kitDir);
            Text = $"{_kit.Title} for Switch - installer v{_kit.Version}";
            _includeGame.Text = $"Copy the game files too ({_kit.TotalGameBytes / 1e9:F1} GB, {_kit.Files.Count} files; already copied ones are skipped)";
            Log($"kit v{_kit.Version}: {_kitDir}");
            Log($"build folder: {AppPaths.WorkRoot}");
        }
        catch (Exception e)
        {
            Fail(_sourceStatus, e.Message);
            _pickIso.Enabled = _pickFolder.Enabled = false;
        }
    }

    internal Task SelectSourceForTestAsync(string path) => SelectSourceAsync(path);

    async Task SelectSourceAsync(string path)
    {
        if (_kit == null) return;
        _sourcePath.Text = path;
        _source = null; _xex = null;
        _build.Enabled = _install.Enabled = false;
        Info(_sourceStatus, "Reading...");
        try
        {
            var (src, problems, xex) = await Task.Run(() =>
            {
                var s = GameSource.Load(path);
                var p = new Builder(_kit, Log).Verify(s, out var x);
                return (s, p, x);
            });
            Log($"{path}: {src.Kind}, {src.Files.Count} files");
            if (problems.Count > 0)
            {
                foreach (var p in problems) Log("  " + p);
                Fail(_sourceStatus, string.Join(Environment.NewLine, problems.Take(3)));
                return;
            }
            _source = src; _xex = xex;
            Ok(_sourceStatus, $"OK: {src.Kind}. default.xex matches {_kit.GameName}; all {_kit.Files.Count} game files found.");
            _build.Enabled = _install.Enabled = true;
            var b = new Builder(_kit, Log);
            if (b.IsBuilt)
            {
                _buildBar.Value = _buildBar.Maximum;
                Ok(_buildStatus, $"Already built for this copy ({new FileInfo(b.NroPath).Length / 1048576.0:F0} MB). Press Build to build again.");
            }
        }
        catch (Exception e)
        {
            Log("error: " + e);
            Fail(_sourceStatus, e.Message);
        }
    }

    async Task<bool> BuildAsync(bool force)
    {
        if (_kit == null || _source == null || _xex == null) return false;
        var builder = new Builder(_kit, Log);
        if (!force && builder.IsBuilt) return true;
        SetBusy(true);
        _cts = new CancellationTokenSource();
        var sw = Stopwatch.StartNew();
        var progress = new Progress<BuildProgress>(p =>
        {
            _buildBar.Value = (int)(p.Fraction * _buildBar.Maximum);
            Info(_buildStatus, $"{p.Stage}{(p.Detail != null ? "  " + p.Detail : "")}   ({sw.Elapsed:mm\\:ss})");
        });
        try
        {
            await Task.Run(() => builder.BuildAsync(_source, _xex, progress, _cts.Token));
            Ok(_buildStatus, $"Built oblivion.nro in {sw.Elapsed:mm\\:ss}.");
            return true;
        }
        catch (OperationCanceledException)
        {
            Fail(_buildStatus, "Cancelled.");
        }
        catch (Exception e)
        {
            Log("error: " + e);
            Fail(_buildStatus, "Build failed: " + e.Message + " (details in the log)");
        }
        finally
        {
            SetBusy(false);
        }
        return false;
    }

    async Task RefreshDevicesAsync()
    {
        _devices.Items.Clear();
        _refresh.Enabled = false;
        try
        {
            var found = await RunSta(() => MtpTarget.List().SelectMany(d => d.storages.Select(s => $"{d.name}  >  {s}")).ToList());
            foreach (var f in found) _devices.Items.Add(f);
            var sd = found.FindIndex(f => f.Contains("Switch", StringComparison.OrdinalIgnoreCase) && f.Contains("SD", StringComparison.OrdinalIgnoreCase));
            if (found.Count > 0) _devices.SelectedIndex = Math.Max(0, sd);
            if (found.Count == 0) Info(_installStatus, "No USB device found. On the Switch: open DBI, choose \"Run MTP responder\", connect the USB cable, then press Refresh.");
        }
        catch (Exception e)
        {
            Log("MTP: " + e.Message);
        }
        finally
        {
            _refresh.Enabled = true;
        }
    }

    async Task InstallAsync()
    {
        if (_kit == null || _source == null) return;
        string? folder = null, device = null;
        if (_toFolder.Checked)
        {
            folder = _folderPath.Text.Trim();
            if (folder.Length == 0 || !Directory.Exists(folder)) { Fail(_installStatus, "Choose the SD card or a folder first."); return; }
        }
        else
        {
            device = _devices.SelectedItem as string;
            if (device == null) { Fail(_installStatus, "Connect the Switch (DBI: Run MTP responder) and press Refresh."); return; }
        }
        if (!await BuildAsync(force: false)) return;

        var builder = new Builder(_kit, Log);
        SetBusy(true);
        _cts = new CancellationTokenSource();
        var sw = Stopwatch.StartNew();
        var progress = new Progress<InstallProgress>(p =>
        {
            _installBar.Value = p.Total > 0 ? (int)(p.Done * _installBar.Maximum / p.Total) : _installBar.Maximum;
            double mbps = p.Done / 1048576.0 / Math.Max(1, sw.Elapsed.TotalSeconds);
            Info(_installStatus, $"{p.File}   {p.Done / 1048576:N0} / {p.Total / 1048576:N0} MB   {mbps:F1} MB/s");
        });
        bool includeGame = _includeGame.Checked;
        try
        {
            await RunSta(() =>
            {
                IInstallTarget target;
                if (folder != null) target = new FolderTarget(folder);
                else
                {
                    var parts = device!.Split("  >  ");
                    var dev = MtpTarget.List().FirstOrDefault(d => d.name == parts[0] && d.storages.Contains(parts[1]));
                    if (dev.dev == null) throw new InvalidOperationException("The Switch is no longer connected. Press Refresh.");
                    target = new MtpTarget(dev.dev, parts[1]);
                }
                using (target)
                    Installer.Run(target, _kit, builder.NroPath, _source, includeGame, Log, progress, _cts.Token);
                return 0;
            });
            Ok(_installStatus, $"Done in {sw.Elapsed:mm\\:ss}. " +
                               (folder != null ? "If this was not the SD card itself, copy the 'switch' and 'oblivion' folders to the SD card root." : "You can close DBI now."));
            ShowLaunchHelp();
        }
        catch (OperationCanceledException)
        {
            Fail(_installStatus, "Cancelled. Run Install again to continue where it stopped.");
        }
        catch (Exception e)
        {
            Log("error: " + e);
            Fail(_installStatus, "Install failed: " + e.Message);
        }
        finally
        {
            SetBusy(false);
        }
    }

    void ShowLaunchHelp() => MessageBox.Show(this,
        "On the Switch (Atmosphere):\n\n" +
        "1. On the HOME menu, hold R and start any game (not the Album).\n" +
        "   Keep holding R until the Homebrew Menu appears.\n" +
        "2. Choose Oblivion.\n\n" +
        "The full memory of a game slot is needed; started from the Album it shows a message and exits.\n" +
        "The first start compiles graphics shaders and is slow.",
        "Starting Oblivion", MessageBoxButtons.OK, MessageBoxIcon.Information);

    void SetBusy(bool busy)
    {
        _pickIso.Enabled = _pickFolder.Enabled = _build.Enabled = _install.Enabled = _refresh.Enabled = !busy;
        _cancel.Enabled = busy;
        UseWaitCursor = busy;
    }

    static void Ok(Label l, string s) { l.ForeColor = Color.FromArgb(0, 128, 60); l.Text = s; }
    static void Fail(Label l, string s) { l.ForeColor = Color.Firebrick; l.Text = s; }
    static void Info(Label l, string s) { l.ForeColor = SystemColors.ControlText; l.Text = s; }

    /// <summary>Windows Portable Devices (MTP) is COM: keep each use on one STA thread.</summary>
    static Task<T> RunSta<T>(Func<T> f)
    {
        var tcs = new TaskCompletionSource<T>();
        var t = new Thread(() =>
        {
            try { tcs.SetResult(f()); }
            catch (Exception e) { tcs.SetException(e); }
        });
        t.SetApartmentState(ApartmentState.STA);
        t.IsBackground = true;
        t.Start();
        return tcs.Task;
    }
}
