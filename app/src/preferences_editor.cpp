#include "preferences_editor.hpp"

#include "session_history_manager.hpp"
#include "streaming_control.hpp"
#include "theme_control.hpp"

#include <loglib/theme.hpp>

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>
#include <QVariant>

#include <cstddef>
#include <exception>

namespace
{
constexpr int PREFERENCES_MIN_WIDTH_PX = 300;
constexpr int RETENTION_LINES_SPIN_SINGLE_STEP = 1000;
constexpr int THEME_STATUS_CLEAR_MS = 5000;

/// Label used for the synthetic "Auto" entry at the top of the
/// theme combo. `ThemeControl::AUTO_TOKEN` (the empty string) is
/// stored as the entry's `QVariant` userdata so a user-named
/// theme that literally spells "Auto (follow system)" still
/// round-trips correctly.
constexpr char THEME_AUTO_LABEL[] = "Auto (follow system)";

QString DescribeThemeKind(loglib::ThemeKind kind)
{
    switch (kind)
    {
    case loglib::ThemeKind::Light:
        return QStringLiteral("light");
    case loglib::ThemeKind::Dark:
        return QStringLiteral("dark");
    }
    return QStringLiteral("?");
}
} // namespace

PreferencesEditor::PreferencesEditor(QWidget *parent)
    : QWidget{parent}
{
    setWindowFlags(Qt::Window);
    setWindowTitle("Preferences");
    setMinimumWidth(PREFERENCES_MIN_WIDTH_PX);

    mThemeComboBox = new QComboBox(this);
    mThemeComboBox->setToolTip(
        "Active theme. `Auto (follow system)` picks Light or Dark based on the OS "
        "palette. User themes live in <AppData>/themes/*.json and shadow built-ins "
        "with the same name."
    );

    mThemePreviewLabel = new QLabel(this);
    mThemePreviewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mThemePreviewLabel->setWordWrap(true);

    // Transient status line for theme button actions (Duplicate /
    // Reload). Clears itself after `THEME_STATUS_CLEAR_MS` so the
    // dialog doesn't accumulate stale "Saved as ..." messages
    // across a session. The timer is `singleShot` and reset on
    // every `ShowThemeStatus` call to debounce rapid clicks.
    mThemeStatusLabel = new QLabel(this);
    mThemeStatusLabel->setWordWrap(true);
    mThemeStatusClearTimer = new QTimer(this);
    mThemeStatusClearTimer->setSingleShot(true);
    connect(mThemeStatusClearTimer, &QTimer::timeout, this, [this]() { mThemeStatusLabel->clear(); });

    auto *openThemesButton = new QPushButton("Open themes folder", this);
    openThemesButton->setToolTip("Reveal the user themes folder in the OS file manager.");
    connect(openThemesButton, &QPushButton::clicked, this, [this]() {
        // Surface a status message on failure -- previously the call
        // silently no-op'd (e.g. when AppData was unwritable on a
        // locked-down host or when no file manager was registered
        // for `file://` URLs on a headless Linux session).
        if (!ThemeControl::RevealUserThemesDir())
        {
            ShowThemeStatus(
                tr("Could not open the user themes folder. The folder is at: %1")
                    .arg(ThemeControl::UserThemesDir().absolutePath())
            );
        }
    });

    auto *duplicateThemeButton = new QPushButton("Duplicate active theme...", this);
    duplicateThemeButton->setToolTip(
        "Copy the currently resolved theme into a new file under <AppData>/themes/ "
        "as `<active>-copy.json`, then open the user themes folder so it can be edited."
    );
    connect(duplicateThemeButton, &QPushButton::clicked, this, [this]() {
        try
        {
            const loglib::Theme &active = ThemeControl::Active();
            QString baseName = QString::fromStdString(active.name);
            if (baseName.isEmpty())
            {
                baseName = QStringLiteral("Theme");
            }
            // Pick the first unused `<base>-copy[ N].json` name so
            // repeated clicks don't silently overwrite.
            QString candidate = baseName + QStringLiteral("-copy");
            int suffix = 2;
            while (ThemeControl::Load(candidate).has_value())
            {
                candidate = baseName + QStringLiteral("-copy ") + QString::number(suffix);
                ++suffix;
            }
            // `SaveUserTheme` refreshes the in-memory index for us;
            // we just need to re-render the combo + reveal the file
            // so the user can edit it.
            ThemeControl::SaveUserTheme(candidate, active);
            RepopulateThemeCombo();
            ThemeControl::RevealUserThemesDir();
            ShowThemeStatus(tr("Saved as \"%1\". Edit the file in your themes folder, then "
                               "click \"Reload themes from disk\" to apply.").arg(candidate));
        }
        catch (const std::exception &ex)
        {
            QMessageBox::warning(this, tr("Duplicate theme"), QString::fromUtf8(ex.what()));
        }
    });

    auto *reloadThemesButton = new QPushButton("Reload themes from disk", this);
    reloadThemesButton->setToolTip(
        "Re-scan the user themes folder and the built-in themes, then re-apply the "
        "active theme. Use after editing a theme JSON file outside the app."
    );
    connect(reloadThemesButton, &QPushButton::clicked, this, [this]() {
        // Snapshot the pre-reload selection so we can detect the
        // "user's pick was deleted on disk" case: `ReloadAll` ->
        // `ResolveAndApplyActive` silently coerces missing names
        // back to Auto, and without explicit feedback users who
        // just edited their theme file see the selection move and
        // can't tell whether the edit took effect or whether the
        // file is broken.
        const QString preReloadSelection = ThemeControl::ActiveSelection();
        ThemeControl::ReloadAll();
        RepopulateThemeCombo();
        RefreshThemePreview();
        const QString postReloadSelection = ThemeControl::ActiveSelection();
        if (!preReloadSelection.isEmpty() && postReloadSelection.isEmpty())
        {
            ShowThemeStatus(
                tr("Reloaded themes. Active theme \"%1\" is gone from disk; reverted to Auto.")
                    .arg(preReloadSelection)
            );
        }
        else
        {
            ShowThemeStatus(tr("Reloaded themes from disk."));
        }
    });

    // Live-apply theme changes when the user actually commits a
    // pick. Wired to `activated` (not `currentIndexChanged`) so
    // arrow-key browsing through the combo does not run a full
    // `ApplyTheme` (palette push + style swap + cache rebuild) per
    // intermediate item -- on long user-theme lists that visibly
    // stutters. Cancel reverts via the standard
    // `ThemeControl::LoadConfiguration` round-trip.
    connect(mThemeComboBox, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (idx < 0)
        {
            return;
        }
        const QString selection = mThemeComboBox->itemData(idx).toString();
        ThemeControl::SetActiveSelection(selection);
        RefreshThemePreview();
    });

    // Track external theme changes (OS dark/light flip while the
    // dialog is open, another window's `SetActiveSelection`, etc.)
    // so the combo and preview don't silently go stale. The
    // `themeChanged` signal coalesces all such paths into one
    // refresh point.
    connect(&ThemeControl::Instance(), &ThemeControl::themeChanged, this, [this]() {
        RepopulateThemeCombo();
        RefreshThemePreview();
    });

    mStreamRetentionSpinBox = new QSpinBox(this);
    mStreamNewestFirstCheckBox = new QCheckBox("Show newest lines first", this);
    mStaticNewestFirstCheckBox = new QCheckBox("Show newest lines first", this);
    mRestoreLastSessionCheckBox = new QCheckBox("Restore last session on launch", this);
    mRestoreLastSessionCheckBox->setToolTip(
        "When enabled, the most recent auto-saved session is reopened automatically on startup. "
        "Only applies to the primary instance on first launch with no command-line files."
    );

    mRecentSessionsMaxSpinBox = new QSpinBox(this);
    mRecentSessionsMaxSpinBox->setRange(
        SessionHistoryManager::MAX_ENTRIES_LOWER_BOUND, SessionHistoryManager::MAX_ENTRIES_UPPER_BOUND
    );
    mRecentSessionsMaxSpinBox->setValue(SessionHistoryManager::MAX_ENTRIES);
    mRecentSessionsMaxSpinBox->setToolTip(
        "Maximum number of entries kept in the Recent Sessions submenu. Older entries are evicted "
        "automatically as new sessions are saved."
    );

    mStreamRetentionSpinBox->setRange(
        static_cast<int>(StreamingControl::MIN_RETENTION_LINES), static_cast<int>(StreamingControl::MAX_RETENTION_LINES)
    );
    mStreamRetentionSpinBox->setSingleStep(RETENTION_LINES_SPIN_SINGLE_STEP);
    mStreamRetentionSpinBox->setValue(static_cast<int>(StreamingControl::DEFAULT_RETENTION_LINES));
    mStreamRetentionSpinBox->setToolTip(
        "Maximum number of streamed lines kept in memory. Oldest lines are dropped when the cap "
        "is reached. Higher values use more memory."
    );
    mStreamNewestFirstCheckBox->setToolTip(
        "When enabled, new lines appear at the top of the stream view (oldest at the bottom). "
        "Follow newest then keeps the top of the view pinned to the most recent line."
    );
    mStaticNewestFirstCheckBox->setToolTip(
        "When enabled, files opened in static (file) mode are displayed with the last line "
        "at the top and the first line at the bottom."
    );

    // Stream retention is applied transactionally on Ok: the spinbox
    // does not push live updates, so Cancel reverts cleanly.

    auto *layout = new QVBoxLayout(this);

    auto *appearanceGroup = new QGroupBox("Theme", this);
    auto *appearanceLayout = new QVBoxLayout(appearanceGroup);
    appearanceLayout->addWidget(new QLabel("Active theme:"));
    appearanceLayout->addWidget(mThemeComboBox);
    appearanceLayout->addWidget(mThemePreviewLabel);
    auto *themeButtonLayout = new QHBoxLayout();
    themeButtonLayout->addWidget(openThemesButton);
    themeButtonLayout->addWidget(duplicateThemeButton);
    themeButtonLayout->addWidget(reloadThemesButton);
    appearanceLayout->addLayout(themeButtonLayout);
    appearanceLayout->addWidget(mThemeStatusLabel);

    auto *streamingGroup = new QGroupBox("Streaming", this);
    auto *streamingLayout = new QVBoxLayout(streamingGroup);
    streamingLayout->addWidget(new QLabel("Stream retention (lines):"));
    streamingLayout->addWidget(mStreamRetentionSpinBox);
    streamingLayout->addWidget(mStreamNewestFirstCheckBox);

    auto *staticGroup = new QGroupBox("Static (file mode)", this);
    auto *staticLayout = new QVBoxLayout(staticGroup);
    staticLayout->addWidget(mStaticNewestFirstCheckBox);

    auto *sessionGroup = new QGroupBox("Session History", this);
    auto *sessionLayout = new QVBoxLayout(sessionGroup);
    sessionLayout->addWidget(mRestoreLastSessionCheckBox);
    sessionLayout->addWidget(new QLabel("Maximum Recent Sessions entries:"));
    sessionLayout->addWidget(mRecentSessionsMaxSpinBox);

    layout->addWidget(appearanceGroup);
    layout->addWidget(streamingGroup);
    layout->addWidget(staticGroup);
    layout->addWidget(sessionGroup);

    auto *okButton = new QPushButton("Ok", this);
    auto *cancelButton = new QPushButton("Cancel", this);

    connect(okButton, &QPushButton::clicked, this, [this]() {
        // Theme selection is already live; Ok just persists it.
        ThemeControl::SaveConfiguration();
        // Mirror dialog edits into `StreamingControl` and persist them
        // before notifying so observers querying the static accessors
        // from a slot see the committed values.
        const auto retention = static_cast<size_t>(mStreamRetentionSpinBox->value());
        const bool streamNewestFirst = mStreamNewestFirstCheckBox->isChecked();
        const bool staticNewestFirst = mStaticNewestFirstCheckBox->isChecked();
        const bool streamNewestFirstChanged = (streamNewestFirst != StreamingControl::IsNewestFirst());
        const bool staticNewestFirstChanged = (staticNewestFirst != StreamingControl::IsStaticNewestFirst());
        StreamingControl::SetRetentionLines(retention);
        StreamingControl::SetNewestFirst(streamNewestFirst);
        StreamingControl::SetStaticNewestFirst(staticNewestFirst);
        StreamingControl::SaveConfiguration();
        SessionHistoryManager::SetRestoreLastSessionOnLaunch(mRestoreLastSessionCheckBox->isChecked());
        SessionHistoryManager::SetMaxEntries(mRecentSessionsMaxSpinBox->value());
        emit streamingRetentionChanged(static_cast<qulonglong>(StreamingControl::RetentionLines()));
        // Only emit on a real toggle so the re-sort chain does not run
        // on every Ok click.
        if (streamNewestFirstChanged)
        {
            emit streamingDisplayOrderChanged(streamNewestFirst);
        }
        if (staticNewestFirstChanged)
        {
            emit staticDisplayOrderChanged(staticNewestFirst);
        }
        close();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this]() {
        // Revert any live-applied theme preview back to whatever is
        // on disk. `SetActiveSelection` short-circuits when the
        // requested value already matches the in-memory live one
        // (no DiscoverThemes / ResolveAndApplyActive / signal
        // fan-out), so the no-preview common case is free. We use
        // it instead of `LoadConfiguration()` to keep the brush /
        // font cache hot when the user opened Prefs without
        // touching the theme combo.
        ThemeControl::SetActiveSelection(ThemeControl::PersistedSelection());
        // Revert the spinbox-edited values to the persisted ones; no
        // emit needed because the on-disk values are unchanged.
        StreamingControl::LoadConfiguration();
        close();
    });

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    layout->addStretch(1);
    layout->addLayout(buttonLayout);

    RepopulateThemeCombo();
    RefreshThemePreview();
}

void PreferencesEditor::UpdateFields()
{
    mStreamRetentionSpinBox->setValue(static_cast<int>(StreamingControl::RetentionLines()));
    mStreamNewestFirstCheckBox->setChecked(StreamingControl::IsNewestFirst());
    mStaticNewestFirstCheckBox->setChecked(StreamingControl::IsStaticNewestFirst());
    mRestoreLastSessionCheckBox->setChecked(SessionHistoryManager::RestoreLastSessionOnLaunch());
    mRecentSessionsMaxSpinBox->setValue(SessionHistoryManager::MaxEntries());
    RepopulateThemeCombo();
    RefreshThemePreview();
    // Wipe any stale "Saved as ..." / "Reloaded" message left over
    // from the previous time the dialog was open. The user reopens
    // for a new task and shouldn't see a half-expired toast.
    ShowThemeStatus(QString());
}

void PreferencesEditor::RepopulateThemeCombo()
{
    // Block signals so rebuilding the combo doesn't fire a spurious
    // SetActiveSelection (which would flip the theme to the first
    // entry mid-rebuild).
    const QSignalBlocker blocker(mThemeComboBox);
    mThemeComboBox->clear();
    mThemeComboBox->addItem(QString::fromLatin1(THEME_AUTO_LABEL), QString::fromLatin1(ThemeControl::AUTO_TOKEN));

    const QList<ThemeControl::ThemeListing> themes = ThemeControl::AvailableThemes();
    for (const ThemeControl::ThemeListing &t : themes)
    {
        QString label = t.name + QStringLiteral(" (") + DescribeThemeKind(t.kind) + QStringLiteral(")");
        if (t.fromUser)
        {
            label += QStringLiteral(" [user]");
        }
        mThemeComboBox->addItem(label, t.name);
    }

    // `ResolveAndApplyActive` coerces stale / unknown selections
    // back to Auto in-memory, so by the time we reach this point
    // `ActiveSelection()` either names an entry that exists in
    // the listing or is empty (Auto, the synthetic first entry).
    const QString selection = ThemeControl::ActiveSelection();
    int matchIdx = 0;
    for (int i = 1; i < mThemeComboBox->count(); ++i)
    {
        if (mThemeComboBox->itemData(i).toString() == selection)
        {
            matchIdx = i;
            break;
        }
    }
    mThemeComboBox->setCurrentIndex(matchIdx);
}

void PreferencesEditor::RefreshThemePreview()
{
    const loglib::Theme &active = ThemeControl::Active();
    QString preview = QStringLiteral("Active: %1 (%2)")
                          .arg(QString::fromStdString(active.name), DescribeThemeKind(active.kind));
    if (active.app.qtStyle.has_value() && !active.app.qtStyle->empty())
    {
        preview += QStringLiteral("\nStyle: ") + QString::fromStdString(*active.app.qtStyle);
    }
    if (active.app.fontFamily.has_value() && !active.app.fontFamily->empty())
    {
        preview += QStringLiteral("\nFont: ") + QString::fromStdString(*active.app.fontFamily);
        if (active.app.fontSize.has_value())
        {
            preview += QStringLiteral(" ") + QString::number(*active.app.fontSize) + QStringLiteral(" pt");
        }
    }
    else if (active.app.fontSize.has_value())
    {
        preview += QStringLiteral("\nFont size: ") + QString::number(*active.app.fontSize) + QStringLiteral(" pt");
    }
    mThemePreviewLabel->setText(preview);
}

void PreferencesEditor::ShowThemeStatus(const QString &message)
{
    mThemeStatusLabel->setText(message);
    if (message.isEmpty())
    {
        mThemeStatusClearTimer->stop();
        return;
    }
    mThemeStatusClearTimer->start(THEME_STATUS_CLEAR_MS);
}
