#pragma once

#include <loglib/regex_templates.hpp>

#include <QDir>
#include <QList>
#include <QObject>
#include <QString>

#include <map>
#include <optional>

/// Owns the merged regex-template catalog: the built-ins from
/// `loglib::BuiltinRegexTemplates()` plus user files under
/// `<AppDataLocation>/regex_templates/*.json`. Mirrors
/// `ThemeControl`'s import / export / reveal affordances.
///
/// One instance per process, constructed by `main()` after
/// `QApplication` is up (so `QStandardPaths::AppDataLocation`
/// resolves). Construction scans disk and pushes the user slice
/// into the library through `loglib::SetExtraRegexTemplates`, so
/// `RegexParser::IsValid` / `DetectRegexTemplate` see user
/// templates without any per-call plumbing.
///
/// User files shadow built-ins by `name` (same convention as
/// `ThemeControl`): a user JSON whose `name` matches a shipped
/// template replaces the picker entry. The library still probes
/// built-ins before extras tier-wise, so a shadow only changes
/// the picker — it never reorders auto-detection.
class RegexTemplateRegistry : public QObject
{
    Q_OBJECT

public:
    explicit RegexTemplateRegistry(QObject *parent = nullptr);

    /// One row of `Available()`. `priority` and `autoDetect` are
    /// surfaced so the picker can render "(probe priority N)" /
    /// "(manual only)" hints without loading the full
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
    /// Returns false on failure (e.g. missing dir despite mkpath,
    /// or the desktop service refusing on headless CI).
    static bool RevealUserTemplatesDir();

    /// Atomically write @p tmpl to `<UserTemplatesDir>/<name>.json`
    /// (with the on-disk `name` field pinned to @p name), then
    /// refresh the index and re-inject library extras. Throws
    /// `std::runtime_error` on invalid @p name
    /// (see `SanitiseTemplateName`) or on write failure.
    void SaveUserTemplate(const QString &name, loglib::RegexTemplate tmpl);

    /// Re-scan disk and re-inject user templates into the library.
    /// Exposed as the "Reload templates from disk" UX action and
    /// called internally by `SaveUserTemplate`.
    void Reload();

    /// Delete the user template @p name from disk. Refreshes the
    /// index on success. Throws `std::runtime_error` if @p name is
    /// not a known user template (built-ins can't be deleted), if
    /// the on-disk file is missing, or if unlink fails.
    void DeleteUserTemplate(const QString &name);

    /// True iff @p name is a user template (safe to edit / delete).
    /// Built-ins and unknown names return false.
    [[nodiscard]] bool IsUserTemplate(const QString &name) const;

    /// Validate @p name as a safe filename basename: rejects path
    /// separators, `..`, control chars, leading/trailing
    /// whitespace, trailing dot or space, and reserved Win32
    /// device names. Returns @p name on success; throws
    /// `std::runtime_error` on rejection. Same rules as
    /// `ThemeControl::SanitiseThemeName` for consistency.
    [[nodiscard]] static QString SanitiseTemplateName(const QString &name);

signals:
    /// Emitted whenever the index is rebuilt (construction,
    /// `Reload`, `SaveUserTemplate`, `DeleteUserTemplate`).
    /// Listeners (e.g. an open `NetworkStreamDialog`) should
    /// repopulate.
    void templatesChanged();

private:
    void Discover();
    /// Push the user slice through `loglib::SetExtraRegexTemplates`
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
