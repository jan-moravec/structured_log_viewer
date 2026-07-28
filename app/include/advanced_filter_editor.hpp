#pragma once

#include <loglib/filter_expression.hpp>

#include <QDialog>
#include <QString>

#include <cstddef>
#include <optional>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace loglib
{
struct LogConfiguration;
}

/// Modal editor for boolean filter expressions.
///
/// Presents a single-line-ish text query the user can type, backed
/// by `loglib::ParseQuery`. Every keystroke re-parses and either
/// updates the "resolved expression" preview or displays a
/// one-line error with a caret offset.
///
/// The dialog is intentionally text-first: the visual tree editor
/// (drag-and-drop AND/OR/NOT nodes) is a follow-up; this v1 wants to
/// unblock users who prefer typing (`level in [Warn, Error] AND
/// NOT service:health`) and to serve as the round-trip surface for
/// pretty-printed expressions coming back from the simple-mode
/// dropdowns.
///
/// The caller is expected to seed the editor with the current
/// `LogConfiguration::expression` via `LoadFromExpression` before
/// `exec()`, then read back `Result()` on `Accepted`.
class AdvancedFilterEditor : public QDialog
{
    Q_OBJECT

public:
    explicit AdvancedFilterEditor(QWidget *parent = nullptr);
    ~AdvancedFilterEditor() override = default;

    AdvancedFilterEditor(const AdvancedFilterEditor &) = delete;
    AdvancedFilterEditor &operator=(const AdvancedFilterEditor &) = delete;
    AdvancedFilterEditor(AdvancedFilterEditor &&) = delete;
    AdvancedFilterEditor &operator=(AdvancedFilterEditor &&) = delete;

    /// Pretty-prints @p expression via `loglib::FormatExpression`
    /// and seeds the text field. Match-all is rendered as an empty
    /// field so the user starts from a clean slate.
    void LoadFromExpression(const loglib::FilterExpression &expression);

    /// The parsed expression that produced the last "OK" state.
    /// `nullopt` before the user has committed a valid query.
    [[nodiscard]] std::optional<loglib::FilterExpression> Result() const;

    /// Set the initial query text directly. Useful for tests and
    /// for callers that have already stringified an expression.
    void SetQueryText(const QString &text);
    [[nodiscard]] QString QueryText() const;

private:
    void SetupLayout();
    /// Reparse the current text and update the status label + OK
    /// button. Called from the text edit's `textChanged` slot.
    void ReparseAndUpdate();
    /// Underline the character at @p byteOffset (byte-offset from
    /// `QueryParseError::offset`) with a wavy squiggle so the parse
    /// error is visible in the text field, not just the status line.
    void HighlightErrorAt(const QString &queryText, std::size_t byteOffset);
    /// Remove any prior parse-error underline.
    void ClearErrorHighlight();

    QPlainTextEdit *mQueryEdit = nullptr;
    QLabel *mStatusLabel = nullptr;
    QLabel *mHelpLabel = nullptr;
    /// Only OK is tracked: Cancel needs no state changes, so it is
    /// wired straight to `QDialog::reject` at construction.
    QPushButton *mOkButton = nullptr;
    std::optional<loglib::FilterExpression> mCachedResult;
};
