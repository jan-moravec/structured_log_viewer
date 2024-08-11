#pragma once

#include "log_line.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace loglib
{

class JsonLine : public LogLine
{
public:
    JsonLine(nlohmann::json &&line);

    LogValue GetRawValue(const std::string &key) const override;
    std::string GetLine() const override;

private:
    nlohmann::json mLine;
};

} // namespace loglib
