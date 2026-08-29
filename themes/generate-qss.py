#!/usr/bin/env python3
#
# Generate the QSS stylesheets of the built-in Mumble themes from their Sass sources.
#
# The SCSS files used here only employ two Sass features: "@import" and "$variable"
# interpolation. Therefore the full Sass compiler is not needed and this script is
# enough to (re-)generate the QSS files:
#
#   python3 themes/generate-qss.py                       # regenerate every theme
#   python3 themes/generate-qss.py themes/Nord/source/   # regenerate a single theme
#
# It is intentionally kept in sync with the workflow described in themes/README.md.

import re
import sys
from pathlib import Path

VARIABLE_DECLARATION = re.compile(r"^\s*\$([A-Za-z0-9_-]+)\s*:\s*(.+?)\s*;?\s*$")
IMPORT_STATEMENT     = re.compile(r"^\s*@import\s+['\"]([^'\"]+)['\"]\s*;\s*$")
VARIABLE_REFERENCE   = re.compile(r"\$([A-Za-z0-9_-]+)")

# A number as it can appear in a Sass arithmetic expression (e.g. "4px", "1.5", "-3")
ARITHMETIC_NUMBER   = r"-?(?:\d+\.\d*|\.\d+|\d+)(?:px|pt|em|%)?"
ARITHMETIC_TOKEN    = re.compile(rf"({ARITHMETIC_NUMBER})|([-+*/])")
# Expressions like "$base-padding - 3" or "$base-padding*4+4". A variable reference is required so
# that value lists like "1px -2px" are never mistaken for arithmetic.
VARIABLE_EXPRESSION = re.compile(
    rf"\$[A-Za-z0-9_-]+(?:\s*(?:[-+*/])\s*(?:\$[A-Za-z0-9_-]+|{ARITHMETIC_NUMBER}))+")


def format_number(value: float, unit: str) -> str:
    if value == int(value):
        value = int(value)
    return f"{value}{unit}"


def evaluate_arithmetic(expression: str) -> str:
    """Evaluate a Sass arithmetic expression like "4px*4+4"; returns the input unchanged if it does
    not fully consist of numbers and operators."""
    tokens = ARITHMETIC_TOKEN.findall(expression)
    numbers = [token for token, _ in tokens]
    if "".join(expression.split()) == "" or not numbers:
        return expression
    # Validate the structure: numbers and operators have to alternate
    consumed = "".join(part for pair in tokens for part in pair if part)
    if consumed.replace(" ", "") != expression.replace(" ", ""):
        return expression

    def split_unit(token: str):
        match = re.fullmatch(rf"(-?(?:\d+\.\d*|\.\d+|\d+))(px|pt|em|%)?", token)
        return float(match.group(1)), match.group(2) or ""

    # First pass: * and / bind tighter than + and -
    unit = next((split_unit(token)[1] for token, is_operator in tokens if not is_operator and split_unit(token)[1]), "")
    values = [split_unit(token)[0] if not is_operator else is_operator for token, is_operator in tokens]

    for precedence in ("*/", "+-"):
        index = 1
        while index < len(values):
            operator = values[index]
            if isinstance(operator, str) and operator in precedence:
                left, right = values[index - 1], values[index + 1]
                if operator == "*":
                    result = left * right
                elif operator == "/":
                    result = left / right
                elif operator == "+":
                    result = left + right
                else:
                    result = left - right
                values[index - 1 : index + 2] = [result]
            else:
                index += 2
    return format_number(values[0], unit)


def strip_line_comment(line: str) -> str:
    # Like Sass, line comments are dropped - but only outside of block comments (which may
    # contain URLs)
    return re.sub(r"//.*$", "", line)


def canonical(name: str) -> str:
    # Like Sass, "-" and "_" are interchangeable within variable names
    return name.replace("_", "-")


def lookup(variables: dict, name: str):
    if name in variables:
        return variables[name]
    return variables.get(canonical(name))


def resolve(definition: dict) -> dict:
    """Resolve variables that reference other variables."""
    resolved = {}
    for _ in range(len(definition) + 1):
        changed = False
        for name, value in definition.items():
            if name in resolved:
                continue
            if not VARIABLE_REFERENCE.search(value):
                resolved[name] = value
                changed = True
        definition = {name: resolved.get(name, definition[name]) for name in definition}
        for name, value in definition.items():
            if name not in resolved:
                definition[name] = VARIABLE_REFERENCE.sub(
                    lambda m: lookup(resolved, m.group(1)) or m.group(0), value)
        if not changed:
            break
    unresolved = []
    for name, value in definition.items():
        match = VARIABLE_REFERENCE.search(value)
        if match and lookup(resolved, match.group(1)) is None:
            unresolved.append(name)
    if unresolved:
        raise RuntimeError(f"Unresolvable variables: {unresolved}")
    return definition


def parse(source_file: Path):
    """Collect variable declarations and the plain stylesheet body of a source file."""
    variables, body = {}, []
    comment = None
    for line in source_file.read_text(encoding="utf-8").splitlines():
        if comment is not None:
            comment += "\n" + line
            if "*/" in line:
                body.append(comment)
                comment = None
            continue
        line = strip_line_comment(line)
        if line.lstrip().startswith("/*"):
            if "*/" not in line:
                comment = line
                continue
            body.append(line)
            continue
        match = IMPORT_STATEMENT.match(line)
        if match:
            imported_vars, imported_body = parse(source_file.parent / f"{match.group(1)}.scss")
            # Declarations of the importing file take precedence over imported ones
            imported_vars.update(variables)
            variables = imported_vars
            body.extend(imported_body)
            continue
        match = VARIABLE_DECLARATION.match(line)
        if match:
            variables[match.group(1)] = match.group(2).rstrip(";").strip()
            continue
        if line.strip():
            body.append(line)
    if comment is not None:
        raise RuntimeError(f"Unterminated block comment in {source_file}")
    return variables, body


def generate(source_file: Path) -> str:
    variables, body = parse(source_file)
    variables = resolve(variables)

    def substitute(expression: str) -> str:
        substituted = VARIABLE_REFERENCE.sub(lambda m: lookup(variables, m.group(1)) or m.group(0), expression)
        if substituted != expression:
            return evaluate_arithmetic(substituted)
        return expression

    def substitute_line(line: str) -> str:
        line = VARIABLE_EXPRESSION.sub(lambda m: substitute(m.group(0)), line)
        return VARIABLE_REFERENCE.sub(lambda m: lookup(variables, m.group(1)) or m.group(0), line)

    # Like Sass, drop a trailing comma of a selector list (e.g. "selector, {")
    return re.sub(r",(\s*\{)", r"\1", "\n".join(substitute_line(line) for line in body)) + "\n"


def theme_sources(path: Path):
    """All top-level .scss files (one per generated stylesheet) for the given argument.

    Accepts the themes root directory, a single theme directory, or a single .scss file.
    """
    if path.is_file():
        return iter([path])
    return iter(sorted(path.glob("source/*.scss")) + sorted(path.glob("*/source/*.scss")))


def main(argv):
    arguments = argv[1:] or [str(Path(__file__).parent)]
    generated = 0
    for argument in arguments:
        for source_file in theme_sources(Path(argument).resolve()):
            # themes/<Theme>/source/<Name>.scss -> themes/<Theme>/<Name>.qss
            output_file = source_file.parent.parent / f"{source_file.stem}.qss"
            output_file.write_text(generate(source_file), encoding="utf-8")
            print(f"{source_file} -> {output_file}")
            generated += 1
    if generated == 0:
        raise RuntimeError(f"No .scss sources found for: {arguments}")


if __name__ == "__main__":
    main(sys.argv)
