#pragma once

#include <loglib/regex_templates.hpp>

#include <QDir>
#include <QList>
#include <QObject>
#include <QString>

#include <map>
#include <optional>

/// Owns the merged regex-template catalog (built-ins shipped in
/// `loglib::BuiltinRegexTemplates()` plus user-supplied files
/// discovered under `<AppDataLocation>/regex_templates/*.json`)
/// and exposes import / export / reveal affordances mirroring
/// `ThemeControl`.
///
/// One instance per process, constructed by `main()` after
/// `QApplication` is up (so `QStandardPaths::AppDataLocation`
/// resolves correctly). On construction the registry scans disk
/// and pushes the discovered user templates into the library via
/// `loglib::SetExtraRegexTemplates`, so `RegexParser::IsValid` /
/// `DetectRegexTemplate` see user templates in the auto-detect
/// loop without any per-call plumbing on the parser side.
///
/// User files shadow built-ins by `name` (mirroring
/// `ThemeControl`'s convention): a JSON in the user dir whose
/// `name` field matches a shipped template silently overrides it
/// in the picker. The library-side probe order still puts
/// built-ins ahead of extras tier-wise, so a user shadow doesn't
/// reorder auto-detection — it just replaces the picker entry
/// and adds the user pattern to the probe list.
class RegexTemplateRegistry : public QObject
{
    Q_OBJECT

public:
    explicit RegexTemplateRegistry(QObject *parent = nullptr);

    /// One row of `Available()`. `priority` and `autoDetect` are
    /// surfaced so the picker can show "(probe priority N)" or
    /// "(manual only)" hints without re-fetching the full
    /// `loglib::RegexTemplate`.
    struct Listing
    {
        QString name;
        bool fromUser = false;
        int priority = 0;
        bool autoDetect = true;
    };

    /// Sorted alphabetically by name. Built-ins shadowed by a
    /// user file appear once, marked `fromUser=true`.
    [[nodiscard]] QList<Listing> Available() const;

    /// Look up by name. User shadow wins over built-in.
    [[nodiscard]] std::optional<loglib::RegexTemplate> Load(const QString &name) const;

    /// `<AppDataLocation>/regex_templates`, created on demand.
    /// Falls back to `<temp>/StructuredLogViewer/regex_templates`
    /// if AppData is empty (test envs, server installs).
    static QDir UserTemplatesDir();

    /// Open the user templates folder in the OS file manager.
    /// Returns false on failure (typically the dir not existing
    /// despite `UserTemplatesDir()`'s mkpath, or the desktop
    /// service refusing — e.g. headless CI).
    static bool RevealUserTemplatesDir();

    /// Atomically write @p tmpl to
    /// `<UserTemplatesDir>/<name>.json` (the on-disk `name` field
    /// is pinned to @p name) and refresh the index / library
    /// extras. Throws `std::runtime_error` on invalid @p name
    /// (see `SanitiseTemplateName`) or write failure.
    void SaveUserTemplate(const QString &name, loglib::RegexTemplate tmpl);

    /// Re-scan disk and re-inject user templates into the library.
    /// Surfaced as a "Reload templates from disk" UX affordance
    /// and called internally by `SaveUserTemplate`.
    void Reload();

    /// Delete the user template named @p name from disk. Throws
    /// `std::runtime_error` if @p name is not a known user
    /// template (built-ins can never be deleted -- they live in
    /// the binary), if the on-disk file is missing, or if the
    /// unlink fails. Refreshes the index and re-injects extras on
    /// success.
    void DeleteUserTemplate(const QString &name);

    /// True iff @p name is a user template (and therefore safe to
    /// edit / delete). Built-ins and unknown names return false.
    [[nodiscard]] bool IsUserTemplate(const QString &name) const;

    /// Validate @p name as a safe filename basename. Rejects path
    /// separators, `..`, control characters, leading/trailing
    /// whitespace, trailing dot or space, and reserved Win32
    /// device names. Returns @p name unchanged on success, throws
    /// `std::runtime_error` on rejection. Same rules as
    /// `ThemeControl::SanitiseThemeName` so the two registries
    /// behave consistently.
    [[nodiscard]] static QString SanitiseTemplateName(const QString &name);

signals:
    /// Emitted whenever the index is rebuilt (construction,
    /// `Reload`, or `SaveUserTemplate`). Listeners (e.g.
    /// `NetworkStreamDialog` while open) should repopulate.
    void templatesChanged();

private:
    void Discover();
    /// Push the user-template slice to `loglib::SetExtraRegexTemplates`
    /// so the parser's auto-detect probe sees user patterns in
    /// priority order. Called by `Discover`.
    void InjectExtrasIntoLoglib();

    struct IndexEntry
    {
        loglib::RegexTemplate tmpl;
        bool fromUser = false;
    };
    /// Per-name index. User-dir entries shadow built-ins.
    std::map<QString, IndexEntry> mIndex;
};
