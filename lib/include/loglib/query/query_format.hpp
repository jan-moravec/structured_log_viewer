/**
 * @brief Writes a filter expression back as query text.
 *
 * FormatExpression is the inverse of ParseQuery. The advanced filter editor uses it to show the text of a filter that
 * was built in another way, and the tests use it to check that every expression the parser accepts survives a round
 * trip.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/schema/filter_expression.hpp"

#include <string>

namespace loglib
{

/**
 * @brief Writes an expression as query text. Every expression that ParseQuery accepts is written so that ParseQuery
 * reads back the same expression.
 *
 * An expression that the parser cannot make has no such guarantee. A leaf with no bounds or values, such as an empty IN
 * list, is written as IN [] for debugging, and that text does not parse.
 *
 * - A bareword is put in quotes when it contains whitespace or operator characters, or starts with a digit. A regular
 *   expression is always written between slashes.
 * - An empty And at the top gives empty text. An empty And deeper in the tree is written as * for debugging, and an
 *   empty Or as (). Neither parses.
 * - An Or under an And or a Not gets parentheses. An And under an And and an Or under an Or do too, because the parser
 *   keeps a group as a nested expression. A Not gets parentheses around its child unless the child is a leaf or another
 *   Not.
 */
[[nodiscard]] std::string FormatExpression(const FilterExpression &expression);

} // namespace loglib
