/**
 * @file parser.hpp
 * @brief Builds an abstract syntax tree (Module) from tokens.
 *
 * Statements are parsed by plain recursive descent, one function per
 * statement form. Expressions use Pratt parsing (precedence climbing): each
 * binary operator has a numeric binding power, and parseExpr() keeps
 * consuming operators whose power is at least the caller's minimum. That
 * keeps operator precedence in one table (binPrec() in parser.cpp) instead of
 * spread across a dozen grammar functions.
 *
 * Precedence, lowest to highest (same as Python):
 *
 * | Level | Operators                               |
 * |-------|-----------------------------------------|
 * | 1     | `or`                                    |
 * | 2     | `and`                                   |
 * | 3     | `not` (prefix)                          |
 * | 4     | `== != < <= > >= in` `not in` (chained) |
 * | 5     | `\|`                                    |
 * | 6     | `^`                                     |
 * | 7     | `&`                                     |
 * | 8     | `<< >>`                                 |
 * | 9     | `+ -`                                   |
 * | 10    | `* / // %`                              |
 * | 11    | unary `- + ~`                           |
 * | 12    | `**` (right-associative)                |
 * | 13    | call `()`, subscript `[]`, attribute `.`|
 */
#pragma once
#include "ast.hpp"
#include "lexer.hpp"

namespace dolphin {

/**
 * @brief Recursive-descent / Pratt parser over a token vector.
 *
 * Single-use: construct with the tokens from Lexer::run(), then call
 * parseModule() once. All errors are reported by throwing SyntaxError; the
 * parser does not attempt recovery, so the first error stops compilation.
 */
class Parser {
public:
    /// @param toks  Tokens from Lexer::run(); must end with Tok::End.
    /// @param file  File name for error messages.
    Parser(std::vector<Token> toks, std::string file);

    /// @brief Parse a complete source file.
    /// @throws SyntaxError on the first syntax error.
    Module parseModule();

    /**
     * @brief Parse a single expression that must consume all input.
     *
     * Used for the `{...}` fields of f-strings, which are re-lexed and parsed
     * as standalone expressions.
     */
    ExprP parseStandaloneExpr();

private:
    /// @name Token cursor helpers
    /// @{
    const Token& peek(size_t k = 0) const;          ///< Look ahead @p k tokens without consuming.
    Token next();                                   ///< Consume and return the current token.
    bool isOp(const char* op, size_t k = 0) const;  ///< Is token @p k the operator @p op?
    bool isKw(const char* kw, size_t k = 0) const;  ///< Is token @p k the keyword @p kw?
    bool acceptOp(const char* op);                  ///< Consume @p op if present; report whether it was.
    bool acceptKw(const char* kw);                  ///< Consume keyword @p kw if present.
    void expectOp(const char* op);                  ///< Consume @p op or throw SyntaxError.
    void expectKw(const char* kw);                  ///< Consume keyword @p kw or throw SyntaxError.
    std::string expectName();                       ///< Consume a non-keyword identifier or throw.
    void expectNewline();                           ///< Consume end-of-line (or accept end-of-file).
    [[noreturn]] void error(const std::string& msg) const;  ///< Throw SyntaxError mentioning the current token.
    /// @}

    /// @name Statements
    /// @{
    StmtP parseStmt();        ///< Any statement, compound or simple.
    StmtP parseSimpleStmt();  ///< One-line statement: assignment, return, raise, expression, …
    StmtP shiftStatement(int line, ExprP shift);   ///< `name << expr` as a statement (see parser.cpp).
    Block parseBlock();       ///< `:` then either an indented block or a one-line body.
    StmtP parseIf();          ///< `if` / `elif` / `else`.
    StmtP parseWhile();       ///< `while cond:`.
    StmtP parseFor();         ///< `for name in expr:`.
    StmtP parseDef();         ///< `def name(params) -> type:`.
    StmtP parseTry();         ///< `try:` with one or more `except` clauses.
    /// @}

    /// @name Expressions
    /// @{
    /**
     * @brief Pratt loop: parse an expression whose operators all bind at
     *        least as tightly as @p minPrec.
     */
    ExprP parseExpr(int minPrec = 0);
    ExprP parsePrefix();             ///< Unary operators, then an atom with postfix operations.
    ExprP parsePostfix(ExprP e);     ///< Calls, subscripts/slices and attribute access, left to right.
    ExprP parseAtom();               ///< Literals, names, parenthesized expressions, list and dict displays.
    ExprP parseFString(const Token& t);  ///< Split an f-string token into text and expression parts.
    /// @}

    std::vector<Token> t_;  ///< Token stream.
    size_t p_ = 0;          ///< Index of the current token.
    std::string file_;      ///< File name for errors.
};

/**
 * @brief Convenience: lex and parse a whole source buffer.
 * @throws SyntaxError on any lexical or syntax error.
 */
Module parseSource(const std::string& src, const std::string& file);

} // namespace dolphin
