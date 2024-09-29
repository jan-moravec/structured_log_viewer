#include "filter_editor.hpp"

#include <QLabel>
#include <QMessageBox>
#include <QUuid>

using namespace loglib;

FilterEditor::FilterEditor(const std::vector<loglib::LogConfiguration::Column> &columns, QWidget *parent)
    : QDialog(parent)
{
    // Create widgets
    rowComboBox = new QComboBox(this);
    stringLineEdit = new QLineEdit(this);
    matchTypeComboBox = new QComboBox(this);
    okButton = new QPushButton("Ok", this);
    cancelButton = new QPushButton("Cancel", this);

    for (const auto &column : columns)
    {
        rowComboBox->addItem(QString::fromStdString(column.header));
    }

    // Add match types
    matchTypeComboBox->addItem("Exactly", static_cast<int>(LogConfiguration::LogFilter::Match::Exactly));
    matchTypeComboBox->addItem("Contains", static_cast<int>(LogConfiguration::LogFilter::Match::Contains));
    matchTypeComboBox->addItem(
        "Regular Expression", static_cast<int>(LogConfiguration::LogFilter::Match::RegularExpression)
    );
    matchTypeComboBox->addItem("Wildcards", static_cast<int>(LogConfiguration::LogFilter::Match::Wildcard));

    // Setup layout
    SetupLayout();

    // Connect signals to slots
    connect(okButton, &QPushButton::clicked, this, &FilterEditor::OnOkClicked);
    connect(cancelButton, &QPushButton::clicked, this, &FilterEditor::reject);
}

int FilterEditor::GetRowToFilter() const
{
    return rowComboBox->currentIndex();
}

QString FilterEditor::GetStringToFilter() const
{
    return stringLineEdit->text();
}

int FilterEditor::GetMatchType() const
{
    return matchTypeComboBox->currentData().toInt();
}

void FilterEditor::SetupLayout()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Row selection
    QHBoxLayout *rowLayout = new QHBoxLayout();
    rowLayout->addWidget(new QLabel("Row to filter:", this));
    rowLayout->addWidget(rowComboBox);
    mainLayout->addLayout(rowLayout);

    // String to filter
    QHBoxLayout *stringLayout = new QHBoxLayout();
    stringLayout->addWidget(new QLabel("String to filter:", this));
    stringLayout->addWidget(stringLineEdit);
    mainLayout->addLayout(stringLayout);

    // Match type selection
    QHBoxLayout *matchLayout = new QHBoxLayout();
    matchLayout->addWidget(new QLabel("Match type:", this));
    matchLayout->addWidget(matchTypeComboBox);
    mainLayout->addLayout(matchLayout);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    mainLayout->addLayout(buttonLayout);
}

void FilterEditor::OnOkClicked()
{
    if (stringLineEdit->text().isEmpty())
    {
        // Validation: Don't allow empty filter string
        stringLineEdit->setStyleSheet("border: 1px solid red");
    }
    else
    {
        const QString filterID = QUuid::createUuid().toString();

        // Emit the filterSubmitted signal with all input data
        emit FilterSubmitted(filterID, GetRowToFilter(), GetStringToFilter(), GetMatchType());

        // Close the dialog with an "accepted" result
        accept();
    }
}
