#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnString.h>
#include <DataTypes/DataTypeString.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <IO/WriteHelpers.h>
#include <ext/range.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int LOGICAL_ERROR;
}

template <typename Impl, typename Name>
class FormatFunction : public IFunction
{
public:
    static constexpr auto name = Name::name;

    static FunctionPtr create(const Context &) { return std::make_shared<FormatFunction>(); }

    String getName() const override { return name; }

    bool isVariadic() const override { return true; }

    size_t getNumberOfArguments() const override { return 0; }

    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {0}; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.empty())
            throw Exception(
                "Number of arguments for function " + getName() + " doesn't match: passed " + toString(arguments.size())
                    + ", should be at least 1",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        if (arguments.size() > Impl::argument_threshold)
            throw Exception(
                "Number of arguments for function " + getName() + " doesn't match: passed " + toString(arguments.size())
                    + ", should be at most " + std::to_string(Impl::argument_threshold),
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        for (const auto arg_idx : ext::range(0, arguments.size()))
        {
            const auto arg = arguments[arg_idx].get();
            if (!isStringOrFixedString(arg))
                throw Exception(
                    "Illegal type " + arg->getName() + " of argument " + std::to_string(arg_idx + 1) + " of function " + getName(),
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }

        return std::make_shared<DataTypeString>();
    }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const ColumnPtr & c0 = block.getByPosition(arguments[0]).column;
        const ColumnConst * c0_const_string = typeid_cast<const ColumnConst *>(&*c0);

        if (!c0_const_string)
            throw Exception("First argument of function " + getName() + " must be constant string", ErrorCodes::ILLEGAL_COLUMN);

        String pattern = c0_const_string->getValue<String>();

        auto col_res = ColumnString::create();
        /// Data for ColumnString and ColumnFixed. Nullptr means no data, it is const string
        std::vector<const ColumnString::Chars *> data(arguments.size() - 1);
        /// Offsets for ColumnString, nullptr is an indicator that there is a fixed string rather than ColumnString
        std::vector<const ColumnString::Offsets *> offsets(arguments.size() - 1);

        /// For savings N to fixed strings
        std::vector<size_t> fixed_string_N(arguments.size() - 1);

        std::vector<String> constant_strings(arguments.size() - 1);

        bool has_column_string = false;
        bool has_column_fixed_string = false;
        for (size_t i = 1; i < arguments.size(); ++i)
        {
            const ColumnPtr & column = block.getByPosition(arguments[i]).column;
            if (const ColumnString * col = checkAndGetColumn<ColumnString>(column.get()))
            {
                has_column_string = true;
                data[i - 1] = std::addressof(col->getChars());
                offsets[i - 1] = std::addressof(col->getOffsets());
            }
            else if (const ColumnFixedString * fixed_col = checkAndGetColumn<ColumnFixedString>(column.get()))
            {
                has_column_fixed_string = true;
                data[i - 1] = std::addressof(fixed_col->getChars());
                fixed_string_N[i - 1] = fixed_col->getN();
            }
            else if (const ColumnConst * const_col = checkAndGetColumnConstStringOrFixedString(column.get()))
            {
                constant_strings[i - 1] = const_col->getValue<String>();
            }
            else
                throw Exception(
                    "Illegal column " + column->getName() + " of argument of function " + getName(), ErrorCodes::ILLEGAL_COLUMN);
        }

        if (has_column_string && has_column_fixed_string)
            Impl::template vector<true, true>(
                std::move(pattern), data, offsets, fixed_string_N, constant_strings, col_res->getChars(), col_res->getOffsets(), input_rows_count);
        else if (!has_column_string && has_column_fixed_string)
            Impl::template vector<false, true>(
                std::move(pattern), data, offsets, fixed_string_N, constant_strings, col_res->getChars(), col_res->getOffsets(), input_rows_count);
        else if (has_column_string && !has_column_fixed_string)
            Impl::template vector<true, false>(
                std::move(pattern), data, offsets, fixed_string_N, constant_strings, col_res->getChars(), col_res->getOffsets(), input_rows_count);
        else
            Impl::template vector<false, false>(
                std::move(pattern), data, offsets, fixed_string_N, constant_strings, col_res->getChars(), col_res->getOffsets(), input_rows_count);
        block.getByPosition(result).column = std::move(col_res);
    }
};


struct FormatImpl
{
    static constexpr size_t argument_threshold = 1024;
    static constexpr size_t right_padding = 15;

    static void parseNumber(const String & description, UInt64 l, UInt64 r, UInt64 & res)
    {
        res = 0;
        for (UInt64 pos = l; pos < r; pos++)
        {
            if (!isNumericASCII(description[pos]))
                throw Exception("Not a number in curly braces at position " + std::to_string(pos), ErrorCodes::LOGICAL_ERROR);
            res = res * 10 + description[pos] - '0';
            if (res >= argument_threshold)
                throw Exception(
                    "Too big number for arguments, must be at most " + std::to_string(argument_threshold), ErrorCodes::LOGICAL_ERROR);
        }
    }

    static inline void init(
        const String & pattern,
        const std::vector<const ColumnString::Chars *> & data,
        size_t argument_number,
        const std::vector<String> & constant_strings,
        UInt64 * index_positions_ptr,
        std::vector<String> & substrings)
    {
        /// Is current position is after open curly brace.
        bool is_open_curly = false;
        /// The position of last open token.
        size_t last_open = -1;

        /// Is formatting in a plain {} token.
        std::optional<bool> is_plain_numbering;
        UInt64 index_if_plain = 0;

        /// Left position of adding substrings, just to the closed brace position or the start of the string.
        /// Invariant --- the start of substring is in this position.
        size_t start_pos = 0;

        /// A flag to decide whether we should glue the constant strings.
        bool glue_to_next = false;

        /// Handling double braces (escaping).
        auto double_brace_removal = [](String & str)
        {
            size_t i = 0;
            bool should_delete = true;
            str.erase(
                std::remove_if(
                    str.begin(),
                    str.end(),
                    [&i, &should_delete, &str](char)
                    {
                        bool is_double_brace = (str[i] == '{' && str[i + 1] == '{') || (str[i] == '}' && str[i + 1] == '}');
                        ++i;
                        if (is_double_brace && should_delete)
                        {
                            should_delete = false;
                            return true;
                        }
                        should_delete = true;
                        return false;
                    }),
                str.end());
        };

        for (size_t i = 0; i < pattern.size(); ++i)
        {
            if (pattern[i] == '{')
            {
                /// Escaping handling
                /// It is safe to access because of null termination
                if (pattern[i + 1] == '{')
                {
                    ++i;
                    continue;
                }

                if (is_open_curly)
                    throw Exception("Two open curly braces without close one at position " + std::to_string(i), ErrorCodes::LOGICAL_ERROR);

                String to_add = String(pattern.data() + start_pos, i - start_pos);
                double_brace_removal(to_add);
                if (!glue_to_next)
                    substrings.emplace_back(to_add);
                else
                    substrings.back() += to_add;

                glue_to_next = false;

                is_open_curly = true;
                last_open = i + 1;
            }
            else if (pattern[i] == '}')
            {
                if (pattern[i + 1] == '}')
                {
                    ++i;
                    continue;
                }

                if (!is_open_curly)
                    throw Exception("Closed curly brace without open one at position " + std::to_string(i), ErrorCodes::LOGICAL_ERROR);

                is_open_curly = false;

                if (last_open == i)
                {
                    if (is_plain_numbering && !*is_plain_numbering)
                        throw Exception(
                            "Cannot switch from automatic field numbering to manual field specification", ErrorCodes::LOGICAL_ERROR);
                    is_plain_numbering = true;
                    if (index_if_plain >= argument_number)
                        throw Exception("Argument is too big for formatting", ErrorCodes::LOGICAL_ERROR);
                    *index_positions_ptr = index_if_plain++;
                }
                else
                {
                    if (is_plain_numbering && *is_plain_numbering)
                        throw Exception(
                            "Cannot switch from automatic field numbering to manual field specification", ErrorCodes::LOGICAL_ERROR);
                    is_plain_numbering = false;

                    UInt64 arg;
                    parseNumber(pattern, last_open, i, arg);

                    if (arg >= argument_number)
                        throw Exception(
                            "Argument is too big for formatting. Note that indexing starts from zero", ErrorCodes::LOGICAL_ERROR);

                    *index_positions_ptr = arg;
                }

                /// Constant string.
                if (!data[*index_positions_ptr])
                {
                    /// The next string should be glued to last `A {} C`.format('B') -> `A B C`.
                    glue_to_next = true;
                    substrings.back() += constant_strings[*index_positions_ptr];
                }
                else
                    ++index_positions_ptr; /// Otherwise we commit arg number and proceed.

                start_pos = i + 1;
            }
        }

        if (is_open_curly)
            throw Exception("Last open curly brace is not closed", ErrorCodes::LOGICAL_ERROR);

        String to_add = String(pattern.data() + start_pos, pattern.size() - start_pos);
        double_brace_removal(to_add);

        if (!glue_to_next)
            substrings.emplace_back(to_add);
        else
            substrings.back() += to_add;
    }

    template <bool HasColumnString, bool HasColumnFixedString>
    static inline void vector(
        String pattern,
        const std::vector<const ColumnString::Chars *> & data,
        const std::vector<const ColumnString::Offsets *> & offsets,
        [[maybe_unused]] /* Because sometimes !HasColumnFixedString */ const std::vector<size_t> & fixed_string_N,
        const std::vector<String> & constant_strings,
        ColumnString::Chars & res_data,
        ColumnString::Offsets & res_offsets,
        size_t input_rows_count)
    {
        /// The subsequent indexes of strings we should use. e.g `Hello world {1} {3} {1} {0}` this array will be filled with [1, 3, 1, 0, ... (garbage)] but without constant indices.
        UInt64 index_positions[argument_threshold];
        /// Vector of substrings of pattern that will be copied to the ans, not string view because of escaping and iterators invalidation.
        /// These are exactly what is between {} tokens, for `Hello {} world {}` we will have [`Hello `, ` world `, ``].
        std::vector<String> substrings;

        init(pattern, data, offsets.size(), constant_strings, index_positions, substrings);

        UInt64 final_size = 0;

        for (String & str : substrings)
        {
            /// To use memcpySmallAllowReadWriteOverflow15 for substrings we should allocate a bit more to each string.
            /// That was chosen due to perfomance issues.
            if (!str.empty())
                str.reserve(str.size() + right_padding);
            final_size += str.size();
        }

        /// The substring number is repeated input_rows_times.
        final_size *= input_rows_count;

        /// Strings without null termination.
        for (size_t i = 1; i < substrings.size(); ++i)
        {
            final_size += data[index_positions[i - 1]]->size();
            /// Fixed strings do not have zero terminating character.
            if (offsets[index_positions[i - 1]])
                final_size -= input_rows_count;
        }

        /// Null termination characters.
        final_size += input_rows_count;

        res_data.resize(final_size);
        res_offsets.resize(input_rows_count);

        UInt64 offset = 0;
        for (UInt64 i = 0; i < input_rows_count; ++i)
        {
            memcpySmallAllowReadWriteOverflow15(res_data.data() + offset, substrings[0].data(), substrings[0].size());
            offset += substrings[0].size();
            /// All strings are constant, we should have substrings.size() == 1.
            if constexpr (HasColumnString || HasColumnFixedString)
            {
                for (size_t j = 1; j < substrings.size(); ++j)
                {
                    UInt64 arg = index_positions[j - 1];
                    auto offset_ptr = offsets[arg];
                    UInt64 arg_offset = 0;
                    UInt64 size = 0;

                    if constexpr (HasColumnString)
                    {
                        if (!HasColumnFixedString || offset_ptr)
                        {
                            arg_offset = (*offset_ptr)[i - 1];
                            size = (*offset_ptr)[i] - arg_offset - 1;
                        }
                    }

                    if constexpr (HasColumnFixedString)
                    {
                        if (!HasColumnString || !offset_ptr)
                        {
                            arg_offset = fixed_string_N[arg] * i;
                            size = fixed_string_N[arg];
                        }
                    }

                    memcpySmallAllowReadWriteOverflow15(res_data.data() + offset, data[arg]->data() + arg_offset, size);
                    offset += size;
                    memcpySmallAllowReadWriteOverflow15(res_data.data() + offset, substrings[j].data(), substrings[j].size());
                    offset += substrings[j].size();
                }
            }
            res_data[offset] = '\0';
            ++offset;
            res_offsets[i] = offset;
        }

        /*
         * Invariant of `offset == final_size` must be held.
         *
         * if (offset != final_size)
         *    abort();
         */
    }
};

struct NameFormat
{
    static constexpr auto name = "format";
};
using FunctionFormat = FormatFunction<FormatImpl, NameFormat>;

void registerFunctionFormat(FunctionFactory & factory)
{
    factory.registerFunction<FunctionFormat>();
}

}
