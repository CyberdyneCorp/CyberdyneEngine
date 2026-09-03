"""Parse the argument list of a CY_REFLECT_TYPE or CY_REFLECT_FIELD annotation.

Task 1.1.3. The macros in `cy/core/reflect/annotations.h` stringise their argument list into a
clang `annotate` attribute, so what arrives here is source text the preprocessor has already
normalised:

    Range(0.0, 10000.0, 1.0), Unit(Percent), Tooltip("The value healing cannot exceed")

The grammar is small and closed, because an attribute the engine does not know is a build error
naming the field rather than a value silently ignored at run time:

    list      := entry ("," entry)*
    entry     := name | name "(" arg ("," arg)* ")"
    arg       := value | name "=" value
    value     := number | string | "true" | "false" | name

`name = value` covers two cases with one production: the enumerators of Enum and Flags, and the
named parameters of an attribute a module declared for itself.

Every failure raises AnnotationError with a message that reads as a compiler diagnostic; the caller
prefixes it with the declaration it came from.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field


class AnnotationError(Exception):
    """A malformed annotation. The message is the diagnostic a contributor will read."""


# One token per alternative, longest first where two could match the same text.
_TOKEN = re.compile(
    r"""
    (?P<space>\s+)
  | (?P<string>"(?:[^"\\]|\\.)*")
  | (?P<number>[+-]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fFuUlL]*)
  | (?P<name>[A-Za-z_][A-Za-z_0-9:]*)
  | (?P<punct>[(),=])
    """,
    re.VERBOSE,
)


@dataclass(frozen=True)
class Argument:
    """One argument of one attribute. `key` is empty for a positional argument."""

    key: str
    value: object  # str for a string or a bare name, int, float, or bool
    is_name: bool  # the value was a bare identifier rather than a literal


@dataclass(frozen=True)
class Annotation:
    """One attribute as written, before it is checked against a schema."""

    name: str
    arguments: list[Argument] = field(default_factory=list)
    had_parentheses: bool = False


def _tokenize(text: str) -> list[tuple[str, str]]:
    tokens: list[tuple[str, str]] = []
    position = 0
    while position < len(text):
        match = _TOKEN.match(text, position)
        if match is None:
            raise AnnotationError(f"unexpected character {text[position]!r} in the attribute list")
        position = match.end()
        kind = match.lastgroup
        assert kind is not None
        if kind != "space":
            tokens.append((kind, match.group()))
    return tokens


def _literal(kind: str, text: str) -> tuple[object, bool]:
    """The Python value of one token, and whether it was a bare identifier."""
    if kind == "string":
        return _unescape(text[1:-1]), False
    if kind == "number":
        return _number(text), False
    if text == "true":
        return True, False
    if text == "false":
        return False, False
    return text, True


def _unescape(text: str) -> str:
    out: list[str] = []
    index = 0
    while index < len(text):
        character = text[index]
        if character == "\\" and index + 1 < len(text):
            index += 1
            out.append({"n": "\n", "t": "\t", "0": "\0"}.get(text[index], text[index]))
        else:
            out.append(character)
        index += 1
    return "".join(out)


def _number(text: str) -> object:
    body = text.rstrip("fFuUlL")
    if any(character in body for character in ".eE"):
        return float(body)
    return int(body)


class _Parser:
    """Recursive descent over the token list. One instance per annotation list."""

    def __init__(self, tokens: list[tuple[str, str]]) -> None:
        self._tokens = tokens
        self._index = 0

    def _peek(self) -> tuple[str, str] | None:
        return self._tokens[self._index] if self._index < len(self._tokens) else None

    def _take(self) -> tuple[str, str]:
        token = self._peek()
        if token is None:
            raise AnnotationError("the attribute list ends in the middle of an attribute")
        self._index += 1
        return token

    def _expect(self, text: str) -> None:
        kind, actual = self._take()
        del kind
        if actual != text:
            raise AnnotationError(f"expected {text!r} in the attribute list, found {actual!r}")

    def parse_list(self) -> list[Annotation]:
        annotations: list[Annotation] = []
        if self._peek() is None:
            return annotations
        while True:
            annotations.append(self._parse_attribute())
            token = self._peek()
            if token is None:
                return annotations
            self._expect(",")

    def _parse_attribute(self) -> Annotation:
        kind, name = self._take()
        if kind != "name":
            raise AnnotationError(f"expected an attribute name, found {name!r}")
        if self._peek() != ("punct", "("):
            return Annotation(name=name)
        self._expect("(")
        arguments: list[Argument] = []
        if self._peek() == ("punct", ")"):
            self._expect(")")
            return Annotation(name=name, arguments=arguments, had_parentheses=True)
        while True:
            arguments.append(self._parse_argument())
            kind, text = self._take()
            del kind
            if text == ")":
                return Annotation(name=name, arguments=arguments, had_parentheses=True)
            if text != ",":
                raise AnnotationError(
                    f"expected ',' or ')' in the arguments of {name}, found {text!r}"
                )

    def _parse_argument(self) -> Argument:
        kind, text = self._take()
        if kind == "punct":
            raise AnnotationError(f"expected an argument, found {text!r}")
        if kind == "name" and self._peek() == ("punct", "="):
            self._expect("=")
            value_kind, value_text = self._take()
            if value_kind == "punct":
                raise AnnotationError(f"expected a value after '{text} =', found {value_text!r}")
            value, is_name = _literal(value_kind, value_text)
            return Argument(key=text, value=value, is_name=is_name)
        value, is_name = _literal(kind, text)
        return Argument(key="", value=value, is_name=is_name)


def parse(text: str) -> list[Annotation]:
    """Every attribute in one annotation's argument list, in the order written."""
    return _Parser(_tokenize(text)).parse_list()
