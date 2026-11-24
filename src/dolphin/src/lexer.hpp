/**
 * @file lexer.hpp
 * @brief Turns Dolphin source text into a flat token stream.
 *
 * Dolphin uses Python-style significant indentation, so the lexer does more
 * than split text: it tracks indentation depth and emits synthetic
 * Tok::Indent / Tok::Dedent tokens, and it suppresses line breaks inside
 * brackets so expressions can span lines. After lexing, the parser never has
 * to think about whitespace.
 *
 * Rules worth knowing:
 *  - Indentation must be spaces. A tab in leading whitespace is a SyntaxError,
 *    which removes the classic tabs-vs-spaces ambiguity outright.
 *  - Blank lines and comment-only lines never affect indentation.
 *  - Inside (), [] or {} newlines are ignored (implicit line joining).
 *    A trailing backslash joins lines explicitly.
 *  - String literals are decoded here, so Token::text holds the real
 *    characters, not the escaped source. f-strings are an exception in one
 *    respect: their `{...}` fields are left as text for the parser to split.
 */
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

namespace dolphin {

/// @brief Token categories produced by the Lexer.
enum class Tok {
    Name,     ///< Identifier or keyword. Keywords are not distinguished here; see isKeyword().
    Int,      ///< Integer literal. Text keeps any base prefix (`0x`, `0b`, `0o`); underscores removed.
    Float,    ///< Floating-point literal, ready for strtod().
    Str,      ///< String literal, escapes already decoded.
    FStr,     ///< f-string literal: escapes decoded, `{fields}` still raw.
    Bytes,    ///< bytes literal (`b"..."`), escapes decoded. ASCII only in source.
    Op,       ///< Operator or punctuation, e.g. `+`, `**=`, `->`, `(`.
    Newline,  ///< End of a logical line (never emitted inside brackets).
    Indent,   ///< Indentation increased: a new block opens.
    Dedent,   ///< Indentation decreased: one block closes. May come in runs.
    End,      ///< End of input. Always the last token.
};

/// @brief One lexical token with its source position.
struct Token {
    Tok kind;          ///< Category.
    std::string text;  ///< Identifier, operator, or decoded literal contents. Empty for layout tokens.
    int line;          ///< 1-based source line where the token starts.
    int col;           ///< 1-based column where the token starts.
};

/**
 * @brief A compile-time error: bad characters, bad indentation, malformed
 *        literals, a parse failure, or (from the resolver) an undefined name.
 *
 * Thrown by the Lexer, the Parser and the resolver. Runtime errors use
 * DolphinError instead, because those travel through the interpreter as
 * Dolphin exceptions.
 */
struct SyntaxError : std::runtime_error {
    std::string file;  ///< Source file name, as given on the command line.
    int line;          ///< 1-based line of the error.
    std::string kind;  ///< Error class shown to the user: `SyntaxError` or `NameError`.

    /// @param f    File name.
    /// @param l    Line number.
    /// @param msg  Human-readable message (no file/line prefix).
    /// @param k    Error class name.
    SyntaxError(std::string f, int l, const std::string& msg, std::string k = "SyntaxError")
        : std::runtime_error(msg), file(std::move(f)), line(l), kind(std::move(k)) {}
};

/**
 * @brief Converts one source buffer into tokens.
 *
 * Single-use: construct, call run() once.
 *
 * @code
 *   Lexer lx(source, "prog.dol");
 *   std::vector<Token> toks = lx.run();
 * @endcode
 */
class Lexer {
public:
    /**
     * @param src         Full source text (UTF-8).
     * @param file        File name used in error messages.
     * @param lineOffset  Added to every reported line number. The parser uses
     *                    this to re-lex f-string fields so errors inside them
     *                    point at the line of the enclosing f-string.
     */
    Lexer(std::string src, std::string file, int lineOffset = 0);

    /**
     * @brief Lex the whole buffer.
     * @return Tokens, always ending in Tok::Newline (if anything precedes it),
     *         any closing Tok::Dedent tokens, then Tok::End.
     * @throws SyntaxError on any lexical error.
     */
    std::vector<Token> run();

private:
    /**
     * @brief Read a quoted literal starting at the opening quote.
     * @param[in,out] i  Index of the opening quote on entry; one past the
     *                   closing quote on return.
     * @param raw        If true, backslashes are kept literally.
     * @return The literal's contents with escapes decoded (unless @p raw).
     */
    std::string readString(size_t& i, bool raw);

    /// @brief Throw a SyntaxError at the current line.
    [[noreturn]] void error(const std::string& msg) const;

    std::string src_;       ///< Source being lexed.
    std::string file_;      ///< File name for errors.
    int line_ = 1;          ///< Current 1-based line.
    size_t lineStart_ = 0;  ///< Byte offset where the current line begins (for columns).
};

/// @brief Short uppercase name of a token kind, for `dolphin --tokens`.
const char* tokName(Tok t);

/**
 * @brief True if @p s is a reserved word.
 *
 * Includes words reserved for features not implemented yet (`struct`,
 * `layout`, `extern`, `needs`, `mut`, `lambda`, `import`, `from`), so that
 * programs written today don't use them as variable names and break later.
 */
bool isKeyword(const std::string& s);

} // namespace dolphin
