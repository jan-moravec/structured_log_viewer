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

/// Modal text editor for boolean filter expressions
/// (`loglib::ParseQuery` grammar).
///
/// Every keystroke re-parses; the status label reports "Parsed OK"
/// or a one-line error with a caret offset, and OK is enabled only
/// on a clean parse. Callers seed the field via `LoadFromExpression`
/// before `exec()` and read `Result()` on `Accepted`.
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

    /// Seed the field from @p expression via `FormatExpression`.
    /// Match-all renders as the empty string.
    void LoadFromExpression(const loglib::FilterExpression &expression);

    /// Last successfully-parsed expression, or `nullopt` before any
    /// clean parse.
    [[nodiscard]] std::optional<loglib::FilterExpression> Result() const;

    /// Direct text setter/getter for tests and pre-stringified callers.
    void SetQueryText(const QString &text);
    [[nodiscard]] QString QueryText() const;

private:
    void SetupLayout();
    /// `textChanged` slot: reparse and refresh status label + OK button.
    void ReparseAndUpdate();
    /// Wavy-underline the character at @p byteOffset (a
    /// `QueryParseError::offset`) so the offender is visible in the
    /// text field, not just the status line.
    void HighlightErrorAt(const QString &queryText, std::size_t byteOffset);
    void ClearErrorHighlight();

    QPlainTextEdit *mQueryEdit = nullptr;
    QLabel *mStatusLabel = nullptr;
    QLabel *mHelpLabel = nullptr;
    QPushButton *mOkButton = nullptr;
    std::optional<loglib::FilterExpression> mCachedResult;
};
