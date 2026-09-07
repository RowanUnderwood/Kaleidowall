#include "window.h"
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QShortcut>
#include <QStatusBar>
#include <QVBoxLayout>

namespace kaleido {
static QPushButton* button(const QString& text) {
    auto* b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
static QLabel* heading(const QString& text) {
    auto* l = new QLabel(text);
    l->setObjectName("sectionHeading");
    return l;
}
static QWidget* pair(QWidget* a, QWidget* b) {
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(a);
    l->addWidget(new QLabel("to"));
    l->addWidget(b);
    return w;
}
static QDoubleSpinBox* number(double lo, double hi, const QString& suffix = {}) {
    auto* b = new QDoubleSpinBox;
    b->setRange(lo, hi);
    b->setDecimals(1);
    b->setSuffix(suffix);
    b->setButtonSymbols(QAbstractSpinBox::PlusMinus);
    return b;
}
static QSpinBox* integer(int lo, int hi) {
    auto* b = new QSpinBox;
    b->setRange(lo, hi);
    return b;
}
Window::Window(const QString& dataDir) {
    setWindowTitle("Kaleidowall — Random Video Player");
    resize(1440, 900);
    setMinimumSize(960, 640);
    QDir().mkpath(dataDir);
    media = new Library(dataDir + "/library.sqlite", this);
    player = new Canvas(media, this);
    canvasStack = new QStackedWidget;
    canvasStack->addWidget(player);
    exportPreview = new QLabel("Preparing export…");
    exportPreview->setAlignment(Qt::AlignCenter);
    exportPreview->setStyleSheet("background:#06080c;");
    exportPreview->setMinimumSize(1, 1);
    exportPreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    canvasStack->addWidget(exportPreview);
    setCentralWidget(canvasStack);
    setStyleSheet(R"(
        QMainWindow,QDialog{background:#0b1019;color:#e4ebf5;}
        QWidget{font-family:'Segoe UI';font-size:13px;color:#dce5f2;}
        QToolBar{background:#101722;border:0;spacing:10px;padding:12px;}
        QToolBar#controls{border-top:1px solid #263142;}
        QDockWidget{font-weight:600;color:#93a5bd;}
        QDockWidget::title{background:#131c29;padding:10px;}
        QScrollArea,QStackedWidget{background:#111925;border:0;}
        QWidget#panel{background:#111925;}
        QLabel#brand{font-size:22px;font-weight:700;letter-spacing:3px;color:#f3f7ff;}
        QLabel#sectionHeading{font-size:11px;font-weight:700;letter-spacing:2px;color:#74d9cf;margin-top:16px;margin-bottom:6px;}
        QLabel#muted{color:#8293ac;}
        QPushButton{background:#1c293b;border:1px solid #304059;border-radius:6px;padding:8px 12px;}
        QPushButton:hover{background:#283b51;border-color:#5d819d;}
        QPushButton:pressed{background:#314b61;}
        QPushButton:disabled{color:#596980;border-color:#233044;}
        QProgressBar{background:#0c131e;border:1px solid #304059;border-radius:6px;min-height:22px;text-align:center;color:#edf5ff;}
        QProgressBar::chunk{background:#398d84;border-radius:5px;}
        QPushButton#primary{background:#69d4c5;border:0;color:#082b2a;font-weight:700;}
        QPushButton#primary:hover{background:#91e6dc;}
        QSpinBox,QDoubleSpinBox,QComboBox,QLineEdit{background:#0c131e;border:1px solid #2b3c53;border-radius:5px;padding:7px;min-height:19px;selection-background-color:#306f79;}
        QComboBox QAbstractItemView{background:#182436;selection-background-color:#305266;}
        QCheckBox{spacing:9px;padding:5px 0;}
        QCheckBox::indicator{width:16px;height:16px;border:1px solid #4c647e;border-radius:4px;background:#0d1622;}
        QCheckBox::indicator:checked{background:#69d4c5;border-color:#69d4c5;}
        QListWidget,QTableWidget,QTextEdit{background:#0d141f;border:1px solid #28374d;border-radius:5px;gridline-color:#202d40;selection-background-color:#254b5d;}
        QHeaderView::section{background:#182335;color:#8da5c1;border:0;padding:8px;text-align:left;}
        QSlider::groove:horizontal{height:4px;background:#30415a;border-radius:2px;}
        QSlider::sub-page:horizontal{background:#6bd7c7;}
        QSlider::handle:horizontal{background:#c3f5ee;width:12px;margin:-5px 0;border-radius:6px;}
        QStatusBar{background:#0c131d;color:#8495ad;font-size:11px;}
        QScrollBar:vertical{background:#101925;width:10px;}QScrollBar::handle:vertical{background:#34465f;border-radius:5px;min-height:24px;}
    )");
    header = addToolBar("Navigation");
    header->setMovable(false);
    auto* brand = new QLabel("◈  KALEIDOWALL");
    brand->setObjectName("brand");
    header->addWidget(brand);
    auto* tag = new QLabel("  A new perspective on your library");
    tag->setObjectName("muted");
    header->addWidget(tag);
    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(spacer);
    QStringList names = {"Settings", "Library", "Performance"};
    for (int i = 0; i < names.size(); ++i) {
        auto* b = button(names[i]);
        header->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, i] { showPanel(i); });
        if (i == 0) {
            auto* exportButton = button("Export");
            header->addWidget(exportButton);
            connect(exportButton, &QPushButton::clicked, this, &Window::showExportDialog);
        }
    }
    auto* full = button("Fullscreen  F11");
    header->addWidget(full);
    connect(full, &QPushButton::clicked, this, &Window::toggleFullscreen);
    dock = new QDockWidget("SESSION SETTINGS", this);
    dock->setAllowedAreas(Qt::RightDockWidgetArea);
    dock->setFeatures(QDockWidget::DockWidgetClosable);
    dock->setMinimumWidth(410);
    panels = new QStackedWidget;
    panels->addWidget(settingsPage());
    panels->addWidget(libraryPage());
    panels->addWidget(performancePage());
    dock->setWidget(panels);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    controls = new QToolBar("Playback", this);
    controls->setObjectName("controls");
    controls->setMovable(false);
    addToolBar(Qt::BottomToolBarArea, controls);
    playButton = button("▶  Play");
    playButton->setObjectName("primary");
    controls->addWidget(playButton);
    connect(playButton, &QPushButton::clicked, player, &Canvas::playPause);
    auto* stop = button("■  Stop");
    controls->addWidget(stop);
    connect(stop, &QPushButton::clicked, player, &Canvas::stop);
    auto* next = button("Next clips");
    controls->addWidget(next);
    connect(next, &QPushButton::clicked, player, &Canvas::nextClips);
    auto* layout = button("Next layout");
    controls->addWidget(layout);
    connect(layout, &QPushButton::clicked, player, &Canvas::nextLayout);
    auto* space = new QWidget;
    space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    controls->addWidget(space);
    muteButton = button("Muted");
    controls->addWidget(muteButton);
    connect(muteButton, &QPushButton::clicked, this, [this] {
        auto s = player->settings();
        s.muted = !s.muted;
        player->setAudio(s.muted, s.volume);
        muteButton->setText(s.muted ? "Muted" : "Sound on");
        audioMode->setCurrentIndex(s.muted ? 0 : 1);
    });
    volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setMaximumWidth(125);
    volume->setToolTip("Master volume");
    controls->addWidget(volume);
    connect(volume, &QSlider::valueChanged, this, [this](int value) {
        if (!syncing) {
            auto s = player->settings();
            player->setAudio(s.muted, value);
        }
    });
    auto* audio = button("New audio source");
    controls->addWidget(audio);
    connect(audio, &QPushButton::clicked, player, &Canvas::nextAudio);
    statusLabel = new QLabel("Ready · Add folders in Library to begin");
    statusBar()->addWidget(statusLabel, 1);
    connect(player, &Canvas::status, statusLabel, &QLabel::setText);
    connect(player, &Canvas::playbackChanged, this, [this] {
        playButton->setText(!player->playing() ? "▶  Play" : player->isPaused() ? "▶  Resume" : "Ⅱ  Pause");
    });
    connect(player, &Canvas::settingsChanged, this, &Window::syncSettings);
    connect(player, &Canvas::interaction, this, &Window::revealControls);
    connect(player, &Canvas::fullscreenRequested, this, &Window::toggleFullscreen);
    connect(media, &Library::changed, this, &Window::refreshLibrary);
    connect(media, &Library::scanStatus, this, [this](const QString& s) {
        scanLabel->setText(s);
        statusLabel->setText(s);
        bool scanning = media->scanning();
        scanButton->setText(scanning ? "Cancel scan" : "Rescan");
        removeFolderButton->setEnabled(!scanning);
        addFolderButton->setEnabled(!scanning);
    });
    connect(media, &Library::scanFinished, this, [this] {
        scanButton->setText("Rescan");
        removeFolderButton->setEnabled(true);
        addFolderButton->setEnabled(true);
        refreshLibrary();
    });
    auto shortcut = [this](const QString& key, auto action) {
        auto* s = new QShortcut(QKeySequence(key), this);
        connect(s, &QShortcut::activated, this, action);
    };
    shortcut("Space", [this] { player->playPause(); });
    shortcut("F11", [this] { toggleFullscreen(); });
    shortcut("Escape", [this] {
        if (isFullScreen())
            toggleFullscreen();
        else
            dock->hide();
    });
    shortcut("Ctrl+Right", [this] { player->nextClips(); });
    shortcut("Ctrl+L", [this] { player->nextLayout(); });
    shortcut("Ctrl+,", [this] { showPanel(0); });
    hideTimer.setSingleShot(true);
    hideTimer.setInterval(3000);
    connect(&hideTimer, &QTimer::timeout, this, [this] {
        if (isFullScreen() && !dock->isVisible()) {
            header->hide();
            controls->hide();
            statusBar()->hide();
            player->setCursor(Qt::BlankCursor);
        }
    });
    qApp->installEventFilter(this);
    statsTimer.start(1000);
    connect(&statsTimer, &QTimer::timeout, this, [this] {
        if (dock->isVisible() && panels->currentIndex() == 2)
            performance->setPlainText(player->performanceText());
    });
    initializePresets();
    syncSettings();
    refreshLibrary();
    if (!media->error().isEmpty())
        QMessageBox::critical(this, "Library database", media->error());
}
Window::~Window() {
    statsTimer.stop();
    hideTimer.stop();
    exportPreviewTimer.stop();
    if (exportWorker) {
        disconnect(exportWorker, nullptr, this, nullptr);
        exportWorker->cancel();
        exportWorker->wait();
        delete exportWorker;
        exportWorker = nullptr;
    }
    delete exportSurface;
    exportSurface = nullptr;
    delete takeCentralWidget();
    player = nullptr;
    delete media;
    media = nullptr;
}
QWidget* Window::settingsPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* page = new QWidget;
    page->setObjectName("panel");
    auto* box = new QVBoxLayout(page);
    box->setContentsMargins(20, 10, 20, 20);
    box->setSpacing(8);
    box->addWidget(heading("PRESETS"));
    presets = new QComboBox;
    presets->setObjectName("presets");
    box->addWidget(presets);
    presetLoad = button("Load");
    presetSave = button("Save");
    auto* saveAs = button("Save as…");
    presetDelete = button("Delete");
    presetDefault = button("Set default");
    presetLoad->setObjectName("presetLoad");
    presetSave->setObjectName("presetSave");
    saveAs->setObjectName("presetSaveAs");
    presetDelete->setObjectName("presetDelete");
    presetDefault->setObjectName("presetDefault");
    auto* saveRow = new QHBoxLayout;
    for (auto* b : {presetLoad, presetSave, saveAs})
        saveRow->addWidget(b);
    box->addLayout(saveRow);
    auto* manageRow = new QHBoxLayout;
    manageRow->addWidget(presetDelete);
    manageRow->addWidget(presetDefault);
    box->addLayout(manageRow);
    presetInfo = new QLabel;
    presetInfo->setObjectName("muted");
    presetInfo->setWordWrap(true);
    box->addWidget(presetInfo);
    connect(presets, &QComboBox::currentIndexChanged, this, &Window::updatePresetActions);
    connect(presetLoad, &QPushButton::clicked, this, [this] { loadPreset(presets->currentData().toString()); });
    connect(presetSave, &QPushButton::clicked, this, [this] { savePreset(false); });
    connect(saveAs, &QPushButton::clicked, this, [this] { savePreset(true); });
    connect(presetDelete, &QPushButton::clicked, this, &Window::deletePreset);
    connect(presetDefault, &QPushButton::clicked, this, &Window::setDefaultPreset);
    box->addWidget(heading("PLAYBACK"));
    auto* form = new QFormLayout;
    form->setSpacing(10);
    minSlots = integer(1, 32);
    maxSlots = integer(1, 32);
    minSlots->setObjectName("minSlots");
    maxSlots->setObjectName("maxSlots");
    form->addRow("Active videos", pair(minSlots, maxSlots));
    clipMin = number(.5, 3600, " s");
    clipMax = number(.5, 3600, " s");
    form->addRow("Clip duration", pair(clipMin, clipMax));
    box->addLayout(form);
    duplicates = new QCheckBox("Allow duplicate videos at the same time");
    box->addWidget(duplicates);
    box->addWidget(heading("AUDIO"));
    audioMode = new QComboBox;
    audioMode->addItems({"Muted", "One random video"});
    box->addWidget(audioMode);
    box->addWidget(heading("LAYOUT & MOTION"));
    auto* layoutForm = new QFormLayout;
    layoutMin = number(1, 3600, " s");
    layoutMax = number(1, 3600, " s");
    layoutForm->addRow("Change layout", pair(layoutMin, layoutMax));
    transition = number(0, 10, " s");
    layoutForm->addRow("Transition", transition);
    backgroundColorButton = button("Choose color…");
    backgroundColorButton->setObjectName("backgroundColorButton");
    backgroundColorButton->setToolTip("Background behind video tiles and in empty borders");
    layoutForm->addRow("Background color", backgroundColorButton);
    connect(backgroundColorButton, &QPushButton::clicked, this, [this] {
        const QColor color =
            QColorDialog::getColor(QColor(selectedBackgroundColor), this, "Choose background color");
        if (color.isValid()) {
            selectedBackgroundColor = color.name(QColor::HexRgb);
            refreshBackgroundColor();
        }
    });
    box->addLayout(layoutForm);
    reduced = new QCheckBox("Reduced motion — instant layout changes");
    crop = new QCheckBox("Crop videos to fill each viewport");
    box->addWidget(reduced);
    box->addWidget(crop);
    auto* note = new QLabel("Layout weights · 0 disables a mode");
    note->setObjectName("muted");
    box->addWidget(note);
    auto* modes = new QFormLayout;
    for (const auto& name : QStringList{"Split", "Grid", "Hero", "Masonry", "Circles", "Hexagons", "Honeycomb", "Inset"}) {
        auto* weight = integer(0, 10);
        modeWeights[name] = weight;
        if (name == "Honeycomb")
            weight->setToolTip("Centered, edge-to-edge hexagons for 3 or more videos; otherwise uses Hexagons.");
        if (name == "Inset")
            weight->setToolTip("One full-screen background video plus random Circles, Hexagons or Honeycomb. "
                               "The background counts toward the total and always fills the screen; "
                               "Crop / Fit applies to the foreground.");
        modes->addRow(name, weight);
    }
    box->addLayout(modes);
    box->addWidget(heading("BEGINNING & END EXCLUSIONS"));
    auto* trims = new QFormLayout;
    skipStart = number(0, 86400);
    skipEnd = number(0, 86400);
    trims->addRow("Skip beginning", skipStart);
    trims->addRow("Skip end", skipEnd);
    box->addLayout(trims);
    percent = new QCheckBox("Use percentages instead of seconds");
    box->addWidget(percent);
    auto* trimNote = new QLabel("Clips always fit inside the usable range. Files that cannot fit the minimum "
                                "clip are skipped. Per-video overrides are available in Library.");
    trimNote->setWordWrap(true);
    trimNote->setObjectName("muted");
    box->addWidget(trimNote);
    box->addWidget(heading("PERFORMANCE"));
    hwdec = new QCheckBox("Hardware decoding when supported");
    box->addWidget(hwdec);
    auto* perf = new QFormLayout;
    fps = integer(24, 144);
    fps->setSuffix(" fps");
    buffer = integer(8, 1024);
    buffer->setSuffix(" MiB");
    texture = integer(320, 3840);
    texture->setSuffix(" px");
    texture->setSingleStep(320);
    perf->addRow("Render target", fps);
    perf->addRow("Cache / video", buffer);
    perf->addRow("Texture limit", texture);
    box->addLayout(perf);
    auto* apply = button("Apply settings");
    apply->setObjectName("primary");
    box->addWidget(apply);
    connect(apply, &QPushButton::clicked, this, &Window::applySettings);
    box->addStretch();
    scroll->setWidget(page);
    return scroll;
}
QWidget* Window::libraryPage() {
    auto* page = new QWidget;
    page->setObjectName("panel");
    auto* box = new QVBoxLayout(page);
    box->setContentsMargins(16, 8, 16, 16);
    box->addWidget(heading("LIBRARY FOLDERS"));
    folderList = new QListWidget;
    folderList->setMaximumHeight(120);
    folderList->setToolTip("Untick a folder to exclude it from playback and export.");
    box->addWidget(folderList);
    connect(folderList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (rebuildingFolders)
            return;
        media->setFolderEnabled(item->data(Qt::UserRole).toString(), item->checkState() == Qt::Checked);
    });
    addFolderButton = button("Add folder");
    removeFolderButton = button("Remove");
    auto* row = new QHBoxLayout;
    row->addWidget(addFolderButton);
    row->addWidget(removeFolderButton);
    box->addLayout(row);
    connect(addFolderButton, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getExistingDirectory(this, "Add video folder");
        if (!path.isEmpty()) {
            media->addFolder(path);
            media->scan();
        }
    });
    connect(removeFolderButton, &QPushButton::clicked, this, [this] {
        if (auto* item = folderList->currentItem())
            media->removeFolder(item->data(Qt::UserRole).toString());
    });
    scanButton = button("Rescan");
    box->addWidget(scanButton);
    connect(scanButton, &QPushButton::clicked, this, [this] {
        if (media->scanning())
            media->cancelScan();
        else
            media->scan();
    });
    scanLabel = new QLabel("Folders are scanned recursively. Source files are never moved or deleted. "
                           "Unticking a folder applies to playback and export at once; rescans skip it "
                           "from then on.");
    scanLabel->setWordWrap(true);
    scanLabel->setObjectName("muted");
    box->addWidget(scanLabel);
    box->addWidget(heading("VIDEOS"));
    search = new QLineEdit;
    search->setPlaceholderText("Search titles, codecs, or paths…");
    box->addWidget(search);
    connect(search, &QLineEdit::textChanged, this, &Window::refreshLibrary);
    librarySummary = new QLabel;
    librarySummary->setObjectName("muted");
    box->addWidget(librarySummary);
    videoTable = new QTableWidget;
    videoTable->setColumnCount(4);
    videoTable->setHorizontalHeaderLabels({"Video", "Length", "Codec", "Status"});
    videoTable->verticalHeader()->hide();
    videoTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    videoTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    videoTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    videoTable->setColumnWidth(1, 65);
    videoTable->setColumnWidth(2, 65);
    videoTable->setColumnWidth(3, 110);
    box->addWidget(videoTable, 1);
    connect(videoTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { editVideo(row); });
    auto* edit = button("Edit selected video…");
    box->addWidget(edit);
    connect(edit, &QPushButton::clicked, this, [this] { editVideo(videoTable->currentRow()); });
    return page;
}
QWidget* Window::performancePage() {
    auto* page = new QWidget;
    page->setObjectName("panel");
    auto* box = new QVBoxLayout(page);
    box->addWidget(heading("LIVE PERFORMANCE"));
    performance = new QTextEdit;
    performance->setReadOnly(true);
    box->addWidget(performance);
    auto* note = new QLabel("This build uses one display GPU. Cache ceilings exclude decoder surfaces and "
                            "GPU textures. Dropped frames are reported by libmpv.");
    note->setWordWrap(true);
    note->setObjectName("muted");
    box->addWidget(note);
    return page;
}
void Window::syncSettings() {
    audioMode->setCurrentIndex(player->settings().muted ? 0 : 1);
    syncing = true;
    const auto s = player->settings();
    selectedBackgroundColor = s.backgroundColor;
    refreshBackgroundColor();
    minSlots->setValue(s.minSlots);
    maxSlots->setValue(s.maxSlots);
    clipMin->setValue(s.clipMin);
    clipMax->setValue(s.clipMax);
    layoutMin->setValue(s.layoutMin);
    layoutMax->setValue(s.layoutMax);
    transition->setValue(s.transition);
    skipStart->setValue(s.skipStart);
    skipEnd->setValue(s.skipEnd);
    percent->setChecked(s.skipPercent);
    reduced->setChecked(s.reducedMotion);
    duplicates->setChecked(s.duplicates);
    crop->setChecked(s.crop);
    hwdec->setChecked(s.hwdec);
    fps->setValue(s.fps);
    buffer->setValue(s.bufferMiB);
    texture->setValue(s.textureLimit);
    volume->setValue(s.volume);
    muteButton->setText(s.muted ? "Muted" : "Sound on");
    for (auto it = modeWeights.begin(); it != modeWeights.end(); ++it)
        it.value()->setValue(s.modes.contains(it.key()) ? s.weights.value(it.key()).toInt(1) : 0);
    syncing = false;
}
void Window::refreshBackgroundColor() {
    QPixmap swatch(20, 20);
    swatch.fill(QColor(selectedBackgroundColor));
    backgroundColorButton->setIcon(QIcon(swatch));
    backgroundColorButton->setIconSize(swatch.size());
    backgroundColorButton->setText(selectedBackgroundColor.toUpper() + "  Choose…");
}
void Window::applySettings() {
    auto s = player->settings();
    s.backgroundColor = selectedBackgroundColor;
    s.muted = audioMode->currentIndex() == 0;
    s.minSlots = minSlots->value();
    s.maxSlots = maxSlots->value();
    s.clipMin = clipMin->value();
    s.clipMax = clipMax->value();
    s.layoutMin = layoutMin->value();
    s.layoutMax = layoutMax->value();
    s.transition = transition->value();
    s.skipStart = skipStart->value();
    s.skipEnd = skipEnd->value();
    s.skipPercent = percent->isChecked();
    s.reducedMotion = reduced->isChecked();
    s.duplicates = duplicates->isChecked();
    s.crop = crop->isChecked();
    s.hwdec = hwdec->isChecked();
    s.fps = fps->value();
    s.bufferMiB = buffer->value();
    s.textureLimit = texture->value();
    s.modes.clear();
    s.weights = {};
    for (auto it = modeWeights.begin(); it != modeWeights.end(); ++it)
        if (it.value()->value() > 0) {
            s.modes << it.key();
            s.weights[it.key()] = it.value()->value();
        }
    s.normalize();
    player->applySettings(s);
    syncSettings();
    refreshLibrary();
    statusLabel->setText("Settings saved. Existing eligible clips finish their current ranges; new "
                         "selections use these exclusions.");
}
void Window::initializePresets() {
    if (media->presets().empty()) {
        Settings s;
        media->setValue("preset:Balanced mosaic", s.json());
        s.minSlots = s.maxSlots = 1;
        s.clipMin = 30;
        s.clipMax = 90;
        s.crop = false;
        media->setValue("preset:Single screen", s.json());
        s = Settings{};
        s.minSlots = 4;
        s.maxSlots = 9;
        s.clipMin = 6;
        s.clipMax = 18;
        s.layoutMin = 8;
        s.layoutMax = 15;
        media->setValue("preset:Kaleidoscope", s.json());
        s = Settings{};
        s.reducedMotion = true;
        s.clipMin = 30;
        s.clipMax = 60;
        s.layoutMin = 45;
        s.layoutMax = 90;
        media->setValue("preset:Calm", s.json());
    }
    const auto state = media->value("presetState");
    const auto names = media->presets();
    loadedPreset = state.value("loadedPreset").toString();
    if (!names.contains(loadedPreset))
        loadedPreset.clear();
    refreshPresets();
    const auto defaultName = state.value("defaultPreset").toString();
    if (names.contains(defaultName))
        loadPreset(defaultName);
    else if (!loadedPreset.isEmpty())
        presets->setCurrentIndex(presets->findData(loadedPreset));
}
void Window::refreshPresets() {
    const auto selected = presets->currentData().toString();
    const auto defaultName = media->value("presetState").value("defaultPreset").toString();
    presets->clear();
    for (const auto& name : media->presets())
        presets->addItem(name + (name == defaultName ? " (default)" : ""), name);
    const int index = presets->findData(selected);
    if (index >= 0)
        presets->setCurrentIndex(index);
    updatePresetActions();
}
void Window::updatePresetActions() {
    const bool selected = presets->currentIndex() >= 0;
    presetLoad->setEnabled(selected);
    presetSave->setEnabled(selected);
    presetDelete->setEnabled(selected);
    const auto defaultName = media->value("presetState").value("defaultPreset").toString();
    presetDefault->setEnabled(selected && presets->currentData().toString() != defaultName);
    presetInfo->setText("Loaded: " + (loadedPreset.isEmpty() ? "Last-used settings" : loadedPreset) +
                        "\nStartup: " + (defaultName.isEmpty() ? "Last-used settings" : defaultName));
}
void Window::loadPreset(const QString& name) {
    if (!media->presets().contains(name))
        return;
    player->applySettings(Settings::fromJson(media->value("preset:" + name)));
    loadedPreset = name;
    auto state = media->value("presetState");
    state["loadedPreset"] = name;
    media->setValue("presetState", state);
    presets->setCurrentIndex(presets->findData(name));
    syncSettings();
    updatePresetActions();
}
void Window::savePreset(bool saveAs) {
    QString name = presets->currentData().toString();
    if (saveAs) {
        bool ok = false;
        name = QInputDialog::getText(this, "Save preset as", "Preset name", QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
    } else if (name.isEmpty()) {
        return;
    }
    if (media->presets().contains(name) &&
        QMessageBox::question(this, "Replace preset", "Replace preset \"" + name +
                              "\" with the settings currently shown? These settings will also be applied.",
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    applySettings();
    if (!media->setValue("preset:" + name, player->settings().json())) {
        QMessageBox::critical(this, "Save preset", media->error());
        return;
    }
    loadedPreset = name;
    auto state = media->value("presetState");
    state["loadedPreset"] = name;
    media->setValue("presetState", state);
    refreshPresets();
    presets->setCurrentIndex(presets->findData(name));
}
void Window::deletePreset() {
    const auto name = presets->currentData().toString();
    const auto names = media->presets();
    if (!names.contains(name))
        return;
    if (names.size() <= 1) {
        QMessageBox::information(this, "Cannot delete preset", "At least one preset must remain.");
        return;
    }
    const int index = names.indexOf(name);
    const auto replacement = names[index + 1 < names.size() ? index + 1 : index - 1];
    const bool deletingLoaded = loadedPreset == name;
    QString message = "Delete preset \"" + name + "\"?";
    if (deletingLoaded)
        message += "\nPreset \"" + replacement + "\" will be loaded, replacing the current settings and any unapplied edits.";
    if (media->value("presetState").value("defaultPreset").toString() == name)
        message += "\nThe startup default will be cleared; startup will restore last-used settings.";
    if (QMessageBox::question(this, "Delete preset", message, QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (!media->removePreset(name)) {
        QMessageBox::critical(this, "Delete preset", media->error());
        return;
    }
    refreshPresets();
    presets->setCurrentIndex(presets->findData(replacement));
    if (deletingLoaded)
        loadPreset(replacement);
}
void Window::setDefaultPreset() {
    const auto name = presets->currentData().toString();
    if (!media->presets().contains(name))
        return;
    auto state = media->value("presetState");
    state["defaultPreset"] = name;
    if (!media->setValue("presetState", state)) {
        QMessageBox::critical(this, "Set default preset", media->error());
        return;
    }
    refreshPresets();
}
void Window::refreshLibrary() {
    auto folders = media->folders();
    int enabledFolders = 0;
    rebuildingFolders = true;
    folderList->clear();
    for (const auto& f : folders) {
        auto* item = new QListWidgetItem(f.path, folderList);
        item->setData(Qt::UserRole, f.path);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(f.enabled ? Qt::Checked : Qt::Unchecked);
        item->setForeground(QColor(f.enabled ? "#dce5f2" : "#8293ac"));
        item->setToolTip(f.enabled ? f.path : f.path + "\nDisabled: excluded from playback and export.");
        enabledFolders += f.enabled ? 1 : 0;
    }
    rebuildingFolders = false;
    rows.clear();
    auto all = media->videos();
    int eligible = 0;
    QString query = search->text();
    for (const auto& v : all) {
        if (eligibilityReason(v, player->settings()).isEmpty())
            ++eligible;
        if (query.isEmpty() || v.title.contains(query, Qt::CaseInsensitive) ||
            v.path.contains(query, Qt::CaseInsensitive) || v.codec.contains(query, Qt::CaseInsensitive))
            rows << v;
    }
    librarySummary->setText(QString("%1 videos · %2 eligible · %3 shown · %4 of %5 folders enabled")
                                .arg(all.size())
                                .arg(eligible)
                                .arg(rows.size())
                                .arg(enabledFolders)
                                .arg(folders.size()));
    videoTable->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        const auto& v = rows[i];
        auto why = eligibilityReason(v, player->settings());
        QStringList text = {
            v.title, QString("%1:%2").arg(int(v.duration) / 60).arg(int(v.duration) % 60, 2, 10, QChar('0')),
            v.codec, why.isEmpty() ? "Ready" : why};
        for (int c = 0; c < 4; ++c) {
            auto* item = new QTableWidgetItem(text[c]);
            item->setToolTip(c == 0 ? v.path : text[c]);
            if (c == 3)
                item->setForeground(why.isEmpty() ? QColor("#77d8bf") : QColor("#d5ad7b"));
            videoTable->setItem(i, c, item);
        }
    }
}
void Window::editVideo(int row) {
    if (row < 0 || row >= rows.size())
        return;
    const auto v = rows[row];
    QDialog dialog(this);
    dialog.setWindowTitle("Video overrides");
    dialog.setMinimumWidth(470);
    auto* box = new QVBoxLayout(&dialog);
    auto* title = new QLabel(v.title);
    title->setWordWrap(true);
    box->addWidget(title);
    auto* path = new QLabel(v.path);
    path->setWordWrap(true);
    path->setObjectName("muted");
    box->addWidget(path);
    auto* enabled = new QCheckBox("Include in shuffle");
    enabled->setChecked(v.enabled);
    box->addWidget(enabled);
    if (!v.folderEnabled) {
        auto* parked = new QLabel("This video's folder is unticked, so it stays out of playback and "
                                  "export regardless of this setting.");
        parked->setWordWrap(true);
        parked->setObjectName("muted");
        box->addWidget(parked);
    }
    auto* form = new QFormLayout;
    auto* start = number(-1, 86400, " s");
    auto* end = number(-1, 86400, " s");
    start->setSpecialValueText("Use global setting");
    end->setSpecialValueText("Use global setting");
    start->setValue(v.skipStart);
    end->setValue(v.skipEnd);
    form->addRow("Skip beginning", start);
    form->addRow("Skip end", end);
    box->addLayout(form);
    auto* note = new QLabel("Overrides are always seconds. Set to −1 to inherit the global value.");
    note->setWordWrap(true);
    box->addWidget(note);
    auto* save = button("Save overrides");
    save->setObjectName("primary");
    box->addWidget(save);
    connect(save, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() == QDialog::Accepted)
        media->updateVideo(v.id, enabled->isChecked(), start->value(), end->value());
}
void Window::showPanel(int i) {
    if (dock->isVisible() && panels->currentIndex() == i) {
        dock->hide();
        return;
    }
    panels->setCurrentIndex(i);
    dock->setWindowTitle(QStringList{"SESSION SETTINGS", "VIDEO LIBRARY", "PERFORMANCE"}[i]);
    dock->show();
    revealControls();
}
void Window::toggleFullscreen() {
    if (isFullScreen()) {
        showNormal();
        header->show();
        controls->show();
        statusBar()->show();
        player->unsetCursor();
        hideTimer.stop();
    } else {
        dock->hide();
        showFullScreen();
        revealControls();
    }
}
void Window::revealControls() {
    header->show();
    controls->show();
    statusBar()->show();
    player->unsetCursor();
    if (isFullScreen())
        hideTimer.start();
}
bool Window::eventFilter(QObject* watched, QEvent* event) {
    if (isFullScreen() && (event->type() == QEvent::MouseMove || event->type() == QEvent::KeyPress))
        revealControls();
    return QMainWindow::eventFilter(watched, event);
}
void Window::closeEvent(QCloseEvent* event) {
    if (exportWorker) {
        closeAfterExport = true;
        cancelExport();
        event->ignore();
        return;
    }
    media->cancelScan();
    player->stop();
    QMainWindow::closeEvent(event);
}
} // namespace kaleido
