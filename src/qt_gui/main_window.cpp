// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDockWidget>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QSysInfo>
#include <QTimer>

#ifdef _WIN32
#include <Windows.h>
#include <psapi.h>
#endif

#include "about_dialog.h"
#include "cheats_patches.h"
#include "version_dialog.h"
#ifdef ENABLE_UPDATER
#include "check_update.h"
#endif
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/versions.h"
#include "control_settings.h"
#include "core/emulator_state.h"
#include "core/file_sys/game_backend.h"
#include "crypto_key_dialog.h"
#include "dimensions_dialog.h"
#include "game_install_dialog.h"
#include "hotkeys.h"
#include "infinity_dialog.h"
#include "input/input.h"
#include "ipc/ipc_client.h"
#include "kbm_gui.h"
#include "main_window.h"
#include "settings_dialog.h"
#include "skylander_dialog.h"
#include "user_manager_dialog.h"

namespace {

QString SanitizeCaptureComponent(QString value) {
    static const QRegularExpression invalid_chars(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));

    value = value.trimmed();
    value.replace(invalid_chars, QStringLiteral("_"));
    while (value.endsWith(' ') || value.endsWith('.')) {
        value.chop(1);
    }
    return value.isEmpty() ? QStringLiteral("Generic") : value;
}

#ifdef _WIN32
struct ProcessWindowSearchData {
    DWORD process_id;
    HWND window{nullptr};
};

BOOL CALLBACK FindProcessMainWindowCallback(HWND hwnd, LPARAM lparam) {
    auto* data = reinterpret_cast<ProcessWindowSearchData*>(lparam);
    DWORD window_process_id = 0;
    GetWindowThreadProcessId(hwnd, &window_process_id);
    if (window_process_id != data->process_id || !IsWindowVisible(hwnd) ||
        GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    wchar_t class_name[256]{};
    GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    const std::wstring_view window_class(class_name);
    if (window_class == L"PseudoConsoleWindow" || window_class == L"ConsoleWindowClass") {
        return TRUE;
    }

    data->window = hwnd;
    return FALSE;
}

HWND FindMainWindowForProcess(DWORD process_id) {
    ProcessWindowSearchData data{process_id, nullptr};
    EnumWindows(&FindProcessMainWindowCallback, reinterpret_cast<LPARAM>(&data));
    return data.window;
}

QPixmap CaptureGameWindowPixmap(HWND hwnd) {
    RECT window_rect{};
    if (!hwnd || !GetWindowRect(hwnd, &window_rect)) {
        return {};
    }

    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    HDC screen_dc = GetDC(nullptr);
    if (width <= 0 || height <= 0 || !screen_dc) {
        return {};
    }

    HDC memory_dc = CreateCompatibleDC(screen_dc);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = memory_dc ? CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bits,
                                                  nullptr, 0)
                               : nullptr;
    QPixmap result;
    if (bitmap && bits) {
        const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
        constexpr UINT render_full_content = 0x00000002u;
        PrintWindow(hwnd, memory_dc, render_full_content);
        GdiFlush();
        const QImage image(static_cast<const uchar*>(bits), width, height, width * 4,
                           QImage::Format_RGB32);
        result = QPixmap::fromImage(image.copy());
        SelectObject(memory_dc, old_bitmap);
    }

    if (bitmap)
        DeleteObject(bitmap);
    if (memory_dc)
        DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return result;
}
#endif

} // namespace

MainWindow::MainWindow(QWidget* parent, bool log_to_terminal)
    : QMainWindow(parent), ui(new Ui::MainWindow),
      m_ipc_client(std::make_shared<IpcClient>(nullptr, log_to_terminal)) {
    ui->setupUi(this);
    installEventFilter(this);
    setAttribute(Qt::WA_DeleteOnClose);
    m_gui_settings = std::make_shared<gui_settings>();
    ui->toggleLabelsAct->setChecked(
        m_gui_settings->GetValue(gui::mw_showLabelsUnderIcons).toBool());

    m_ipc_client->gameClosedFunc = [this]() { onGameClosed(); };
    m_ipc_client->restartEmulatorFunc = [this]() { RestartEmulator(); };
    m_ipc_client->startGameFunc = [this]() { RunGame(); };
}

MainWindow::~MainWindow() {
    SaveWindowState();
}

bool MainWindow::Init() {
    auto start = std::chrono::steady_clock::now();
    // setup ui
    LoadTranslation();
    AddUiWidgets();
    CreateActions();
    CreateRecentGameActions();
    ConfigureGuiFromSettings();
    CreateDockWindows(true);
    CreateConnects();
    SetLastUsedTheme();
    SetLastIconSizeBullet();
    // show ui
    setMinimumSize(1280, 405);
    const std::string_view revision(Common::g_scm_rev);
    const std::string window_title =
        fmt::format("shadPS4QtLauncher - build {} - {}", revision.substr(0, 7),
                    Common::g_scm_date);
    setWindowTitle(QString::fromStdString(window_title));
    this->show();
    // load game list
    LoadGameLists();

#ifdef ENABLE_UPDATER
    // Check for update
    CheckUpdateMain(true);
#endif

    LoadVersionComboBox();
    if (m_gui_settings->GetValue(gui::vm_checkOnStartup).toBool()) {
        auto versionDialog = new VersionDialog(m_gui_settings, this);
        versionDialog->checkUpdatePre(false);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    statusBar.reset(new QStatusBar);
    this->setStatusBar(statusBar.data());
    // Update status bar
    int numGames = m_game_info->m_games.size();
    QString statusMessage = tr("Games: ") + QString::number(numGames) + " (" +
                            QString::number(duration.count()) + "ms)";
    statusBar->showMessage(statusMessage);

    return true;
}

void MainWindow::CreateActions() {
    // create action group for icon size
    m_icon_size_act_group = new QActionGroup(this);
    m_icon_size_act_group->addAction(ui->setIconSizeTinyAct);
    m_icon_size_act_group->addAction(ui->setIconSizeSmallAct);
    m_icon_size_act_group->addAction(ui->setIconSizeMediumAct);
    m_icon_size_act_group->addAction(ui->setIconSizeLargeAct);

    // create action group for list mode
    m_list_mode_act_group = new QActionGroup(this);
    m_list_mode_act_group->addAction(ui->setlistModeListAct);
    m_list_mode_act_group->addAction(ui->setlistModeGridAct);
    m_list_mode_act_group->addAction(ui->setlistElfAct);

    // create action group for themes
    m_theme_act_group = new QActionGroup(this);
    m_theme_act_group->addAction(ui->setThemeDark);
    m_theme_act_group->addAction(ui->setThemeLight);
    m_theme_act_group->addAction(ui->setThemeGreen);
    m_theme_act_group->addAction(ui->setThemeBlue);
    m_theme_act_group->addAction(ui->setThemeViolet);
    m_theme_act_group->addAction(ui->setThemeGruvbox);
    m_theme_act_group->addAction(ui->setThemeTokyoNight);
    m_theme_act_group->addAction(ui->setThemeOled);
}

void MainWindow::PauseGame() {
    if (!m_ipc_client->isProcessRunning()) {
        return;
    }
    if (is_paused) {
        m_ipc_client->resumeGame();
        is_paused = false;
    } else {
        m_ipc_client->pauseGame();
        is_paused = true;
    }
    UpdateToolbarButtons();
}

void MainWindow::StopGame() {
    if (!m_ipc_client->isProcessRunning()) {
        return;
    }
    m_ipc_client->stopEmulator();
    ScheduleForcedProcessShutdown();
}

void MainWindow::onGameClosed() {
    EmulatorState::GetInstance()->SetGameRunning(false);
    is_paused = false;
    UpdateToolbarButtons();

    // clear dialogs when game closed
    skylander_dialog* sky_diag = skylander_dialog::get_dlg(this, m_ipc_client);
    sky_diag->clear_all();
    dimensions_dialog* dim_diag = dimensions_dialog::get_dlg(this, m_ipc_client);
    dim_diag->clear_all();
    infinity_dialog* inf_diag = infinity_dialog::get_dlg(this, m_ipc_client);
    inf_diag->clear_all();

    if (exit_after_game_closes) {
        exit_after_game_closes = false;
        QTimer::singleShot(0, this, &QWidget::close);
    }
}

void MainWindow::RestartGame() {
    m_ipc_client->restartEmulator();
}

void MainWindow::toggleLabelsUnderIcons() {
    bool showLabels = ui->toggleLabelsAct->isChecked();
    m_gui_settings->SetValue(gui::mw_showLabelsUnderIcons, showLabels);
    UpdateToolbarLabels();
    if (EmulatorState::GetInstance()->IsGameRunning()) {
        UpdateToolbarButtons();
    }
}

void MainWindow::toggleFullscreen() {
    if (m_ipc_client->isProcessRunning()) {
        m_ipc_client->toggleFullscreen();
    }
}

void MainWindow::ExitApplication() {
    if (m_ipc_client->isProcessRunning()) {
        exit_after_game_closes = true;
        m_ipc_client->stopEmulator();
        ScheduleForcedProcessShutdown();
        return;
    }
    close();
}

void MainWindow::ScheduleForcedProcessShutdown() {
    QTimer::singleShot(3000, this, [this]() {
        if (m_ipc_client->isProcessRunning()) {
            m_ipc_client->killEmulator();
        }
    });
}

QString MainWindow::BuildCaptureTimestamp() const {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss_zzz"));
}

QString MainWindow::BuildCaptureGameFolderName() const {
    return SanitizeCaptureComponent(runningGameSerial.empty()
                                        ? QStringLiteral("Generic")
                                        : QString::fromStdString(runningGameSerial));
}

std::filesystem::path MainWindow::EnsureCaptureOutputDirectory() const {
    const auto output_path = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir) /
                             Common::FS::PathFromQString(BuildCaptureGameFolderName());
    std::filesystem::create_directories(output_path);
    return output_path;
}

quintptr MainWindow::GetRunningGameWindowId() const {
#ifdef _WIN32
    const qint64 process_id = m_ipc_client->processId();
    return process_id > 0
               ? reinterpret_cast<quintptr>(FindMainWindowForProcess(static_cast<DWORD>(process_id)))
               : 0;
#else
    return 0;
#endif
}

void MainWindow::SnapshotCapture() {
#ifndef _WIN32
    QMessageBox::information(this, tr("Screenshot"),
                             tr("Screenshot capture is currently implemented only on Windows."));
    return;
#else
    if (!m_ipc_client->isProcessRunning()) {
        if (statusBar) {
            statusBar->showMessage(tr("Start a game before taking a screenshot."), 3000);
        }
        return;
    }

    const quintptr window_id = GetRunningGameWindowId();
    if (window_id == 0) {
        QMessageBox::warning(this, tr("Screenshot"),
                             tr("Could not find the emulator window to capture."));
        return;
    }

    const int burst_count = qBound(1, m_snapshot_burst_spinbox->value(), 99);
    const QString timestamp = BuildCaptureTimestamp();
    const auto output_dir = EnsureCaptureOutputDirectory();
    int saved_count = 0;
    QString last_file;

    for (int index = 0; index < burst_count; ++index) {
        const QPixmap snapshot =
            CaptureGameWindowPixmap(reinterpret_cast<HWND>(window_id));
        if (snapshot.isNull()) {
            break;
        }

        const QString suffix = burst_count > 1
                                   ? QStringLiteral("_%1").arg(index + 1, 2, 10, QLatin1Char('0'))
                                   : QString();
        const auto output_path = output_dir / Common::FS::PathFromQString(
                                                  QStringLiteral("Snapshot_%1%2.png")
                                                      .arg(timestamp, suffix));
        Common::FS::PathToQString(last_file, output_path);
        if (!snapshot.save(last_file, "PNG")) {
            break;
        }
        ++saved_count;
    }

    if (saved_count == 0) {
        QMessageBox::warning(this, tr("Screenshot"), tr("The emulator window could not be captured."));
    } else if (saved_count == 1) {
        if (statusBar) {
            statusBar->showMessage(
                tr("Screenshot saved: %1").arg(QDir::toNativeSeparators(last_file)), 5000);
        }
    } else {
        if (statusBar) {
            statusBar->showMessage(tr("Screenshot burst saved: %1 images.").arg(saved_count),
                                   5000);
        }
    }
#endif
}

QString MainWindow::BuildSystemInfoText() const {
#ifdef _WIN32
    QSettings cpu_settings(
        "HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        QSettings::NativeFormat);
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    const QString cpu_name = cpu_settings.value("ProcessorNameString", tr("Unavailable"))
                                 .toString()
                                 .simplified();
    const qulonglong total_ram_mb = GlobalMemoryStatusEx(&memory_status)
                                        ? memory_status.ullTotalPhys / (1024ull * 1024ull)
                                        : 0;
    return tr("Operating System: %1\nCPU: %2\nTotal RAM: %3 MB")
        .arg(QSysInfo::prettyProductName(), cpu_name, QString::number(total_ram_mb));
#else
    return tr("Operating System: %1\nCPU: Unavailable\nTotal RAM: Unavailable")
        .arg(QSysInfo::prettyProductName());
#endif
}

void MainWindow::ShowSystemInfo() {
    QMessageBox::information(this, tr("System Information"), BuildSystemInfoText());
}

QWidget* MainWindow::createButtonWithLabel(QPushButton* button, const QString& labelText,
                                           bool showLabel) {
    QWidget* container = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(container);
    layout->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(button);

    QLabel* label = nullptr;
    if (showLabel && ui->toggleLabelsAct->isChecked()) {
        label = new QLabel(labelText, this);
        label->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
        layout->addWidget(label);
        button->setToolTip("");
    } else {
        button->setToolTip(labelText);
    }

    container->setLayout(layout);
    container->setProperty("buttonLabel", QVariant::fromValue(label));
    return container;
}

QWidget* createSpacer(QWidget* parent) {
    QWidget* spacer = new QWidget(parent);
    spacer->setFixedWidth(15);
    spacer->setFixedHeight(15);
    return spacer;
}

void MainWindow::AddUiWidgets() {
    // add toolbar widgets
    QApplication::setStyle("Fusion");

    bool showLabels = ui->toggleLabelsAct->isChecked();
    ui->toolBar->clear();
    ui->mw_searchbar->hide();
    ui->refreshButton->hide();
    ui->controllerButton->hide();
    ui->keyboardButton->hide();

    if (!m_snapshot_burst_spinbox) {
        m_snapshot_burst_spinbox = new QSpinBox(this);
        m_snapshot_burst_spinbox->setRange(1, 99);
        m_snapshot_burst_spinbox->setAccelerated(true);
        m_snapshot_burst_spinbox->setAlignment(Qt::AlignCenter);
        m_snapshot_burst_spinbox->setValue(1);
        m_snapshot_burst_spinbox->setFixedWidth(68);
    }

    ui->sizeSliderContainer->setFixedWidth(150);
    const auto add_button = [this, showLabels](QPushButton* button, const QString& label) {
        button->setAccessibleName(label);
        ui->toolBar->addWidget(createButtonWithLabel(button, label, showLabels));
    };
    const auto add_labeled_widget = [this, showLabels](QWidget* widget, const QString& label) {
        widget->setAccessibleName(label);
        auto* container = new QWidget(this);
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setAlignment(Qt::AlignCenter);
        layout->addWidget(widget);
        if (showLabels) {
            auto* text = new QLabel(label, container);
            text->setAlignment(Qt::AlignCenter);
            layout->addWidget(text);
        } else {
            widget->setToolTip(label);
        }
        ui->toolBar->addWidget(container);
    };

    add_button(ui->playButton, tr("Play"));
    add_button(ui->pauseButton, tr("Pause"));
    add_button(ui->stopButton, tr("Stop"));
    add_button(ui->restartButton, tr("Restart"));
    add_button(ui->exitButton, tr("Terminate"));
    add_button(ui->fullscreenButton, tr("Fullscreen"));
    add_button(ui->settingsButton, tr("Settings"));
    add_button(ui->systemInfoButton, tr("Info"));
    add_button(ui->snapshotButton, tr("Screenshot"));
    add_labeled_widget(m_snapshot_burst_spinbox, tr("Burst"));
    ui->sizeSlider->setAccessibleName(tr("Icon Size"));
    add_labeled_widget(ui->sizeSliderContainer, tr("Icon Size"));

    auto* versionArea = new QWidget(this);
    versionArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* versionAreaLayout = new QHBoxLayout(versionArea);
    versionAreaLayout->setContentsMargins(0, 0, 4, 0);
    versionAreaLayout->addStretch();

    auto* versionContainer = new QWidget(versionArea);
    versionContainer->setFixedWidth(180);
    delete ui->versionComboBox;
    delete ui->versionManagerButton;
    ui->versionComboBox = new QComboBox(versionContainer);
    ui->versionComboBox->setObjectName("versionComboBox");
    ui->versionComboBox->setAccessibleName(tr("Emulator Version"));
    ui->versionManagerButton = new QPushButton(versionContainer);
    ui->versionManagerButton->setObjectName("versionManagerButton");
    ui->versionManagerButton->setAccessibleName(tr("Version Manager"));
    auto* versionLayout = new QVBoxLayout(versionContainer);
    versionLayout->setContentsMargins(0, 0, 0, 0);
    versionLayout->addWidget(ui->versionComboBox);
    versionLayout->addWidget(ui->versionManagerButton);
    ui->versionComboBox->setMinimumWidth(180);
    ui->versionManagerButton->setMinimumWidth(180);
    ui->versionManagerButton->setText(tr("Version Manager"));
    versionAreaLayout->addWidget(versionContainer);
    ui->toolBar->addWidget(versionArea);
    ui->versionComboBox->show();
    ui->versionManagerButton->show();
    versionContainer->show();
    versionArea->show();
    UpdateToolbarButtons();
}

void MainWindow::UpdateToolbarButtons() {
    const bool game_running = m_ipc_client->isProcessRunning();

    if (is_paused) {
        ui->pauseButton->setIcon(ui->playButton->icon());
        ui->pauseButton->setToolTip(tr("Resume"));
    } else {
        if (isIconBlack) {
            ui->pauseButton->setIcon(QIcon(":images/pause_icon.png"));
        } else {
            ui->pauseButton->setIcon(RecolorIcon(QIcon(":images/pause_icon.png"), isWhite));
        }
        ui->pauseButton->setToolTip(tr("Pause"));
    }

    if (ui->toggleLabelsAct->isChecked()) {
        QLabel* pauseButtonLabel = ui->pauseButton->parentWidget()->findChild<QLabel*>();
        if (pauseButtonLabel) {
            pauseButtonLabel->setText(is_paused ? tr("Resume") : tr("Pause"));
        }
    }

    ui->playButton->setEnabled(!game_running);
    ui->pauseButton->setEnabled(game_running);
    ui->stopButton->setEnabled(game_running);
    ui->restartButton->setEnabled(game_running);
    ui->fullscreenButton->setEnabled(game_running);
    ui->snapshotButton->setEnabled(game_running);
    m_snapshot_burst_spinbox->setEnabled(game_running);
}

void MainWindow::UpdateToolbarLabels() {
    AddUiWidgets();
}

void MainWindow::CreateDockWindows(bool newDock) {
    // place holder widget is needed for good health they say :)
    QWidget* phCentralWidget = new QWidget(this);
    setCentralWidget(phCentralWidget);

    QWidget* dockContents = new QWidget(this);
    QVBoxLayout* dockLayout = new QVBoxLayout(this);

    ui->splitter = new QSplitter(Qt::Vertical);
    ui->logDisplay = new QTextEdit(ui->splitter);
    ui->logDisplay->setText(tr("Game Log"));
    ui->logDisplay->setReadOnly(true);

    if (newDock) {
        m_dock_widget.reset(new QDockWidget(tr("Game List"), this));
        m_game_list_frame.reset(
            new GameListFrame(m_gui_settings, m_game_info, m_compat_info, m_ipc_client, this));
        m_game_list_frame->setObjectName("gamelist");
        m_game_grid_frame.reset(
            new GameGridFrame(m_gui_settings, m_game_info, m_compat_info, m_ipc_client, this));
        m_game_grid_frame->setObjectName("gamegridlist");
        m_elf_viewer.reset(new ElfViewer(m_gui_settings, this));
        m_elf_viewer->setObjectName("elflist");
    }

    int table_mode = m_gui_settings->GetValue(gui::gl_mode).toInt();
    int slider_pos = 0;
    if (table_mode == 0) { // List
        m_game_grid_frame->hide();
        m_elf_viewer->hide();
        m_game_list_frame->show();
        if (!newDock) {
            m_game_list_frame->clearContents();
            m_game_list_frame->PopulateGameList();
        }
        ui->splitter->addWidget(m_game_list_frame.data());
        slider_pos = m_gui_settings->GetValue(gui::gl_slider_pos).toInt();
        ui->sizeSlider->setSliderPosition(slider_pos); // set slider pos at start;
        isTableList = true;
    } else if (table_mode == 1) { // Grid
        m_game_list_frame->hide();
        m_elf_viewer->hide();
        m_game_grid_frame->show();
        if (!newDock) {
            if (m_game_grid_frame->item(0, 0) == nullptr) {
                m_game_grid_frame->clearContents();
                m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            }
        }
        ui->splitter->addWidget(m_game_grid_frame.data());
        slider_pos = m_gui_settings->GetValue(gui::gg_slider_pos).toInt();
        ui->sizeSlider->setSliderPosition(slider_pos); // set slider pos at start;
        isTableList = false;
    } else {
        m_game_list_frame->hide();
        m_game_grid_frame->hide();
        m_elf_viewer->show();
        ui->splitter->addWidget(m_elf_viewer.data());
        isTableList = false;
    }

    QPalette logPalette = ui->logDisplay->palette();
    logPalette.setColor(QPalette::Base, Qt::black);
    ui->logDisplay->setPalette(logPalette);
    ui->splitter->addWidget(ui->logDisplay);

    QList<int> defaultSizes = {800, 200}; // these are proportionally adjusted by qt
    QList<int> sizes = gui_settings::Var2IntList(m_gui_settings->GetValue(
        gui::main_window, "dockWidgetSizes", QVariant::fromValue(defaultSizes)));
    if (sizes.size() > 0 &&
        sizes[1] == 0) { // This happens if log is hidden when settings are saved
        sizes = defaultSizes;
    }

    ui->splitter->setSizes({sizes});
    ui->splitter->setCollapsible(0, false);
    ui->splitter->setCollapsible(1, false);

    bool showLog = ui->showLogAct->isChecked();
    showLog ? ui->logDisplay->show() : ui->logDisplay->hide();

    dockLayout->addWidget(ui->splitter);
    dockContents->setLayout(dockLayout);
    m_dock_widget->setWidget(dockContents);

    m_dock_widget->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_dock_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_dock_widget->resize(this->width(), this->height());

    addDockWidget(Qt::LeftDockWidgetArea, m_dock_widget.data());
    this->setDockNestingEnabled(true);
}

void MainWindow::LoadGameLists() {
    // Load compatibility database
    m_compat_info->LoadCompatibilityFile();

    // Update compatibility database
    if (m_gui_settings->GetValue(gui::gen_checkCompatibilityAtStartup).toBool())
        m_compat_info->UpdateCompatibilityDatabase(this);

    // Get game info from game folders.
    m_game_info->GetGameInfo(this);
    if (isTableList) {
        m_game_list_frame->PopulateGameList();
    } else {
        m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
    }
}

#ifdef ENABLE_UPDATER
void MainWindow::CheckUpdateMain(bool checkSave) {
    if (checkSave) {
        if (!m_gui_settings->GetValue(gui::gen_checkForUpdates).toBool()) {
            return;
        }
    }
    auto checkUpdate = new CheckUpdate(m_gui_settings, false);
    checkUpdate->exec();
}
#endif

void MainWindow::CreateConnects() {
    connect(this, &MainWindow::WindowResized, this, &MainWindow::HandleResize);
    connect(ui->mw_searchbar, &QLineEdit::textChanged, this, &MainWindow::SearchGameTable);
    connect(ui->exitAct, &QAction::triggered, this, &MainWindow::ExitApplication);
    connect(ui->refreshGameListAct, &QAction::triggered, this, &MainWindow::RefreshGameTable);
    connect(ui->refreshButton, &QPushButton::clicked, this, &MainWindow::RefreshGameTable);
    connect(ui->showGameListAct, &QAction::triggered, this, &MainWindow::ShowGameList);
    connect(ui->toggleLabelsAct, &QAction::toggled, this, &MainWindow::toggleLabelsUnderIcons);
    connect(ui->fullscreenButton, &QPushButton::clicked, this, &MainWindow::toggleFullscreen);
    connect(ui->exitButton, &QPushButton::clicked, this, &MainWindow::ExitApplication);
    connect(ui->systemInfoButton, &QPushButton::clicked, this, &MainWindow::ShowSystemInfo);
    connect(ui->snapshotButton, &QPushButton::clicked, this, &MainWindow::SnapshotCapture);

    auto* snapshot_hotkey_action = new QAction(this);
    snapshot_hotkey_action->setShortcut(QKeySequence(Qt::Key_F12));
    snapshot_hotkey_action->setShortcutContext(Qt::ApplicationShortcut);
    addAction(snapshot_hotkey_action);
    connect(snapshot_hotkey_action, &QAction::triggered, this, &MainWindow::SnapshotCapture);

    connect(ui->showLogAct, &QAction::triggered, this, [this](bool state) {
        if (state) {
            ui->logDisplay->show();
            m_gui_settings->SetValue(gui::mw_showLog, true);
        } else {
            ui->logDisplay->hide();
            m_gui_settings->SetValue(gui::mw_showLog, false);
        }
    });

    connect(ui->sizeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (isTableList) {
            m_game_list_frame->icon_size =
                48 + value; // 48 is the minimum icon size to use due to text disappearing.
            m_game_list_frame->ResizeIcons(48 + value);
            m_gui_settings->SetValue(gui::gl_icon_size, 48 + value);
            m_gui_settings->SetValue(gui::gl_slider_pos, value);
        } else {
            m_game_grid_frame->icon_size = 69 + value;
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            m_gui_settings->SetValue(gui::gg_icon_size, 69 + value);
            m_gui_settings->SetValue(gui::gg_slider_pos, value);
        }
    });

    connect(ui->shadFolderAct, &QAction::triggered, this, [this]() {
        QString userPath;
        Common::FS::PathToQString(userPath, Common::FS::GetUserPath(Common::FS::PathType::UserDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(userPath));
    });

    connect(ui->playButton, &QPushButton::clicked, this, &MainWindow::StartGame);
    connect(ui->pauseButton, &QPushButton::clicked, this, &MainWindow::PauseGame);
    connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::StopGame);
    connect(ui->restartButton, &QPushButton::clicked, this, &MainWindow::RestartGame);
    connect(m_game_grid_frame.get(), &QTableWidget::cellDoubleClicked, this,
            &MainWindow::StartGame);
    connect(m_game_list_frame.get(), &QTableWidget::cellDoubleClicked, this,
            &MainWindow::StartGame);

    connect(ui->configureAct, &QAction::triggered, this, [this]() {
        auto settingsDialog = new SettingsDialog(m_gui_settings, m_compat_info, m_ipc_client, this,
                                                 EmulatorState::GetInstance()->IsGameRunning());

        connect(settingsDialog, &SettingsDialog::LanguageChanged, this,
                &MainWindow::OnLanguageChanged);

        connect(settingsDialog, &SettingsDialog::CompatibilityChanged, this,
                &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::accepted, this, &MainWindow::RefreshGameTable);
        connect(settingsDialog, &SettingsDialog::rejected, this, &MainWindow::RefreshGameTable);
        connect(settingsDialog, &SettingsDialog::close, this, &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::BackgroundOpacityChanged, this,
                [this](int opacity) {
                    m_gui_settings->SetValue(gui::gl_backgroundImageOpacity,
                                             std::clamp(opacity, 0, 100));
                    if (m_game_list_frame) {
                        QTableWidgetItem* current = m_game_list_frame->GetCurrentItem();
                        if (current) {
                            m_game_list_frame->SetListBackgroundImage(current);
                        }
                    }
                    if (m_game_grid_frame) {
                        if (m_game_grid_frame->IsValidCellSelected()) {
                            m_game_grid_frame->SetGridBackgroundImage(m_game_grid_frame->crtRow,
                                                                      m_game_grid_frame->crtColumn);
                        }
                    }
                });

        settingsDialog->exec();
    });

    connect(ui->settingsButton, &QPushButton::clicked, this, [this]() {
        auto settingsDialog = new SettingsDialog(m_gui_settings, m_compat_info, m_ipc_client, this,
                                                 EmulatorState::GetInstance()->IsGameRunning());

        connect(settingsDialog, &SettingsDialog::LanguageChanged, this,
                &MainWindow::OnLanguageChanged);

        connect(settingsDialog, &SettingsDialog::CompatibilityChanged, this,
                &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::accepted, this, &MainWindow::RefreshGameTable);
        connect(settingsDialog, &SettingsDialog::rejected, this, &MainWindow::RefreshGameTable);
        connect(settingsDialog, &SettingsDialog::close, this, &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::BackgroundOpacityChanged, this,
                [this](int opacity) {
                    m_gui_settings->SetValue(gui::gl_backgroundImageOpacity,
                                             std::clamp(opacity, 0, 100));
                    if (m_game_list_frame) {
                        QTableWidgetItem* current = m_game_list_frame->GetCurrentItem();
                        if (current) {
                            m_game_list_frame->SetListBackgroundImage(current);
                        }
                    }
                    if (m_game_grid_frame) {
                        if (m_game_grid_frame->IsValidCellSelected()) {
                            m_game_grid_frame->SetGridBackgroundImage(m_game_grid_frame->crtRow,
                                                                      m_game_grid_frame->crtColumn);
                        }
                    }
                });

        settingsDialog->exec();
    });

    connect(ui->controllerButton, &QPushButton::clicked, this, [this]() {
        ControlSettings* remapWindow = new ControlSettings(
            m_game_info, m_ipc_client, EmulatorState::GetInstance()->IsGameRunning(),
            runningGameSerial, this);
        remapWindow->exec();
    });

    connect(ui->keyboardButton, &QPushButton::clicked, this, [this]() {
        auto kbmWindow =
            new KBMSettings(m_game_info, m_ipc_client,
                            EmulatorState::GetInstance()->IsGameRunning(), runningGameSerial, this);
        kbmWindow->exec();
    });

    connect(ui->versionManagerButton, &QPushButton::clicked, this, [this]() {
        auto versionDialog = new VersionDialog(m_gui_settings, this);
        connect(versionDialog, &QDialog::finished, this, [this](int) { LoadVersionComboBox(); });
        versionDialog->exec();
    });

#ifdef ENABLE_UPDATER
    connect(ui->updaterAct, &QAction::triggered, this, [this]() {
        auto checkUpdate = new CheckUpdate(m_gui_settings, true);
        checkUpdate->exec();
    });
#endif

    connect(ui->aboutAct, &QAction::triggered, this, [this]() {
        auto aboutDialog = new AboutDialog(m_gui_settings, this);
        aboutDialog->exec();
    });

    connect(ui->configureHotkeys, &QAction::triggered, this, [this]() {
        auto hotkeyDialog =
            new Hotkeys(m_ipc_client, EmulatorState::GetInstance()->IsGameRunning(), this);
        hotkeyDialog->exec();
    });

    connect(ui->userManager, &QAction::triggered, this, [this]() {
        auto userDialog = new UserManagerDialog(this);
        userDialog->exec();
    });

    connect(ui->keyManager, &QAction::triggered, this, [this]() {
        auto keyDialog = new CryptoManagerDialog(this);
        keyDialog->exec();
    });

    connect(ui->setIconSizeTinyAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size =
                36; // 36 is the minimum icon size to use due to text disappearing.
            ui->sizeSlider->setValue(0); // icone_size - 36
            m_gui_settings->SetValue(gui::gl_icon_size, 36);
            m_gui_settings->SetValue(gui::gl_slider_pos, 0);
        } else {
            m_game_grid_frame->icon_size = 69;
            ui->sizeSlider->setValue(0); // icone_size - 36
            m_gui_settings->SetValue(gui::gg_icon_size, 69);
            m_gui_settings->SetValue(gui::gg_slider_pos, 9);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });

    connect(ui->setIconSizeSmallAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 64;
            ui->sizeSlider->setValue(28);
            m_gui_settings->SetValue(gui::gl_icon_size, 64);
            m_gui_settings->SetValue(gui::gl_slider_pos, 28);
        } else {
            m_game_grid_frame->icon_size = 97;
            ui->sizeSlider->setValue(28);
            m_gui_settings->SetValue(gui::gg_icon_size, 97);
            m_gui_settings->SetValue(gui::gg_slider_pos, 28);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });

    connect(ui->setIconSizeMediumAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 128;
            ui->sizeSlider->setValue(92);
            m_gui_settings->SetValue(gui::gl_icon_size, 128);
            m_gui_settings->SetValue(gui::gl_slider_pos, 92);
        } else {
            m_game_grid_frame->icon_size = 161;
            ui->sizeSlider->setValue(92);
            m_gui_settings->SetValue(gui::gg_icon_size, 161);
            m_gui_settings->SetValue(gui::gg_slider_pos, 92);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });

    connect(ui->setIconSizeLargeAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 256;
            ui->sizeSlider->setValue(220);
            m_gui_settings->SetValue(gui::gl_icon_size, 256);
            m_gui_settings->SetValue(gui::gl_slider_pos, 220);
        } else {
            m_game_grid_frame->icon_size = 256;
            ui->sizeSlider->setValue(220);
            m_gui_settings->SetValue(gui::gg_icon_size, 256);
            m_gui_settings->SetValue(gui::gg_slider_pos, 220);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });

    // handle resize like this for now, we deal with it when we add more docks
    connect(this, &MainWindow::WindowResized, this, [&]() {
        this->resizeDocks({m_dock_widget.data()}, {this->width()}, Qt::Orientation::Horizontal);
    });

    // List
    connect(ui->setlistModeListAct, &QAction::triggered, m_dock_widget.data(), [this]() {
        ui->sizeSlider->setEnabled(true);
        BackgroundMusicPlayer::getInstance().stopMusic();
        QList<int> sizes = {ui->splitter->sizes()};
        m_gui_settings->SetValue(gui::mw_dockWidgetSizes, QVariant::fromValue(sizes));
        m_gui_settings->SetValue(gui::gl_mode, 0);
        CreateDockWindows(false);
        ui->mw_searchbar->setText("");
        SetLastIconSizeBullet();
    });
    // Grid
    connect(ui->setlistModeGridAct, &QAction::triggered, m_dock_widget.data(), [this]() {
        ui->sizeSlider->setEnabled(true);
        BackgroundMusicPlayer::getInstance().stopMusic();
        QList<int> sizes = {ui->splitter->sizes()};
        m_gui_settings->SetValue(gui::mw_dockWidgetSizes, QVariant::fromValue(sizes));
        m_gui_settings->SetValue(gui::gl_mode, 1);
        CreateDockWindows(false);
        ui->mw_searchbar->setText("");
        SetLastIconSizeBullet();
    });
    // Elf Viewer
    connect(ui->setlistElfAct, &QAction::triggered, m_dock_widget.data(), [this]() {
        ui->sizeSlider->setEnabled(false);
        BackgroundMusicPlayer::getInstance().stopMusic();
        QList<int> sizes = {ui->splitter->sizes()};
        m_gui_settings->SetValue(gui::mw_dockWidgetSizes, QVariant::fromValue(sizes));
        m_gui_settings->SetValue(gui::gl_mode, 2);
        CreateDockWindows(false);
        SetLastIconSizeBullet();
    });

    // Cheats/Patches Download.
    connect(ui->downloadCheatsPatchesAct, &QAction::triggered, this, [this]() {
        QDialog* panelDialog = new QDialog(this);
        QVBoxLayout* layout = new QVBoxLayout(panelDialog);
        QPushButton* downloadAllCheatsButton =
            new QPushButton(tr("Download Cheats For All Installed Games"), panelDialog);
        QPushButton* downloadAllPatchesButton =
            new QPushButton(tr("Download Patches For All Games"), panelDialog);

        layout->addWidget(downloadAllCheatsButton);
        layout->addWidget(downloadAllPatchesButton);

        panelDialog->setLayout(layout);

        connect(downloadAllCheatsButton, &QPushButton::clicked, this, [this, panelDialog]() {
            QEventLoop eventLoop;
            int pendingDownloads = 0;

            auto onDownloadFinished = [&]() {
                if (--pendingDownloads <= 0) {
                    eventLoop.quit();
                }
            };

            for (const GameInfo& game : m_game_info->m_games) {
                QString empty = "";
                QString gameSerial = QString::fromStdString(game.serial);
                QString gameVersion = QString::fromStdString(game.version);

                CheatsPatches* cheatsPatches = new CheatsPatches(
                    m_gui_settings, m_ipc_client, empty, empty, empty, empty, empty, nullptr);
                connect(cheatsPatches, &CheatsPatches::downloadFinished, onDownloadFinished);

                pendingDownloads += 2;

                cheatsPatches->downloadCheats("GoldHEN", gameSerial, gameVersion, false);
                cheatsPatches->downloadCheats("shadPS4", gameSerial, gameVersion, false);
            }
            eventLoop.exec();

            QMessageBox::information(
                nullptr, tr("Download Complete"),
                tr("You have downloaded cheats for all the games you have installed."));

            panelDialog->accept();
        });
        connect(downloadAllPatchesButton, &QPushButton::clicked, [this, panelDialog]() {
            QEventLoop eventLoop;
            int pendingDownloads = 0;

            auto onDownloadFinished = [&]() {
                if (--pendingDownloads <= 0) {
                    eventLoop.quit();
                }
            };

            QString empty = "";
            CheatsPatches* cheatsPatches = new CheatsPatches(m_gui_settings, m_ipc_client, empty,
                                                             empty, empty, empty, empty, nullptr);
            connect(cheatsPatches, &CheatsPatches::downloadFinished, onDownloadFinished);

            pendingDownloads += 2;

            cheatsPatches->downloadPatches("GoldHEN", false);
            cheatsPatches->downloadPatches("shadPS4", false);

            eventLoop.exec();
            QMessageBox::information(
                nullptr, tr("Download Complete"),
                QString(tr("Patches Downloaded Successfully!") + "\n" +
                        tr("All Patches available for all games have been downloaded.")));
            cheatsPatches->createFilesJson("GoldHEN");
            cheatsPatches->createFilesJson("shadPS4");
            panelDialog->accept();
        });
        panelDialog->exec();
    });

    // Dump game list.
    connect(ui->dumpGameListAct, &QAction::triggered, this, [&] {
        QString filePath = qApp->applicationDirPath().append("/GameList.txt");
        QFile file(filePath);
        QTextStream out(&file);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qDebug() << "Failed to open file for writing:" << file.errorString();
            return;
        }
        out << QString("%1 %2 %3 %4 %5\n")
                   .arg("          NAME", -50)
                   .arg("    ID", -10)
                   .arg("FW", -4)
                   .arg(" APP VERSION", -11)
                   .arg("                Path");
        for (const GameInfo& game : m_game_info->m_games) {
            QString game_path;
            Common::FS::PathToQString(game_path, game.path);
            out << QString("%1 %2 %3     %4 %5\n")
                       .arg(QString::fromStdString(game.name), -50)
                       .arg(QString::fromStdString(game.serial), -10)
                       .arg(QString::fromStdString(game.fw), -4)
                       .arg(QString::fromStdString(game.version), -11)
                       .arg(game_path);
        }
    });

    // Package install.
    connect(ui->bootGameAct, &QAction::triggered, this, &MainWindow::BootGame);
    connect(ui->gameInstallPathAct, &QAction::triggered, this, &MainWindow::InstallDirectory);

    // elf viewer
    connect(ui->addElfFolderAct, &QAction::triggered, m_elf_viewer.data(),
            &ElfViewer::OpenElfFolder);

    // Trophy Viewer
    connect(ui->trophyViewerAct, &QAction::triggered, this, [this]() {
        if (m_game_info->m_games.empty()) {
            QMessageBox::information(
                this, tr("Trophy Viewer"),
                tr("No games found. Please add your games to your library first."));
            return;
        }

        const auto& firstGame = m_game_info->m_games[0];
        QString trophyPath, gameTrpPath;
        Common::FS::PathToQString(trophyPath, firstGame.serial);
        Common::FS::PathToQString(gameTrpPath, firstGame.path);

        auto game_update_path = Common::FS::PathFromQString(gameTrpPath);
        game_update_path += "-UPDATE";
        if (std::filesystem::exists(game_update_path)) {
            Common::FS::PathToQString(gameTrpPath, game_update_path);
        } else {
            game_update_path = Common::FS::PathFromQString(gameTrpPath);
            game_update_path += "-patch";
            if (std::filesystem::exists(game_update_path)) {
                Common::FS::PathToQString(gameTrpPath, game_update_path);
            }
        }

        QVector<TrophyGameInfo> allTrophyGames;
        for (const auto& game : m_game_info->m_games) {
            TrophyGameInfo gameInfo;
            gameInfo.name = QString::fromStdString(game.name);
            Common::FS::PathToQString(gameInfo.trophyPath, game.serial);
            Common::FS::PathToQString(gameInfo.gameTrpPath, game.path);

            auto update_path = Common::FS::PathFromQString(gameInfo.gameTrpPath);
            update_path += "-UPDATE";
            if (std::filesystem::exists(update_path)) {
                Common::FS::PathToQString(gameInfo.gameTrpPath, update_path);
            } else {
                update_path = Common::FS::PathFromQString(gameInfo.gameTrpPath);
                update_path += "-patch";
                if (std::filesystem::exists(update_path)) {
                    Common::FS::PathToQString(gameInfo.gameTrpPath, update_path);
                }
            }

            allTrophyGames.append(gameInfo);
        }

        QString gameName = QString::fromStdString(firstGame.name);
        TrophyViewer* trophyViewer =
            new TrophyViewer(m_gui_settings, trophyPath, gameTrpPath, gameName, allTrophyGames);
        trophyViewer->show();
    });

    // Manage Skylanders
    connect(ui->skylanderPortalAction, &QAction::triggered, this, [this]() {
        skylander_dialog* sky_diag = skylander_dialog::get_dlg(this, m_ipc_client);
        sky_diag->show();
    });

    // Manage Infinity Figures
    connect(ui->infinityFiguresAction, &QAction::triggered, this, [this]() {
        infinity_dialog* inf_diag = infinity_dialog::get_dlg(this, m_ipc_client);
        inf_diag->show();
    });

    // Manage Dimensions Toypad
    connect(ui->dimensionsToypadAction, &QAction::triggered, this, [this]() {
        dimensions_dialog* dim_dialog = dimensions_dialog::get_dlg(this, m_ipc_client);
        dim_dialog->show();
    });

    // Themes
    connect(ui->setThemeDark, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::Dark, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::Dark));
        if (isIconBlack) {
            SetUiIcons(false);
            isIconBlack = false;
        }
    });
    connect(ui->setThemeLight, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::Light, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::Light));
        if (!isIconBlack) {
            SetUiIcons(true);
            isIconBlack = true;
        }
    });
    connect(ui->setThemeGreen, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::Green, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::Green));
        if (isIconBlack) {
            SetUiIcons(false);
            isIconBlack = false;
        }
    });
    connect(ui->setThemeBlue, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::Blue, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::Blue));
        if (isIconBlack) {
            SetUiIcons(false);
            isIconBlack = false;
        }
    });
    connect(ui->setThemeViolet, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::Violet, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::Violet));
        if (isIconBlack) {
            SetUiIcons(false);
            isIconBlack = false;
        }
    });
    connect(ui->setThemeGruvbox, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::Gruvbox, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::Gruvbox));
        if (isIconBlack) {
            SetUiIcons(false);
            isIconBlack = false;
        }
    });
    connect(ui->setThemeTokyoNight, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::TokyoNight, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::TokyoNight));
        if (isIconBlack) {
            SetUiIcons(false);
            isIconBlack = false;
        }
    });
    connect(ui->setThemeOled, &QAction::triggered, &m_window_themes, [this]() {
        m_window_themes.SetWindowTheme(Theme::Oled, ui->mw_searchbar);
        m_gui_settings->SetValue(gui::gen_theme, static_cast<int>(Theme::Oled));
        if (isIconBlack) {
            SetUiIcons(false);
            isIconBlack = false;
        }
    });

    QObject::connect(m_ipc_client.get(), &IpcClient::LogEntrySent, this, &MainWindow::PrintLog);
}

void MainWindow::PrintLog(QString entry, QColor textColor) {
    ui->logDisplay->setTextColor(textColor);
    ui->logDisplay->append(entry);
    QScrollBar* sb = ui->logDisplay->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::StartGameWithArgs(QStringList args) {
    BackgroundMusicPlayer::getInstance().stopMusic();
    QString gamePath = "";
    int table_mode = m_gui_settings->GetValue(gui::gl_mode).toInt();
    if (table_mode == 0) {
        if (m_game_list_frame->currentItem()) {
            int itemID = m_game_list_frame->currentItem()->row();
            Common::FS::PathToQString(gamePath, m_game_info->m_games[itemID].path / "eboot.bin");
            runningGameSerial = m_game_info->m_games[itemID].serial;
        }
    } else if (table_mode == 1) {
        if (m_game_grid_frame->cellClicked) {
            int itemID = (m_game_grid_frame->crtRow * m_game_grid_frame->columnCnt) +
                         m_game_grid_frame->crtColumn;
            Common::FS::PathToQString(gamePath, m_game_info->m_games[itemID].path / "eboot.bin");
            runningGameSerial = m_game_info->m_games[itemID].serial;
        }
    } else {
        if (m_elf_viewer->currentItem()) {
            int itemID = m_elf_viewer->currentItem()->row();
            gamePath = m_elf_viewer->m_elf_list[itemID];
        }
    }
    if (gamePath != "") {
        AddRecentFiles(gamePath);
        const auto path = Common::FS::PathFromQString(gamePath);
        const auto archive_root = path.parent_path();
        const bool eboot_present =
            Core::FileSys::IsZArchiveFile(archive_root)
                ? Core::FileSys::ReadGameFile(archive_root, "eboot.bin").has_value()
                : std::filesystem::exists(path);
        if (!eboot_present) {
            QMessageBox::critical(nullptr, tr("Run Game"), QString(tr("Eboot.bin file not found")));
            return;
        }
        StartEmulator(path, args);

        UpdateToolbarButtons();
    }
}

void MainWindow::StartGame() {
    StartGameWithArgs({});
}

bool isTable;
void MainWindow::SearchGameTable(const QString& text) {
    if (isTableList) {
        if (isTable != true) {
            m_game_info->m_games = m_game_info->m_games_backup;
            m_game_list_frame->PopulateGameList();
            isTable = true;
        }
        for (int row = 0; row < m_game_list_frame->rowCount(); row++) {
            QString game_name = QString::fromStdString(m_game_info->m_games[row].name);
            bool match = (game_name.contains(text, Qt::CaseInsensitive)); // Check only in column 1
            m_game_list_frame->setRowHidden(row, !match);
        }
    } else {
        isTable = false;
        m_game_info->m_games = m_game_info->m_games_backup;
        m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);

        QVector<GameInfo> filteredGames;
        for (const auto& gameInfo : m_game_info->m_games) {
            QString game_name = QString::fromStdString(gameInfo.name);
            if (game_name.contains(text, Qt::CaseInsensitive)) {
                filteredGames.push_back(gameInfo);
            }
        }
        std::sort(filteredGames.begin(), filteredGames.end(), m_game_info->CompareStrings);
        m_game_info->m_games = filteredGames;
        m_game_grid_frame->PopulateGameGrid(filteredGames, true);
    }
}

void MainWindow::ShowGameList() {
    if (ui->showGameListAct->isChecked()) {
        RefreshGameTable();
    } else {
        m_game_grid_frame->clearContents();
        m_game_list_frame->clearContents();
    }
};

void MainWindow::RefreshGameTable() {
    // m_game_info->m_games.clear();
    m_game_info->GetGameInfo(this);
    m_game_list_frame->clearContents();
    m_game_list_frame->PopulateGameList();
    m_game_grid_frame->clearContents();
    m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
    statusBar->clearMessage();
    int numGames = m_game_info->m_games.size();
    QString statusMessage = tr("Games: ") + QString::number(numGames);
    statusBar->showMessage(statusMessage);
    m_game_list_frame->ToggleColumnVisibility();
}

void MainWindow::ConfigureGuiFromSettings() {
    if (!restoreGeometry(m_gui_settings->GetValue(gui::mw_geometry).toByteArray())) {
        // By default, set the window to 70% of the screen
        resize(QGuiApplication::primaryScreen()->availableSize() * 0.7);
    }
    ui->showGameListAct->setChecked(true);
    int table_mode = m_gui_settings->GetValue(gui::gl_mode).toInt();
    if (table_mode == 0) {
        ui->setlistModeListAct->setChecked(true);
    } else if (table_mode == 1) {
        ui->setlistModeGridAct->setChecked(true);
    } else if (table_mode == 2) {
        ui->setlistElfAct->setChecked(true);
    }

    bool showLog = m_gui_settings->GetValue(gui::mw_showLog).toBool();
    ui->showLogAct->setChecked(showLog);

    BackgroundMusicPlayer::getInstance().setVolume(
        m_gui_settings->GetValue(gui::gl_backgroundMusicVolume).toInt());
}

void MainWindow::SaveWindowState() {
    m_gui_settings->SetValue(gui::mw_geometry, saveGeometry(), false);
    QList<int> sizes = {ui->splitter->sizes()};
    m_gui_settings->SetValue(gui::mw_dockWidgetSizes, QVariant::fromValue(sizes));
}

void MainWindow::BootGame() {
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter(tr("ELF files (*.bin *.elf *.oelf)"));
    if (dialog.exec()) {
        QStringList fileNames = dialog.selectedFiles();
        int nFiles = fileNames.size();

        if (nFiles > 1) {
            QMessageBox::critical(nullptr, tr("Game Boot"),
                                  QString(tr("Only one file can be selected!")));
        } else {
            std::filesystem::path path = Common::FS::PathFromQString(fileNames[0]);
            if (!std::filesystem::exists(path)) {
                QMessageBox::critical(nullptr, tr("Run Game"),
                                      QString(tr("Eboot.bin file not found")));
                return;
            }
            StartEmulator(path);
        }
    }
}

void MainWindow::InstallDirectory() {
    GameInstallDialog dlg;
    dlg.exec();
    RefreshGameTable();
}

void MainWindow::SetLastUsedTheme() {
    Theme lastTheme = static_cast<Theme>(m_gui_settings->GetValue(gui::gen_theme).toInt());
    m_window_themes.SetWindowTheme(lastTheme, ui->mw_searchbar);

    switch (lastTheme) {
    case Theme::Light:
        ui->setThemeLight->setChecked(true);
        isIconBlack = true;
        break;
    case Theme::Dark:
        ui->setThemeDark->setChecked(true);
        isIconBlack = false;
        SetUiIcons(false);
        break;
    case Theme::Green:
        ui->setThemeGreen->setChecked(true);
        isIconBlack = false;
        SetUiIcons(false);
        break;
    case Theme::Blue:
        ui->setThemeBlue->setChecked(true);
        isIconBlack = false;
        SetUiIcons(false);
        break;
    case Theme::Violet:
        ui->setThemeViolet->setChecked(true);
        isIconBlack = false;
        SetUiIcons(false);
        break;
    case Theme::Gruvbox:
        ui->setThemeGruvbox->setChecked(true);
        isIconBlack = false;
        SetUiIcons(false);
        break;
    case Theme::TokyoNight:
        ui->setThemeTokyoNight->setChecked(true);
        isIconBlack = false;
        SetUiIcons(false);
        break;
    case Theme::Oled:
        ui->setThemeOled->setChecked(true);
        isIconBlack = false;
        SetUiIcons(false);
        break;
    }
}

void MainWindow::SetLastIconSizeBullet() {
    // set QAction bullet point if applicable
    int lastSize = m_gui_settings->GetValue(gui::gl_icon_size).toInt();
    int lastSizeGrid = m_gui_settings->GetValue(gui::gg_icon_size).toInt();
    if (isTableList) {
        switch (lastSize) {
        case 36:
            ui->setIconSizeTinyAct->setChecked(true);
            break;
        case 64:
            ui->setIconSizeSmallAct->setChecked(true);
            break;
        case 128:
            ui->setIconSizeMediumAct->setChecked(true);
            break;
        case 256:
            ui->setIconSizeLargeAct->setChecked(true);
            break;
        }
    } else {
        switch (lastSizeGrid) {
        case 69:
            ui->setIconSizeTinyAct->setChecked(true);
            break;
        case 97:
            ui->setIconSizeSmallAct->setChecked(true);
            break;
        case 161:
            ui->setIconSizeMediumAct->setChecked(true);
            break;
        case 256:
            ui->setIconSizeLargeAct->setChecked(true);
            break;
        }
    }
}

QIcon MainWindow::RecolorIcon(const QIcon& icon, bool isWhite) {
    QPixmap pixmap(icon.pixmap(icon.actualSize(QSize(120, 120))));
    QColor clr(isWhite ? Qt::white : Qt::black);
    QBitmap mask = pixmap.createMaskFromColor(clr, Qt::MaskOutColor);
    pixmap.fill(QColor(isWhite ? Qt::black : Qt::white));
    pixmap.setMask(mask);
    return QIcon(pixmap);
}

void MainWindow::SetUiIcons(bool isWhite) {
    ui->bootGameAct->setIcon(RecolorIcon(ui->bootGameAct->icon(), isWhite));
    ui->shadFolderAct->setIcon(RecolorIcon(ui->shadFolderAct->icon(), isWhite));
    ui->exitAct->setIcon(RecolorIcon(ui->exitAct->icon(), isWhite));
#ifdef ENABLE_UPDATER
    ui->updaterAct->setIcon(RecolorIcon(ui->updaterAct->icon(), isWhite));
#endif
    ui->downloadCheatsPatchesAct->setIcon(
        RecolorIcon(ui->downloadCheatsPatchesAct->icon(), isWhite));
    ui->dumpGameListAct->setIcon(RecolorIcon(ui->dumpGameListAct->icon(), isWhite));
    ui->aboutAct->setIcon(RecolorIcon(ui->aboutAct->icon(), isWhite));
    ui->setlistModeListAct->setIcon(RecolorIcon(ui->setlistModeListAct->icon(), isWhite));
    ui->setlistModeGridAct->setIcon(RecolorIcon(ui->setlistModeGridAct->icon(), isWhite));
    ui->gameInstallPathAct->setIcon(RecolorIcon(ui->gameInstallPathAct->icon(), isWhite));
    ui->menuThemes->setIcon(RecolorIcon(ui->menuThemes->icon(), isWhite));
    ui->menuGame_List_Icons->setIcon(RecolorIcon(ui->menuGame_List_Icons->icon(), isWhite));
    ui->menuUtils->setIcon(RecolorIcon(ui->menuUtils->icon(), isWhite));
    ui->playButton->setIcon(RecolorIcon(ui->playButton->icon(), isWhite));
    ui->pauseButton->setIcon(RecolorIcon(ui->pauseButton->icon(), isWhite));
    ui->stopButton->setIcon(RecolorIcon(ui->stopButton->icon(), isWhite));
    ui->exitButton->setIcon(RecolorIcon(ui->exitButton->icon(), isWhite));
    ui->refreshButton->setIcon(RecolorIcon(ui->refreshButton->icon(), isWhite));
    ui->restartButton->setIcon(RecolorIcon(ui->restartButton->icon(), isWhite));
    ui->settingsButton->setIcon(RecolorIcon(ui->settingsButton->icon(), isWhite));
    ui->systemInfoButton->setIcon(RecolorIcon(ui->systemInfoButton->icon(), isWhite));
    ui->snapshotButton->setIcon(
        RecolorIcon(QIcon(":images/screenshot_icon.png"), isWhite));
    ui->fullscreenButton->setIcon(RecolorIcon(ui->fullscreenButton->icon(), isWhite));
    ui->controllerButton->setIcon(RecolorIcon(ui->controllerButton->icon(), isWhite));
    ui->keyboardButton->setIcon(RecolorIcon(ui->keyboardButton->icon(), isWhite));
    ui->refreshGameListAct->setIcon(RecolorIcon(ui->refreshGameListAct->icon(), isWhite));
    ui->menuGame_List_Mode->setIcon(RecolorIcon(ui->menuGame_List_Mode->icon(), isWhite));
    ui->trophyViewerAct->setIcon(RecolorIcon(ui->trophyViewerAct->icon(), isWhite));
    ui->skylanderPortalAction->setIcon(RecolorIcon(ui->skylanderPortalAction->icon(), isWhite));
    ui->infinityFiguresAction->setIcon(RecolorIcon(ui->infinityFiguresAction->icon(), isWhite));
    ui->dimensionsToypadAction->setIcon(RecolorIcon(ui->dimensionsToypadAction->icon(), isWhite));
    ui->configureAct->setIcon(RecolorIcon(ui->configureAct->icon(), isWhite));
    ui->keyManager->setIcon(RecolorIcon(ui->keyManager->icon(), isWhite));
    ui->userManager->setIcon(RecolorIcon(ui->userManager->icon(), isWhite));
    ui->addElfFolderAct->setIcon(RecolorIcon(ui->addElfFolderAct->icon(), isWhite));
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    emit WindowResized(event);
    QMainWindow::resizeEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;

    QString filePath = urls.first().toLocalFile();
    bool isExecutable = false;

#ifdef Q_OS_WIN
    isExecutable = filePath.endsWith(".exe", Qt::CaseInsensitive);
#else
    QFileInfo info(filePath);
    isExecutable = info.isExecutable() || filePath.endsWith(".AppImage", Qt::CaseInsensitive);
#endif

    if (!isExecutable)
        return;
    // if a executable is dropped, launch version control
    event->acceptProposedAction();
    auto versionDialog = new VersionDialog(m_gui_settings, this);
    versionDialog->addExecutableFromDrop(filePath);
    connect(versionDialog, &QDialog::finished, this, [this](int) { LoadVersionComboBox(); });
    versionDialog->exec();
}

void MainWindow::HandleResize(QResizeEvent* event) {
    if (isTableList) {
        m_game_list_frame->RefreshListBackgroundImage();
    } else {
        m_game_grid_frame->windowWidth = this->width();
        m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        m_game_grid_frame->RefreshGridBackgroundImage();
    }
}

void MainWindow::AddRecentFiles(QString filePath) {
    QList<QString> list = gui_settings::Var2List(m_gui_settings->GetValue(gui::gen_recentFiles));
    if (!list.empty()) {
        if (filePath == list.at(0)) {
            return;
        }
        auto it = std::find(list.begin(), list.end(), filePath);
        if (it != list.end()) {
            list.erase(it);
        }
    }
    list.insert(list.begin(), filePath);
    if (list.size() > 6) {
        list.pop_back();
    }
    m_gui_settings->SetValue(gui::gen_recentFiles, gui_settings::List2Var(list));
    CreateRecentGameActions(); // Refresh the QActions.
}

void MainWindow::CreateRecentGameActions() {
    m_recent_files_group = new QActionGroup(this);
    ui->menuRecent->clear();
    QList<QString> list = gui_settings::Var2List(m_gui_settings->GetValue(gui::gen_recentFiles));

    for (int i = 0; i < list.size(); i++) {
        QAction* recentFileAct = new QAction(this);
        recentFileAct->setText(list.at(i));
        ui->menuRecent->addAction(recentFileAct);
        m_recent_files_group->addAction(recentFileAct);
    }

    connect(m_recent_files_group, &QActionGroup::triggered, this, [this](QAction* action) {
        auto gamePath = Common::FS::PathFromQString(action->text());
        AddRecentFiles(action->text()); // Update the list.
        if (!std::filesystem::exists(gamePath)) {
            QMessageBox::critical(nullptr, tr("Run Game"), QString(tr("Eboot.bin file not found")));
            return;
        }
        StartEmulator(gamePath);
    });
}

void MainWindow::LoadTranslation() {
    auto language = m_gui_settings->GetValue(gui::gen_guiLanguage).toString();

    const QString base_dir = QStringLiteral(":/translations");
    QString base_path = QStringLiteral("%1/%2.qm").arg(base_dir).arg(language);

    if (QFile::exists(base_path)) {
        if (translator != nullptr) {
            qApp->removeTranslator(translator);
        }

        translator = new QTranslator(qApp);
        if (!translator->load(base_path)) {
            QMessageBox::warning(
                nullptr, QStringLiteral("Translation Error"),
                QStringLiteral("Failed to find load translation file for '%1':\n%2")
                    .arg(language)
                    .arg(base_path));
            delete translator;
        } else {
            qApp->installTranslator(translator);
            ui->retranslateUi(this);
        }
    }
}

void MainWindow::OnLanguageChanged(const QString& locale) {
    m_gui_settings->SetValue(gui::gen_guiLanguage, locale);

    LoadTranslation();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Enter || keyEvent->key() == Qt::Key_Return) {
            auto tblMode = m_gui_settings->GetValue(gui::gl_mode).toInt();
            if (tblMode != 2 && (tblMode != 1 || m_game_grid_frame->IsValidCellSelected())) {
                StartGame();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::StartEmulator(std::filesystem::path path, QStringList args) {
    if (EmulatorState::GetInstance()->IsGameRunning()) {
        QMessageBox::critical(nullptr, tr("Run Game"), QString(tr("Game is already running!")));
        return;
    }

    QString selectedVersion = m_gui_settings->GetValue(gui::vm_versionSelected).toString();
    if (selectedVersion.isEmpty()) {
        QMessageBox::warning(this, tr("No Version Selected"),
                             // clang-format off
tr("No emulator version was selected.\nThe Version Manager menu will then open.\nSelect an emulator version from the right panel."));
        // clang-format on
        auto versionDialog = new VersionDialog(m_gui_settings, this);
        connect(versionDialog, &QDialog::finished, this, [this](int) { LoadVersionComboBox(); });
        versionDialog->exec();
        return;
    }

    QFileInfo fileInfo(selectedVersion);
    if (!fileInfo.exists()) {
        QMessageBox::critical(nullptr, "shadPS4",
                              QString(tr("Could not find the emulator executable")));
        return;
    }

    QStringList final_args{"--game", QString::fromStdWString(path.wstring())};

    final_args.append(args);

    EmulatorState::GetInstance()->SetGameRunning(true);
    last_game_path = path;

    QString workDir = QDir::currentPath();
    m_ipc_client->startEmulator(fileInfo, final_args, workDir);
    m_ipc_client->setActiveController(GamepadSelect::GetSelectedGamepad());
    UpdateToolbarButtons();
}

void MainWindow::StartEmulatorExecutable(std::filesystem::path emuPath, QString gameArg,
                                         QStringList args, bool disable_ipc) {
    if (EmulatorState::GetInstance()->IsGameRunning()) {
        QMessageBox::critical(nullptr, tr("Run Emulator"),
                              QString(tr("Emulator is already running!")));
        return;
    }

    bool gameFound = false;
    if (std::filesystem::exists(Common::FS::PathFromQString(gameArg))) {
        last_game_path = Common::FS::PathFromQString(gameArg);
        if (Core::FileSys::IsZArchiveFile(last_game_path)) {
            last_game_path /= "eboot.bin";
        }
        gameFound = true;
    } else {
        // In install folders, find game folder with same name as gameArg
        const auto install_dir_array = EmulatorSettings.GetGameInstallDirs();
        std::vector<bool> install_dirs_enabled;

        try {
            install_dirs_enabled = EmulatorSettings.GetGameInstallDirsEnabled();
        } catch (...) {
            // If it does not exist, assume that all are enabled.
            install_dirs_enabled.resize(install_dir_array.size(), true);
        }

        for (size_t i = 0; i < install_dir_array.size(); i++) {
            std::filesystem::path dir = install_dir_array[i];
            bool enabled = install_dirs_enabled[i];

            if (enabled && std::filesystem::exists(dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                    if (entry.is_directory()) {
                        if (entry.path().filename().string() == gameArg.toStdString()) {
                            last_game_path = entry.path() / "eboot.bin";
                            gameFound = true;
                            break;
                        }
                    } else if (Core::FileSys::IsZArchiveFile(entry.path())) {
                        if (entry.path().stem().string() == gameArg.toStdString()) {
                            last_game_path = entry.path() / "eboot.bin";
                            gameFound = true;
                            break;
                        }
                    }
                }
            }

            if (gameFound)
                break;
        }
    }
    if (!gameArg.isEmpty()) {
        if (!gameFound) {
            QMessageBox::critical(nullptr, "shadPS4",
                                  QString(tr("Invalid game argument provided")));
            quick_exit(1);
        }

        QStringList game_args{"--game", QString::fromStdWString(last_game_path.wstring())};
        args = game_args + args;
    }

    QFileInfo fileInfo(emuPath);
    if (!fileInfo.exists()) {
        QMessageBox::critical(nullptr, "shadPS4",
                              QString(tr("Could not find the emulator executable")));
        return;
    }

    EmulatorState::GetInstance()->SetGameRunning(true);
    QString workDir = QDir::currentPath();
    m_ipc_client->startEmulator(fileInfo, args, workDir, disable_ipc);
    UpdateToolbarButtons();
}

void MainWindow::RunGame() {
    auto gameInfo = GameInfoClass();
    auto dir = last_game_path.parent_path();
    auto info = gameInfo.readGameInfo(dir);
    auto appVersion = info.version;
    auto gameSerial = info.serial;
    auto patches = MemoryPatcher::readPatches(gameSerial, appVersion);
    for (auto patch : patches) {
        m_ipc_client->sendMemoryPatches(patch.modName, patch.address, patch.value, patch.target,
                                        patch.size, patch.maskOffset, patch.littleEndian,
                                        patch.mask, patch.maskOffset);
    }

    m_ipc_client->startGame();
}

void MainWindow::RestartEmulator() {
    QString exe = m_gui_settings->GetValue(gui::vm_versionSelected).toString();
    QStringList args{"--game", QString::fromStdWString(last_game_path.wstring())};

    if (m_ipc_client->parsedArgs.size() > 0) {
        args.clear();
        for (auto arg : m_ipc_client->parsedArgs) {
            args.append(QString::fromStdString(arg));
        }
        m_ipc_client->parsedArgs.clear();
    }

    QFileInfo fileInfo(exe);
    QString workDir = fileInfo.absolutePath();

    m_ipc_client->startEmulator(fileInfo, args, workDir);
    UpdateToolbarButtons();
}

void MainWindow::LoadVersionComboBox() {
    ui->versionComboBox->clear();
    ui->versionComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    QString savedVersionPath = m_gui_settings->GetValue(gui::vm_versionSelected).toString();
    auto versions = VersionManager::GetVersionList();

    std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) {
        auto getOrder = [](int type) {
            switch (type) {
            case 1: // Pre-release
                return 0;
            case 0: // Release
                return 1;
            case 2: // Local
                return 2;
            default:
                return 3;
            }
        };

        int orderA = getOrder(static_cast<int>(a.type));
        int orderB = getOrder(static_cast<int>(b.type));

        if (orderA != orderB)
            return orderA < orderB;

        if (a.type == VersionManager::VersionType::Release) {
            static QRegularExpression versionRegex("^v\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)$");
            QRegularExpressionMatch matchA = versionRegex.match(QString::fromStdString(a.name));
            QRegularExpressionMatch matchB = versionRegex.match(QString::fromStdString(b.name));

            if (matchA.hasMatch() && matchB.hasMatch()) {
                int majorA = matchA.captured(1).toInt();
                int minorA = matchA.captured(2).toInt();
                int patchA = matchA.captured(3).toInt();
                int majorB = matchB.captured(1).toInt();
                int minorB = matchB.captured(2).toInt();
                int patchB = matchB.captured(3).toInt();

                if (majorA != majorB)
                    return majorA > majorB;
                if (minorA != minorB)
                    return minorA > minorB;
                return patchA > patchB;
            }
        }

        return QString::fromStdString(a.name).compare(QString::fromStdString(b.name),
                                                      Qt::CaseInsensitive) < 0;
    });

    if (versions.empty()) {
        ui->versionComboBox->addItem(tr("None"));
        ui->versionComboBox->setCurrentIndex(0);
        return;
    }

    for (const auto& v : versions) {
        ui->versionComboBox->addItem(QString::fromStdString(v.name),
                                     QString::fromStdString(v.path));
    }

    int selectedIndex = ui->versionComboBox->findData(savedVersionPath);
    if (selectedIndex >= 0) {
        ui->versionComboBox->setCurrentIndex(selectedIndex);
    } else {
        ui->versionComboBox->setCurrentIndex(0);
    }

    connect(ui->versionComboBox, QOverload<int>::of(&QComboBox::activated), this,
            [this](int index) {
                QString fullPath = ui->versionComboBox->itemData(index).toString();
                m_gui_settings->SetValue(gui::vm_versionSelected, fullPath);
            });

    ui->versionComboBox->adjustSize();
}
