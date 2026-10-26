#!/usr/bin/env python3
"""CSM lexer, parser, semantic analyzer, and compiler-facing song model.

This module is the language front end used directly by ``csmc.py``. It checks
syntax and musical relationships, resolves references, and returns the
normalized in-memory model consumed by the selected backend. It is not a
standalone compilation stage and does not write an intermediary file.

The implementation intentionally uses only the Python standard library.
"""

from __future__ import annotations

import hashlib
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Iterator, Optional, Sequence


# ---------------------------------------------------------------------------
# Diagnostics
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class SourcePos:
    line: int
    column: int
    offset: int

    def to_json(self) -> dict[str, int]:
        return {"line": self.line, "column": self.column, "offset": self.offset}


@dataclass
class Diagnostic:
    severity: str
    code: str
    message: str
    pos: SourcePos
    context: Optional[str] = None

    def to_json(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "severity": self.severity,
            "code": self.code,
            "message": self.message,
            "location": self.pos.to_json(),
        }
        if self.context:
            result["context"] = self.context
        return result


class Diagnostics:
    def __init__(self) -> None:
        self.items: list[Diagnostic] = []

    def error(self, code: str, message: str, pos: SourcePos, context: str | None = None) -> None:
        self.items.append(Diagnostic("error", code, message, pos, context))

    def warning(self, code: str, message: str, pos: SourcePos, context: str | None = None) -> None:
        self.items.append(Diagnostic("warning", code, message, pos, context))

    @property
    def error_count(self) -> int:
        return sum(1 for item in self.items if item.severity == "error")

    @property
    def warning_count(self) -> int:
        return sum(1 for item in self.items if item.severity == "warning")

    def sorted(self) -> list[Diagnostic]:
        order = {"error": 0, "warning": 1}
        return sorted(self.items, key=lambda d: (d.pos.line, d.pos.column, order.get(d.severity, 9), d.code))


# ---------------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    pos: SourcePos
    end_offset: int

    def to_json(self) -> dict[str, Any]:
        return {"kind": self.kind, "text": self.text, "location": self.pos.to_json()}


PUNCTUATION = {
    "{": "LBRACE",
    "}": "RBRACE",
    "[": "LBRACKET",
    "]": "RBRACKET",
    "(": "LPAREN",
    ")": "RPAREN",
    "=": "EQUAL",
    ";": "SEMI",
    ",": "COMMA",
    ".": "DOT",
    "*": "STAR",
    "|": "BAR",
    "~": "REST",
    "-": "MINUS",
    "/": "SLASH",
    "@": "AT",
    "^": "CARET",
    "<": "LT",
    ">": "GT",
}

NOTE_RE = re.compile(r"[a-g](?:b|#)?[0-9]+(?![A-Za-z0-9_])")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
INT_RE = re.compile(r"[0-9]+")


class Lexer:
    def __init__(self, source: str, diagnostics: Diagnostics) -> None:
        self.source = source
        self.diagnostics = diagnostics
        self.offset = 0
        self.line = 1
        self.column = 1
        self.tokens: list[Token] = []

    def current_pos(self) -> SourcePos:
        return SourcePos(self.line, self.column, self.offset)

    def advance_text(self, text: str) -> None:
        for char in text:
            self.offset += 1
            if char == "\n":
                self.line += 1
                self.column = 1
            else:
                self.column += 1

    def emit(self, kind: str, text: str, start: SourcePos) -> None:
        self.tokens.append(Token(kind, text, start, start.offset + len(text)))

    def lex(self) -> list[Token]:
        length = len(self.source)
        while self.offset < length:
            char = self.source[self.offset]

            if char.isspace():
                self.advance_text(char)
                continue

            if self.source.startswith("//", self.offset):
                newline = self.source.find("\n", self.offset)
                if newline == -1:
                    self.advance_text(self.source[self.offset:])
                else:
                    self.advance_text(self.source[self.offset:newline])
                continue

            start = self.current_pos()

            note_match = NOTE_RE.match(self.source, self.offset)
            if note_match:
                text = note_match.group(0)
                self.emit("NOTE", text, start)
                self.advance_text(text)
                continue

            ident_match = IDENT_RE.match(self.source, self.offset)
            if ident_match:
                text = ident_match.group(0)
                self.emit("IDENT", text, start)
                self.advance_text(text)
                continue

            int_match = INT_RE.match(self.source, self.offset)
            if int_match:
                text = int_match.group(0)
                self.emit("INT", text, start)
                self.advance_text(text)
                continue

            kind = PUNCTUATION.get(char)
            if kind:
                self.emit(kind, char, start)
                self.advance_text(char)
                continue

            self.diagnostics.error("LEX001", f"unexpected character {char!r}", start)
            self.advance_text(char)

        eof = self.current_pos()
        self.tokens.append(Token("EOF", "", eof, eof.offset))
        return self.tokens


# ---------------------------------------------------------------------------
# Structural AST
# ---------------------------------------------------------------------------


@dataclass
class Tag:
    name: str
    value: str | int
    value_kind: str
    pos: SourcePos

    def to_json(self) -> dict[str, Any]:
        return {"name": self.name, "value": self.value, "value_kind": self.value_kind, "location": self.pos.to_json()}


@dataclass
class Assignment:
    name: str
    expr_tokens: list[Token]
    raw: str
    pos: SourcePos
    parsed: Optional["Expr"] = None
    pattern: Optional["PatternSequence"] = None


@dataclass
class Declaration:
    decl_type: str
    name: Optional[str]
    form: str  # block | value
    pos: SourcePos
    tags: list[Tag] = field(default_factory=list)
    items: list["Node"] = field(default_factory=list)
    expr_tokens: list[Token] = field(default_factory=list)
    raw: str = ""
    parsed: Optional["Expr"] = None
    pattern: Optional["PatternSequence"] = None
    parent: Optional["Declaration"] = field(default=None, repr=False)


Node = Assignment | Declaration


@dataclass
class Document:
    items: list[Declaration]


class StructuralParser:
    def __init__(self, tokens: list[Token], source: str, diagnostics: Diagnostics) -> None:
        self.tokens = tokens
        self.source = source
        self.diagnostics = diagnostics
        self.index = 0

    def peek(self, distance: int = 0) -> Token:
        idx = min(self.index + distance, len(self.tokens) - 1)
        return self.tokens[idx]

    def advance(self) -> Token:
        token = self.peek()
        if token.kind != "EOF":
            self.index += 1
        return token

    def match(self, kind: str) -> Optional[Token]:
        if self.peek().kind == kind:
            return self.advance()
        return None

    def expect(self, kind: str, message: str) -> Optional[Token]:
        if self.peek().kind == kind:
            return self.advance()
        self.diagnostics.error("SYN001", message, self.peek().pos)
        return None

    def parse_document(self) -> Document:
        items: list[Declaration] = []
        while self.peek().kind != "EOF":
            if self.peek().kind != "IDENT":
                self.diagnostics.error("SYN002", "expected a top-level declaration", self.peek().pos)
                self.synchronize_top_level()
                continue
            node = self.parse_item(parent=None, top_level=True)
            if isinstance(node, Declaration):
                items.append(node)
            elif node is not None:
                self.diagnostics.error("SYN003", "top-level assignments are not allowed", node.pos)
        return Document(items)

    def parse_item(self, parent: Optional[Declaration], top_level: bool = False) -> Optional[Node]:
        first = self.expect("IDENT", "expected an identifier")
        if first is None:
            return None

        # Assignment starts with a potentially qualified/indexed reference.
        saved_index = self.index
        lhs = self.parse_reference_tail(first.text, allow_index=True)
        if self.peek().kind == "EQUAL":
            self.advance()
            expr_tokens, raw = self.capture_expression(first.pos)
            return Assignment(lhs, expr_tokens, raw, first.pos)

        # Not an assignment: rewind and parse a declaration.
        self.index = saved_index
        decl_type = first.text

        if self.peek().kind == "LBRACE":
            decl = Declaration(decl_type, None, "block", first.pos, parent=parent)
            self.parse_block_body(decl)
            return decl

        if self.peek().kind != "IDENT":
            self.diagnostics.error(
                "SYN004",
                f"expected a name, '=' or '{{' after declaration type '{decl_type}'",
                self.peek().pos,
            )
            self.synchronize_item()
            return None

        name_first = self.advance()
        name = self.parse_reference_tail(name_first.text, allow_index=False)
        tags: list[Tag] = []
        if self.peek().kind == "LBRACKET":
            tags = self.parse_tags()

        if self.peek().kind == "LBRACE":
            decl = Declaration(decl_type, name, "block", first.pos, tags=tags, parent=parent)
            self.parse_block_body(decl)
            return decl

        if tags:
            self.diagnostics.error("SYN005", "tags are only allowed on block declarations", self.peek().pos)

        if self.match("EQUAL"):
            expr_tokens, raw = self.capture_expression(first.pos)
            return Declaration(
                decl_type,
                name,
                "value",
                first.pos,
                tags=tags,
                expr_tokens=expr_tokens,
                raw=raw,
                parent=parent,
            )

        self.diagnostics.error(
            "SYN006",
            f"expected '{{' or '=' after declaration '{decl_type} {name}'",
            self.peek().pos,
        )
        self.synchronize_item()
        return None

    def parse_reference_tail(self, initial: str, allow_index: bool) -> str:
        parts = [initial]
        while True:
            if self.match("DOT"):
                ident = self.peek()
                # A component such as b00 is lexically note-shaped, but after
                # a dot it is unambiguously part of a qualified identifier.
                if ident.kind not in {"IDENT", "NOTE"}:
                    self.diagnostics.error("SYN001", "expected identifier after '.'", ident.pos)
                    break
                self.advance()
                parts.extend([".", ident.text])
                continue
            if allow_index and self.match("LBRACKET"):
                ident = self.expect("IDENT", "expected identifier inside reference index")
                self.expect("RBRACKET", "expected ']' after reference index")
                if ident:
                    parts.extend(["[", ident.text, "]"])
                continue
            break
        return "".join(parts)

    def parse_tags(self) -> list[Tag]:
        tags: list[Tag] = []
        self.expect("LBRACKET", "expected '['")
        if self.match("RBRACKET"):
            return tags
        while self.peek().kind not in {"RBRACKET", "EOF"}:
            name = self.expect("IDENT", "expected tag name")
            self.expect("EQUAL", "expected '=' after tag name")
            value_token = self.peek()
            if value_token.kind == "INT":
                self.advance()
                value: str | int = int(value_token.text)
                value_kind = "integer"
            elif value_token.kind == "IDENT":
                first = self.advance()
                value = self.parse_reference_tail(first.text, allow_index=False)
                value_kind = "reference"
            else:
                self.diagnostics.error("SYN007", "tag value must be an integer or identifier", value_token.pos)
                value = "<error>"
                value_kind = "error"
                self.advance()
            if name:
                tags.append(Tag(name.text, value, value_kind, name.pos))
            if self.match("COMMA"):
                if self.peek().kind == "RBRACKET":
                    break
                continue
            break
        self.expect("RBRACKET", "expected ']' after tag list")
        return tags

    def parse_block_body(self, declaration: Declaration) -> None:
        self.expect("LBRACE", "expected '{'")
        while self.peek().kind not in {"RBRACE", "EOF"}:
            if self.peek().kind != "IDENT":
                self.diagnostics.error("SYN008", "expected an assignment or declaration", self.peek().pos)
                self.synchronize_item()
                continue
            item = self.parse_item(parent=declaration)
            if item is not None:
                declaration.items.append(item)
        if self.peek().kind == "RBRACE":
            self.advance()
        else:
            self.diagnostics.error("SYN009", f"unterminated block '{declaration.decl_type}'", declaration.pos)

    def capture_expression(self, owner_pos: SourcePos) -> tuple[list[Token], str]:
        start_index = self.index
        paren_depth = 0
        bracket_depth = 0

        while True:
            token = self.peek()
            if token.kind == "EOF":
                self.diagnostics.error("SYN010", "unterminated assignment; expected ';'", owner_pos)
                break
            if token.kind == "RBRACE" and paren_depth == 0 and bracket_depth == 0:
                self.diagnostics.error("SYN011", "missing ';' before '}'", token.pos)
                break
            if token.kind == "SEMI" and paren_depth == 0 and bracket_depth == 0:
                break
            if token.kind == "LPAREN":
                paren_depth += 1
            elif token.kind == "RPAREN":
                paren_depth -= 1
                if paren_depth < 0:
                    self.diagnostics.error("SYN012", "unmatched ')' in expression", token.pos)
                    paren_depth = 0
            elif token.kind == "LBRACKET":
                bracket_depth += 1
            elif token.kind == "RBRACKET":
                bracket_depth -= 1
                if bracket_depth < 0:
                    self.diagnostics.error("SYN013", "unmatched ']' in expression", token.pos)
                    bracket_depth = 0
            self.advance()

        expr_tokens = self.tokens[start_index:self.index]
        if self.peek().kind == "SEMI":
            self.advance()
        if not expr_tokens:
            self.diagnostics.error("SYN014", "empty expression", owner_pos)
            return [], ""
        raw_start = expr_tokens[0].pos.offset
        raw_end = expr_tokens[-1].end_offset
        return expr_tokens, self.source[raw_start:raw_end]

    def synchronize_item(self) -> None:
        depth = 0
        while self.peek().kind != "EOF":
            token = self.peek()
            if token.kind == "LBRACE":
                depth += 1
            elif token.kind == "RBRACE":
                if depth == 0:
                    return
                depth -= 1
            elif token.kind == "SEMI" and depth == 0:
                self.advance()
                return
            self.advance()

    def synchronize_top_level(self) -> None:
        while self.peek().kind not in {"IDENT", "EOF"}:
            self.advance()


# ---------------------------------------------------------------------------
# Generic expressions
# ---------------------------------------------------------------------------


@dataclass
class Expr:
    kind: str
    pos: SourcePos
    value: Any = None
    name: Optional[str] = None
    args: list["Expr"] = field(default_factory=list)
    items: list["Expr"] = field(default_factory=list)
    resolved: Optional[dict[str, str]] = None

    def to_json(self) -> dict[str, Any]:
        result: dict[str, Any] = {"kind": self.kind, "location": self.pos.to_json()}
        if self.kind == "integer":
            result["value"] = self.value
        elif self.kind == "reference":
            result["name"] = self.name
            if self.resolved:
                result["resolved"] = self.resolved
        elif self.kind == "call":
            result["name"] = self.name
            result["arguments"] = [arg.to_json() for arg in self.args]
        elif self.kind == "array":
            result["items"] = [item.to_json() for item in self.items]
        return result


class ExpressionParser:
    def __init__(self, tokens: Sequence[Token], diagnostics: Diagnostics, context: str) -> None:
        self.tokens = list(tokens) + [Token("EOF", "", tokens[-1].pos if tokens else SourcePos(1, 1, 0), tokens[-1].end_offset if tokens else 0)]
        self.diagnostics = diagnostics
        self.context = context
        self.index = 0

    def peek(self) -> Token:
        return self.tokens[min(self.index, len(self.tokens) - 1)]

    def advance(self) -> Token:
        token = self.peek()
        if token.kind != "EOF":
            self.index += 1
        return token

    def match(self, kind: str) -> Optional[Token]:
        if self.peek().kind == kind:
            return self.advance()
        return None

    def parse(self) -> Optional[Expr]:
        expr = self.parse_expr()
        if expr is None:
            return None
        if self.peek().kind != "EOF":
            token = self.peek()
            if token.kind in {"DOT", "SLASH"}:
                self.diagnostics.error(
                    "EXP001",
                    "only integer literals are allowed; use a function call for multi-part values",
                    token.pos,
                    self.context,
                )
            else:
                self.diagnostics.error("EXP002", f"unexpected token {token.text!r} in expression", token.pos, self.context)
        return expr

    def parse_expr(self) -> Optional[Expr]:
        token = self.peek()
        if token.kind == "MINUS":
            minus = self.advance()
            integer = self.peek()
            if integer.kind != "INT":
                self.diagnostics.error("EXP003", "unary '-' may only precede an integer", minus.pos, self.context)
                return None
            self.advance()
            return Expr("integer", minus.pos, value=-int(integer.text))

        if token.kind == "INT":
            self.advance()
            return Expr("integer", token.pos, value=int(token.text))

        if token.kind == "LBRACKET":
            return self.parse_array()

        if token.kind == "IDENT":
            first = self.advance()
            name = self.parse_reference_tail(first.text)
            if self.match("LPAREN"):
                args: list[Expr] = []
                if self.match("RPAREN"):
                    return Expr("call", first.pos, name=name, args=args)
                while self.peek().kind != "EOF":
                    arg = self.parse_expr()
                    if arg is not None:
                        args.append(arg)
                    if self.match("COMMA"):
                        if self.match("RPAREN"):
                            return Expr("call", first.pos, name=name, args=args)
                        continue
                    if self.match("RPAREN"):
                        return Expr("call", first.pos, name=name, args=args)
                    self.diagnostics.error("EXP004", "expected ',' or ')' in function call", self.peek().pos, self.context)
                    return Expr("call", first.pos, name=name, args=args)
                self.diagnostics.error("EXP005", "unterminated function call", first.pos, self.context)
                return Expr("call", first.pos, name=name, args=args)
            return Expr("reference", first.pos, name=name)

        self.diagnostics.error("EXP006", f"expected expression, found {token.text!r}", token.pos, self.context)
        self.advance()
        return None

    def parse_array(self) -> Expr:
        start = self.advance()
        items: list[Expr] = []
        if self.match("RBRACKET"):
            return Expr("array", start.pos, items=items)
        while self.peek().kind != "EOF":
            item = self.parse_expr()
            if item is not None:
                items.append(item)
            if self.match("COMMA"):
                if self.match("RBRACKET"):
                    return Expr("array", start.pos, items=items)
                continue
            if self.match("RBRACKET"):
                return Expr("array", start.pos, items=items)
            self.diagnostics.error("EXP007", "expected ',' or ']' in array", self.peek().pos, self.context)
            break
        self.diagnostics.error("EXP008", "unterminated array", start.pos, self.context)
        return Expr("array", start.pos, items=items)

    def parse_reference_tail(self, initial: str) -> str:
        parts = [initial]
        while True:
            if self.match("DOT"):
                token = self.peek()
                if token.kind not in {"IDENT", "NOTE"}:
                    self.diagnostics.error("EXP009", "expected identifier after '.'", token.pos, self.context)
                    break
                self.advance()
                parts.extend([".", token.text])
                continue
            if self.match("LBRACKET"):
                token = self.peek()
                if token.kind != "IDENT":
                    self.diagnostics.error("EXP010", "expected identifier inside reference index", token.pos, self.context)
                    break
                self.advance()
                if not self.match("RBRACKET"):
                    self.diagnostics.error("EXP011", "expected ']' after reference index", self.peek().pos, self.context)
                parts.extend(["[", token.text, "]"])
                continue
            break
        return "".join(parts)


# ---------------------------------------------------------------------------
# Pattern expressions
# ---------------------------------------------------------------------------


@dataclass
class PatternModifier:
    kind: str
    value: Any
    pos: SourcePos

    def to_json(self) -> dict[str, Any]:
        return {"kind": self.kind, "value": self.value, "location": self.pos.to_json()}


@dataclass
class PatternEvent:
    kind: str
    value: str
    pos: SourcePos
    duration_ticks: Optional[int] = None
    modifiers: list[PatternModifier] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        result = {"kind": self.kind, "value": self.value, "location": self.pos.to_json()}
        if self.duration_ticks is not None:
            result["duration_ticks"] = self.duration_ticks
        if self.modifiers:
            result["modifiers"] = [modifier.to_json() for modifier in self.modifiers]
        return result


@dataclass
class PatternSlot:
    events: list[PatternEvent]
    subdivided: bool
    pos: SourcePos

    def to_json(self) -> dict[str, Any]:
        return {
            "subdivided": self.subdivided,
            "events": [event.to_json() for event in self.events],
            "location": self.pos.to_json(),
        }


@dataclass
class PatternTerm:
    kind: str  # bar | reference
    pos: SourcePos
    slots: list[PatternSlot] = field(default_factory=list)
    reference: Optional[str] = None
    repeat: int = 1
    resolved: Optional[dict[str, str]] = None

    def to_json(self) -> dict[str, Any]:
        result: dict[str, Any] = {"kind": self.kind, "location": self.pos.to_json()}
        if self.kind == "bar":
            result["slots"] = [slot.to_json() for slot in self.slots]
            result["slot_count"] = len(self.slots)
        else:
            result["reference"] = self.reference
            result["repeat"] = self.repeat
            if self.resolved:
                result["resolved"] = self.resolved
        return result


@dataclass
class PatternSequence:
    terms: list[PatternTerm]
    pos: SourcePos

    def to_json(self) -> dict[str, Any]:
        return {"kind": "pattern_sequence", "terms": [term.to_json() for term in self.terms], "location": self.pos.to_json()}

    def dependencies(self) -> list[str]:
        return [term.reference for term in self.terms if term.kind == "reference" and term.reference is not None]


class PatternParser:
    def __init__(self, tokens: Sequence[Token], diagnostics: Diagnostics, context: str) -> None:
        eof_pos = tokens[-1].pos if tokens else SourcePos(1, 1, 0)
        eof_end = tokens[-1].end_offset if tokens else eof_pos.offset
        self.tokens = list(tokens) + [Token("EOF", "", eof_pos, eof_end)]
        self.diagnostics = diagnostics
        self.context = context
        self.index = 0

    def peek(self) -> Token:
        return self.tokens[min(self.index, len(self.tokens) - 1)]

    def peek_ahead(self, distance: int) -> Token:
        return self.tokens[min(self.index + distance, len(self.tokens) - 1)]

    def advance(self) -> Token:
        token = self.peek()
        if token.kind != "EOF":
            self.index += 1
        return token

    def match(self, kind: str) -> Optional[Token]:
        if self.peek().kind == kind:
            return self.advance()
        return None

    def parse(self) -> Optional[PatternSequence]:
        if self.peek().kind == "EOF":
            return None
        start = self.peek().pos
        terms: list[PatternTerm] = []
        expecting_term = True
        while self.peek().kind != "EOF":
            if self.match("COMMA"):
                if expecting_term:
                    self.diagnostics.error("PAT001", "unexpected comma in pattern sequence", self.tokens[self.index - 1].pos, self.context)
                expecting_term = True
                continue

            token = self.peek()
            if token.kind == "BAR":
                terms.append(self.parse_bar())
                expecting_term = False
                continue

            if token.kind == "IDENT":
                first = self.advance()
                name = self.parse_qualified_name(first.text)
                repeat = 1
                if self.match("STAR"):
                    count = self.peek()
                    if count.kind != "INT":
                        self.diagnostics.error("PAT002", "pattern repetition requires a positive integer", count.pos, self.context)
                    else:
                        self.advance()
                        repeat = int(count.text)
                        if repeat <= 0:
                            self.diagnostics.error("PAT003", "pattern repetition must be positive", count.pos, self.context)
                terms.append(PatternTerm("reference", first.pos, reference=name, repeat=repeat))
                expecting_term = False
                continue

            self.diagnostics.error("PAT004", f"unexpected token {token.text!r} in pattern expression", token.pos, self.context)
            self.advance()
            expecting_term = True

        if not terms:
            self.diagnostics.error("PAT005", "empty pattern expression", start, self.context)
        return PatternSequence(terms, start)

    def parse_bar(self) -> PatternTerm:
        start = self.advance()
        slots: list[PatternSlot] = []
        while self.peek().kind not in {"BAR", "EOF"}:
            token = self.peek()
            if token.kind == "LBRACKET":
                slots.append(self.parse_subdivision())
                continue
            event = self.parse_event(in_subdivision=False)
            if event:
                slots.append(PatternSlot([event], False, event.pos))
        if not self.match("BAR"):
            self.diagnostics.error("PAT006", "unterminated bar; expected '|'", start.pos, self.context)
        return PatternTerm("bar", start.pos, slots=slots)

    def parse_subdivision(self) -> PatternSlot:
        start = self.advance()
        events: list[PatternEvent] = []
        while self.peek().kind not in {"RBRACKET", "EOF"}:
            event = self.parse_event(in_subdivision=True)
            if event:
                events.append(event)
        if not self.match("RBRACKET"):
            self.diagnostics.error("PAT007", "unterminated subdivision; expected ']'", start.pos, self.context)
        if not events:
            self.diagnostics.error("PAT008", "subdivision may not be empty", start.pos, self.context)
        return PatternSlot(events, True, start.pos)

    def parse_event(self, in_subdivision: bool) -> Optional[PatternEvent]:
        token = self.peek()
        if token.kind == "NOTE":
            self.advance()
            duration_ticks = self.parse_event_duration()
            return PatternEvent(
                "note", token.text, token.pos, duration_ticks,
                self.parse_modifiers(self.tokens[self.index - 1].end_offset),
            )
        if token.kind == "MINUS":
            self.advance()
            pitch = self.peek()
            if pitch.kind != "NOTE":
                self.diagnostics.error("PAT013", "'-' in a pattern must be followed by a pitch", pitch.pos, self.context)
                return None
            self.advance()
            duration_ticks = self.parse_event_duration()
            return PatternEvent(
                "legato", pitch.text, token.pos, duration_ticks,
                self.parse_modifiers(self.tokens[self.index - 1].end_offset),
            )
        if token.kind == "REST":
            self.advance()
            return PatternEvent("rest", "~", token.pos)
        if token.kind == "IDENT" and token.text == "x":
            self.advance()
            return PatternEvent("hit", "x", token.pos)
        if token.kind == "IDENT" and token.text == "_":
            self.advance()
            return PatternEvent("continuation", "_", token.pos)
        if token.kind in {"CARET", "LT", "GT", "EQUAL"}:
            return PatternEvent("modifier", "", token.pos, modifiers=self.parse_modifiers())
        if token.kind == "IDENT" and token.text == "r":
            self.diagnostics.error("PAT009", "'r' is not a rest; use '~'", token.pos, self.context)
            self.advance()
            return PatternEvent("invalid", token.text, token.pos)
        if token.kind == "LBRACKET" and in_subdivision:
            self.diagnostics.error("PAT010", "nested subdivisions are not supported", token.pos, self.context)
        else:
            self.diagnostics.error("PAT011", f"invalid bar event {token.text!r}", token.pos, self.context)
        self.advance()
        return None

    def parse_event_duration(self) -> Optional[int]:
        if not self.match("AT"):
            return None
        duration = self.peek()
        if duration.kind != "INT" or int(duration.text) <= 0:
            self.diagnostics.error("PAT014", "note duration must be a positive tick count", duration.pos, self.context)
            return None
        self.advance()
        return int(duration.text)

    def parse_modifiers(self, adjacent_to: Optional[int] = None) -> list[PatternModifier]:
        modifiers: list[PatternModifier] = []
        seen: set[str] = set()
        while (
            self.peek().kind in {"CARET", "LT", "GT", "EQUAL"}
            and (adjacent_to is None or self.peek().pos.offset == adjacent_to)
        ):
            token = self.advance()
            if token.kind == "CARET":
                if self.peek().kind != "INT" or self.peek().pos.offset != token.end_offset:
                    modifier = PatternModifier("arpeggio", None, token.pos)
                else:
                    first_token = self.advance()
                    first = int(first_token.text)
                    second = first
                    if (
                        self.peek().kind == "CARET"
                        and self.peek().pos.offset == first_token.end_offset
                        and self.peek_ahead(1).kind == "INT"
                        and self.peek_ahead(1).pos.offset == self.peek().end_offset
                    ):
                        self.advance()
                        second = int(self.advance().text)
                    if first > 15 or second > 15:
                        self.diagnostics.error(
                            "PAT015", "arpeggio offsets must be in 0..15 semitones", token.pos, self.context
                        )
                    modifier = PatternModifier("arpeggio", [first, second], token.pos)
            elif token.kind in {"LT", "GT"}:
                rate = self.peek()
                if (
                    rate.kind != "INT"
                    or rate.pos.offset != token.end_offset
                    or not 1 <= int(rate.text) <= 15
                ):
                    self.diagnostics.error(
                        "PAT016", "hairpin rate must be an integer in 1..15", rate.pos, self.context
                    )
                    modifier = PatternModifier("hairpin", 0, token.pos)
                else:
                    self.advance()
                    amount = int(rate.text)
                    modifier = PatternModifier("hairpin", amount if token.kind == "LT" else -amount, token.pos)
            else:
                modifier = PatternModifier("hairpin", 0, token.pos)

            adjacent_to = self.tokens[self.index - 1].end_offset

            if modifier.kind in seen:
                self.diagnostics.error(
                    "PAT017", f"an event may contain only one {modifier.kind} modifier", modifier.pos, self.context
                )
            else:
                seen.add(modifier.kind)
                modifiers.append(modifier)
        return modifiers

    def parse_qualified_name(self, initial: str) -> str:
        parts = [initial]
        while self.match("DOT"):
            token = self.peek()
            if token.kind not in {"IDENT", "NOTE"}:
                self.diagnostics.error("PAT012", "expected identifier after '.'", token.pos, self.context)
                break
            self.advance()
            parts.extend([".", token.text])
        return "".join(parts)


# ---------------------------------------------------------------------------
# Semantic model and analyzer
# ---------------------------------------------------------------------------


@dataclass
class Symbol:
    kind: str
    name: str
    pos: SourcePos
    node: Any
    owner: Optional[str] = None

    @property
    def symbol_id(self) -> str:
        if self.owner:
            return f"{self.kind}:{self.owner}/{self.name}"
        return f"{self.kind}:{self.name}"

    def ref_json(self) -> dict[str, str]:
        return {"kind": self.kind, "name": self.name, "symbol_id": self.symbol_id}


@dataclass
class Relationship:
    source_symbol: str
    property_name: str
    target_kind: str
    target_name: str
    pos: SourcePos
    target_symbol: Optional[str] = None
    status: str = "unresolved"

    def to_json(self) -> dict[str, Any]:
        return {
            "source_symbol": self.source_symbol,
            "property": self.property_name,
            "target_kind": self.target_kind,
            "target_name": self.target_name,
            "target_symbol": self.target_symbol,
            "status": self.status,
            "location": self.pos.to_json(),
        }


TRANSPARENT_CONTAINERS = {"patterns", "sections", "instruments", "scenes"}
SYMBOL_TYPES = {"pattern", "rhythm", "section", "instrument", "orchestra", "layer", "group", "scene", "form"}


class Analyzer:
    def __init__(self, document: Document, source_path: Path, source: str, diagnostics: Diagnostics) -> None:
        self.document = document
        self.source_path = source_path
        self.source = source
        self.diagnostics = diagnostics
        self.song: Optional[Declaration] = None
        self.targets: list[Declaration] = []
        self.symbols: dict[str, dict[str, Symbol]] = {kind: {} for kind in SYMBOL_TYPES}
        self.symbols["role"] = {}
        self.symbols["target"] = {}
        self.relationships: list[Relationship] = []
        self.song_assignments: dict[str, Assignment] = {}
        self.role_slots: dict[str, int] = {}
        self.section_bars: dict[str, int] = {}
        self.pattern_bar_cache: dict[tuple[str, str], Optional[list[int]]] = {}
        self.target_models: dict[str, dict[str, Any]] = {}
        self.required_checks: dict[str, bool] = {}

    def analyze(self) -> dict[str, Any]:
        self.find_roots()
        if self.song:
            self.collect_song()
            self.parse_all_expressions()
            self.validate_song_properties()
            self.resolve_patterns_and_sections()
            self.validate_objects()
            self.validate_forms()
            self.validate_groups_and_scenes()
        self.collect_targets()
        if self.song:
            self.validate_targets()
        self.validate_required_object_types()
        return self.build_ir()

    # ---- collection -----------------------------------------------------

    def find_roots(self) -> None:
        songs = [item for item in self.document.items if item.decl_type == "song" and item.form == "block"]
        self.targets = [item for item in self.document.items if item.decl_type == "target" and item.form == "block"]
        for item in self.document.items:
            if item.decl_type not in {"song", "target"}:
                self.diagnostics.error("SEM001", f"unsupported top-level declaration '{item.decl_type}'", item.pos)
        if not songs:
            pos = self.document.items[0].pos if self.document.items else SourcePos(1, 1, 0)
            self.diagnostics.error("SEM002", "document must contain exactly one song block", pos)
        elif len(songs) > 1:
            self.diagnostics.error("SEM003", "document may contain only one song block", songs[1].pos)
            self.song = songs[0]
        else:
            self.song = songs[0]
        for target in self.targets:
            if not target.name:
                self.diagnostics.error("SEM004", "target block requires a name", target.pos)
                continue
            self.add_symbol("target", target.name, target.pos, target)

    def collect_song(self) -> None:
        assert self.song is not None
        if not self.song.name:
            self.diagnostics.error("SEM005", "song block requires a name", self.song.pos)

        for item in self.song.items:
            if isinstance(item, Assignment):
                if item.name in self.song_assignments:
                    self.diagnostics.error("SEM006", f"duplicate song property '{item.name}'", item.pos)
                else:
                    self.song_assignments[item.name] = item
            else:
                self.collect_song_declaration(item, orchestra_owner=None)

    def collect_song_declaration(self, decl: Declaration, orchestra_owner: Optional[str]) -> None:
        if decl.decl_type in TRANSPARENT_CONTAINERS and decl.name is None:
            for item in decl.items:
                if isinstance(item, Assignment):
                    self.diagnostics.error("SEM007", f"assignments are not allowed directly inside '{decl.decl_type}'", item.pos)
                else:
                    self.collect_song_declaration(item, orchestra_owner)
            return

        if decl.decl_type not in SYMBOL_TYPES:
            self.diagnostics.error("SEM062", f"unsupported song declaration '{decl.decl_type}'", decl.pos)
            return

        if decl.decl_type in {"pattern", "rhythm"} and decl.form != "value":
            self.diagnostics.error("SEM063", f"{decl.decl_type} '{decl.name or ''}' must use '= expression;' form", decl.pos)
        if decl.decl_type not in {"pattern", "rhythm"} and decl.form != "block":
            self.diagnostics.error("SEM064", f"{decl.decl_type} '{decl.name or ''}' must use block form", decl.pos)

        if decl.decl_type in {"layer", "group"}:
            if orchestra_owner is None:
                self.diagnostics.error("SEM065", f"{decl.decl_type} '{decl.name or ''}' must be declared inside an orchestra", decl.pos)
        elif orchestra_owner is not None:
            self.diagnostics.error(
                "SEM066",
                f"declaration '{decl.decl_type} {decl.name or ''}' is not allowed inside orchestra '{orchestra_owner}'",
                decl.pos,
            )

        if not decl.name:
            self.diagnostics.error("SEM008", f"{decl.decl_type} declaration requires a name", decl.pos)
        else:
            owner = orchestra_owner if decl.decl_type in {"layer", "group"} else None
            self.add_symbol(decl.decl_type, decl.name, decl.pos, decl, owner=owner)

        next_owner = orchestra_owner
        if decl.decl_type == "orchestra" and decl.name:
            next_owner = decl.name

        for item in decl.items:
            if isinstance(item, Declaration):
                self.collect_song_declaration(item, next_owner)

    def add_symbol(self, kind: str, name: str, pos: SourcePos, node: Any, owner: str | None = None) -> Optional[Symbol]:
        namespace = self.symbols.setdefault(kind, {})
        if name in namespace:
            self.diagnostics.error("SEM009", f"duplicate {kind} declaration '{name}'", pos)
            return None
        symbol = Symbol(kind, name, pos, node, owner)
        namespace[name] = symbol
        return symbol

    # ---- expression parsing --------------------------------------------

    def parse_all_expressions(self) -> None:
        assert self.song is not None
        for assignment in self.walk_assignments(self.song):
            if self.is_pattern_assignment(assignment):
                assignment.pattern = PatternParser(assignment.expr_tokens, self.diagnostics, f"assignment {assignment.name}").parse()
            else:
                assignment.parsed = ExpressionParser(assignment.expr_tokens, self.diagnostics, f"assignment {assignment.name}").parse()

        for kind in ("pattern", "rhythm"):
            for symbol in self.symbols[kind].values():
                decl: Declaration = symbol.node
                decl.pattern = PatternParser(decl.expr_tokens, self.diagnostics, f"{kind} {symbol.name}").parse()

        for target in self.targets:
            for assignment in self.walk_assignments(target):
                assignment.parsed = ExpressionParser(assignment.expr_tokens, self.diagnostics, f"target assignment {assignment.name}").parse()

    def walk_assignments(self, declaration: Declaration) -> Iterator[Assignment]:
        for item in declaration.items:
            if isinstance(item, Assignment):
                yield item
            else:
                yield from self.walk_assignments(item)

    def is_pattern_assignment(self, assignment: Assignment) -> bool:
        parent = self.find_assignment_parent(self.song, assignment) if self.song else None
        return bool(parent and parent.decl_type == "section")

    def find_assignment_parent(self, root: Optional[Declaration], target: Assignment) -> Optional[Declaration]:
        if root is None:
            return None
        for item in root.items:
            if item is target:
                return root
            if isinstance(item, Declaration):
                found = self.find_assignment_parent(item, target)
                if found:
                    return found
        return None

    # ---- song and role validation --------------------------------------

    def validate_song_properties(self) -> None:
        required = ["tick_rate", "ticks_per_row", "rows_per_quarter", "tuning", "meter", "roles", "play"]
        for name in required:
            if name not in self.song_assignments:
                self.diagnostics.error("SEM010", f"song is missing required property '{name}'", self.song.pos if self.song else SourcePos(1, 1, 0))

        for numeric in ("tick_rate", "ticks_per_row", "rows_per_quarter", "tuning"):
            assignment = self.song_assignments.get(numeric)
            if assignment:
                self.require_positive_integer(assignment.parsed, numeric, assignment.pos)

        meter = self.song_assignments.get("meter")
        if meter:
            self.validate_call_signature(meter.parsed, "meter", 2, meter.pos, positive_integer_args=True)

        roles = self.song_assignments.get("roles")
        if roles:
            self.parse_roles(roles)

        play = self.song_assignments.get("play")
        if play:
            ref = self.require_reference(play.parsed, "play", play.pos)
            if ref:
                self.resolve_reference("song", self.song.name or "<song>", "play", "form", ref.name or "", ref.pos, ref)

    def parse_roles(self, assignment: Assignment) -> None:
        expr = assignment.parsed
        if not expr or expr.kind != "array":
            self.diagnostics.error("SEM011", "roles must be an array of role(name, slots) calls", assignment.pos)
            return
        for item in expr.items:
            if item.kind != "call" or item.name != "role" or len(item.args) != 2:
                self.diagnostics.error("SEM012", "each roles entry must be role(name, slots)", item.pos)
                continue
            name_expr, slots_expr = item.args
            if name_expr.kind != "reference" or not name_expr.name:
                self.diagnostics.error("SEM013", "role name must be an identifier", name_expr.pos)
                continue
            if slots_expr.kind != "integer" or slots_expr.value <= 0:
                self.diagnostics.error("SEM014", "role slot count must be a positive integer", slots_expr.pos)
                continue
            symbol = self.add_symbol("role", name_expr.name, name_expr.pos, item)
            if symbol:
                self.role_slots[name_expr.name] = slots_expr.value
                name_expr.resolved = symbol.ref_json()

    # ---- patterns, rhythms, sections -----------------------------------

    def resolve_patterns_and_sections(self) -> None:
        for kind in ("pattern", "rhythm"):
            namespace = self.symbols[kind]
            target_kind = kind
            for symbol in namespace.values():
                seq: Optional[PatternSequence] = symbol.node.pattern
                if not seq:
                    continue
                for term in seq.terms:
                    if term.kind == "reference" and term.reference:
                        target = self.resolve_reference(kind, symbol.name, "pattern", target_kind, term.reference, term.pos)
                        if target:
                            term.resolved = target.ref_json()
            self.detect_dependency_cycles(kind)

        for symbol in self.symbols["rhythm"].values():
            role = self.symbols["role"].get(symbol.name)
            if not role:
                self.diagnostics.error("SEM015", f"rhythm '{symbol.name}' has no matching role", symbol.pos)
            else:
                self.add_resolved_relationship(symbol, "role", role, symbol.pos)
                bars = self.expand_pattern(symbol.name, "rhythm", stack=[])
                if bars is not None:
                    expected = self.role_slots.get(symbol.name)
                    if expected is not None:
                        self.validate_slot_counts(bars, expected, symbol.pos, f"rhythm {symbol.name}")

        for section_symbol in self.symbols["section"].values():
            section: Declaration = section_symbol.node
            seen_roles: set[str] = set()
            stream_lengths: list[tuple[str, int, SourcePos]] = []
            for item in section.items:
                if isinstance(item, Declaration):
                    self.diagnostics.error("SEM016", "nested declarations are not allowed inside a section", item.pos)
                    continue
                role_name = item.name
                if role_name in seen_roles:
                    self.diagnostics.error("SEM017", f"duplicate role stream '{role_name}' in section '{section_symbol.name}'", item.pos)
                seen_roles.add(role_name)
                role_symbol = self.symbols["role"].get(role_name)
                if not role_symbol:
                    self.diagnostics.error("SEM018", f"section '{section_symbol.name}' references undeclared role '{role_name}'", item.pos)
                    continue
                self.add_resolved_relationship(section_symbol, role_name, role_symbol, item.pos)
                seq = item.pattern
                if not seq:
                    continue
                for term in seq.terms:
                    if term.kind == "reference" and term.reference:
                        target = self.resolve_reference("section", section_symbol.name, role_name, "pattern", term.reference, term.pos)
                        if target:
                            term.resolved = target.ref_json()
                bars = self.expand_sequence(seq, "pattern", stack=[])
                if bars is None:
                    continue
                expected = self.role_slots.get(role_name)
                if expected is not None:
                    self.validate_slot_counts(bars, expected, item.pos, f"section {section_symbol.name}.{role_name}")
                stream_lengths.append((role_name, len(bars), item.pos))
            if stream_lengths:
                expected_bars = stream_lengths[0][1]
                self.section_bars[section_symbol.name] = expected_bars
                for role_name, count, pos in stream_lengths[1:]:
                    if count != expected_bars:
                        self.diagnostics.error(
                            "SEM019",
                            f"section '{section_symbol.name}' role '{role_name}' has {count} bars; expected {expected_bars}",
                            pos,
                        )

    def detect_dependency_cycles(self, kind: str) -> None:
        namespace = self.symbols[kind]
        graph: dict[str, list[str]] = {}
        for name, symbol in namespace.items():
            seq: Optional[PatternSequence] = symbol.node.pattern
            graph[name] = [dep for dep in (seq.dependencies() if seq else []) if dep in namespace]
        self.detect_cycles(graph, kind)

    def expand_pattern(self, name: str, kind: str, stack: list[str]) -> Optional[list[int]]:
        key = (kind, name)
        if key in self.pattern_bar_cache:
            return self.pattern_bar_cache[key]
        if name in stack:
            return None
        symbol = self.symbols[kind].get(name)
        if not symbol or not symbol.node.pattern:
            return None
        bars = self.expand_sequence(symbol.node.pattern, kind, stack + [name])
        self.pattern_bar_cache[key] = bars
        return bars

    def expand_sequence(self, seq: PatternSequence, ref_kind: str, stack: list[str]) -> Optional[list[int]]:
        result: list[int] = []
        valid = True
        for term in seq.terms:
            if term.kind == "bar":
                result.append(len(term.slots))
            elif term.reference:
                bars = self.expand_pattern(term.reference, ref_kind, stack)
                if bars is None:
                    valid = False
                    continue
                for _ in range(term.repeat):
                    result.extend(bars)
        return result if valid else None

    def validate_slot_counts(self, bars: list[int], expected: int, pos: SourcePos, context: str) -> None:
        for index, actual in enumerate(bars, 1):
            if actual != expected:
                self.diagnostics.error(
                    "SEM020",
                    f"{context} bar {index} has {actual} top-level slots; role requires {expected}",
                    pos,
                )

    # ---- object validation ---------------------------------------------

    def validate_objects(self) -> None:
        for instrument_symbol in self.symbols["instrument"].values():
            decl: Declaration = instrument_symbol.node
            props = self.assignment_map(decl, allow_duplicates=False)
            for required in ("priority", "level", "envelope"):
                if required not in props:
                    self.diagnostics.warning("SEM021", f"instrument '{instrument_symbol.name}' has no '{required}' property", decl.pos)

        for orchestra_symbol in self.symbols["orchestra"].values():
            decl: Declaration = orchestra_symbol.node
            child_orchestra_layers = [
                item for item in decl.items if isinstance(item, Declaration) and item.decl_type == "layer"
            ]
            if not child_orchestra_layers:
                self.diagnostics.error("SEM022", f"orchestra '{orchestra_symbol.name}' declares no layers", decl.pos)

        for layer_symbol in self.symbols["layer"].values():
            decl: Declaration = layer_symbol.node
            props = self.assignment_map(decl, allow_duplicates=False)
            source = props.get("source")
            instrument = props.get("instrument")
            if not source:
                self.diagnostics.error("SEM023", f"layer '{layer_symbol.name}' is missing source", decl.pos)
            else:
                ref = self.require_reference(source.parsed, "layer source", source.pos)
                if ref and ref.name:
                    self.resolve_reference("layer", layer_symbol.name, "source", "role", ref.name, ref.pos, ref)
            if not instrument:
                self.diagnostics.error("SEM024", f"layer '{layer_symbol.name}' is missing instrument", decl.pos)
            else:
                ref = self.require_reference(instrument.parsed, "layer instrument", instrument.pos)
                if ref and ref.name:
                    self.resolve_reference("layer", layer_symbol.name, "instrument", "instrument", ref.name, ref.pos, ref)
            if "route" not in props and "distribute" not in props:
                self.diagnostics.error("SEM025", f"layer '{layer_symbol.name}' requires route or distribute", decl.pos)

        for group_symbol in self.symbols["group"].values():
            decl: Declaration = group_symbol.node
            props = self.assignment_map(decl, allow_duplicates=False)
            members = props.get("members")
            if not members:
                self.diagnostics.error("SEM026", f"group '{group_symbol.name}' is missing members", decl.pos)
                continue
            array = self.require_array(members.parsed, "group members", members.pos)
            if not array:
                continue
            for item in array.items:
                ref = self.require_reference(item, "group member", item.pos)
                if not ref or not ref.name:
                    continue
                target = self.symbols["layer"].get(ref.name) or self.symbols["group"].get(ref.name)
                if not target:
                    self.diagnostics.error("SEM027", f"group '{group_symbol.name}' references undeclared layer or group '{ref.name}'", ref.pos)
                    self.add_unresolved_relationship(group_symbol, "members", "layer_or_group", ref.name, ref.pos)
                else:
                    ref.resolved = target.ref_json()
                    self.add_resolved_relationship(group_symbol, "members", target, ref.pos)

    def validate_groups_and_scenes(self) -> None:
        graph: dict[str, list[str]] = {}
        for name, symbol in self.symbols["group"].items():
            graph[name] = []
            decl: Declaration = symbol.node
            props = self.assignment_map(decl, allow_duplicates=True)
            members = props.get("members")
            if members and members.parsed and members.parsed.kind == "array":
                for item in members.parsed.items:
                    if item.kind == "reference" and item.name in self.symbols["group"]:
                        graph[name].append(item.name)
        self.detect_cycles(graph, "group")

        for scene_symbol in self.symbols["scene"].values():
            decl: Declaration = scene_symbol.node
            props = self.assignment_map(decl, allow_duplicates=False)
            enables = props.get("enables")
            if not enables:
                self.diagnostics.error("SEM028", f"scene '{scene_symbol.name}' is missing enables", decl.pos)
            else:
                array = self.require_array(enables.parsed, "scene enables", enables.pos)
                if array:
                    for item in array.items:
                        ref = self.require_reference(item, "scene enabled object", item.pos)
                        if not ref or not ref.name:
                            continue
                        target = self.symbols["layer"].get(ref.name) or self.symbols["group"].get(ref.name)
                        if not target:
                            self.diagnostics.error("SEM029", f"scene '{scene_symbol.name}' references undeclared layer or group '{ref.name}'", ref.pos)
                            self.add_unresolved_relationship(scene_symbol, "enables", "layer_or_group", ref.name, ref.pos)
                        else:
                            ref.resolved = target.ref_json()
                            self.add_resolved_relationship(scene_symbol, "enables", target, ref.pos)
            transforms = props.get("transforms")
            if transforms:
                array = self.require_array(transforms.parsed, "scene transforms", transforms.pos)
                if array:
                    for item in array.items:
                        if item.kind != "call" or item.name != "transpose" or len(item.args) != 2:
                            self.diagnostics.error("SEM030", "scene transforms currently support transpose(target, semitones)", item.pos)
                            continue
                        target_expr, amount_expr = item.args
                        ref = self.require_reference(target_expr, "transpose target", target_expr.pos)
                        if amount_expr.kind != "integer":
                            self.diagnostics.error("SEM031", "transpose amount must be an integer", amount_expr.pos)
                        if ref and ref.name:
                            target = self.symbols["role"].get(ref.name) or self.symbols["layer"].get(ref.name)
                            if not target:
                                self.diagnostics.error("SEM032", f"transpose target '{ref.name}' is not a declared role or layer", ref.pos)
                                self.add_unresolved_relationship(scene_symbol, "transforms", "role_or_layer", ref.name, ref.pos)
                            else:
                                ref.resolved = target.ref_json()
                                self.add_resolved_relationship(scene_symbol, "transforms", target, ref.pos)

    # ---- forms ----------------------------------------------------------

    def validate_forms(self) -> None:
        for form_symbol in self.symbols["form"].values():
            decl: Declaration = form_symbol.node
            props = self.assignment_map(decl, allow_duplicates=False)
            using = props.get("using")
            sequence = props.get("sequence")
            orchestra_name: Optional[str] = None
            if not using:
                self.diagnostics.error("SEM033", f"form '{form_symbol.name}' is missing using", decl.pos)
            else:
                ref = self.require_reference(using.parsed, "form orchestra", using.pos)
                if ref and ref.name:
                    target = self.resolve_reference("form", form_symbol.name, "using", "orchestra", ref.name, ref.pos, ref)
                    if target:
                        orchestra_name = target.name
            if not sequence:
                self.diagnostics.error("SEM034", f"form '{form_symbol.name}' is missing sequence", decl.pos)
                continue
            array = self.require_array(sequence.parsed, "form sequence", sequence.pos)
            if not array:
                continue
            total_bars = 0
            for item in array.items:
                if item.kind != "call" or item.name != "play" or len(item.args) != 2:
                    self.diagnostics.error("SEM035", "form sequence entries must be play(section, scene)", item.pos)
                    continue
                section_expr, scene_expr = item.args
                section_ref = self.require_reference(section_expr, "play section", section_expr.pos)
                scene_ref = self.require_reference(scene_expr, "play scene", scene_expr.pos)
                if section_ref and section_ref.name:
                    target = self.resolve_reference("form", form_symbol.name, "sequence.section", "section", section_ref.name, section_ref.pos, section_ref)
                    if target:
                        total_bars += self.section_bars.get(target.name, 0)
                if scene_ref and scene_ref.name:
                    scene_target = self.resolve_reference(
                        "form", form_symbol.name, "sequence.scene", "scene", scene_ref.name, scene_ref.pos, scene_ref
                    )
                    if scene_target and orchestra_name:
                        for layer_name in sorted(self.layers_for_scene(scene_target.name)):
                            layer = self.symbols["layer"].get(layer_name)
                            if layer and layer.owner != orchestra_name:
                                self.diagnostics.error(
                                    "SEM067",
                                    f"scene '{scene_target.name}' enables layer '{layer_name}' from orchestra "
                                    f"'{layer.owner}', but form '{form_symbol.name}' uses orchestra '{orchestra_name}'",
                                    scene_ref.pos,
                                )
            setattr(decl, "computed_total_bars", total_bars)

    # ---- targets --------------------------------------------------------

    def collect_targets(self) -> None:
        for target in self.targets:
            if not target.name:
                continue
            model: dict[str, Any] = {
                "declaration": target,
                "properties": {},
                "routes": {},
                "realize": {},
                "bindings": {},
                "limits": {},
                "extensions": {},
                "resolved_layer_routes": {},
                "required_layers": [],
                "used_instruments": [],
            }
            for item in target.items:
                if isinstance(item, Assignment):
                    if item.name in model["properties"]:
                        self.diagnostics.error("SEM036", f"duplicate target property '{item.name}'", item.pos)
                    model["properties"][item.name] = item
                elif item.decl_type == "routes" and item.form == "block":
                    for assignment in item.items:
                        if isinstance(assignment, Assignment):
                            if assignment.name in model["routes"]:
                                self.diagnostics.error("SEM037", f"duplicate route '{assignment.name}'", assignment.pos)
                            model["routes"][assignment.name] = assignment
                elif item.decl_type == "realize" and item.form == "block":
                    if not item.name:
                        self.diagnostics.error("SEM038", "realize block requires an instrument name", item.pos)
                    elif item.name in model["realize"]:
                        self.diagnostics.error("SEM039", f"duplicate realization for '{item.name}'", item.pos)
                    else:
                        model["realize"][item.name] = item
                elif item.decl_type == "bind" and item.form == "block":
                    for assignment in item.items:
                        if isinstance(assignment, Assignment):
                            if assignment.name in model["bindings"]:
                                self.diagnostics.error("SEM040", f"duplicate binding for layer '{assignment.name}'", assignment.pos)
                            model["bindings"][assignment.name] = assignment
                elif item.decl_type == "limits" and item.form == "block":
                    for assignment in item.items:
                        if isinstance(assignment, Assignment):
                            if assignment.name in model["limits"]:
                                self.diagnostics.error("SEM041", f"duplicate limit '{assignment.name}'", assignment.pos)
                            model["limits"][assignment.name] = assignment
                elif item.decl_type == "extension" and item.form == "block":
                    if not item.name:
                        self.diagnostics.error("SEM068", "extension block requires a backend name", item.pos)
                    elif item.name in model["extensions"]:
                        self.diagnostics.error("SEM069", f"duplicate extension block '{item.name}'", item.pos)
                    else:
                        model["extensions"][item.name] = item
                    extension_properties: set[str] = set()
                    for nested in item.items:
                        if isinstance(nested, Declaration):
                            self.diagnostics.error("SEM073", "target extension may contain assignments only", nested.pos)
                        elif nested.name in extension_properties:
                            self.diagnostics.error(
                                "SEM074", f"duplicate extension property '{nested.name}'", nested.pos
                            )
                        else:
                            extension_properties.add(nested.name)
                else:
                    self.diagnostics.error("SEM042", f"unsupported target declaration '{item.decl_type}'", item.pos)
            self.target_models[target.name] = model

    def validate_targets(self) -> None:
        for target_name, model in self.target_models.items():
            target_symbol = self.symbols["target"][target_name]
            props: dict[str, Assignment] = model["properties"]
            for required in ("backend", "clock"):
                if required not in props:
                    self.diagnostics.error("SEM043", f"target '{target_name}' is missing required property '{required}'", target_symbol.pos)
            for numeric in ("clock",):
                assignment = props.get(numeric)
                if assignment:
                    self.require_positive_integer(assignment.parsed, f"target {numeric}", assignment.pos)
            if not model["routes"]:
                self.diagnostics.error("SEM044", f"target '{target_name}' declares no routes", target_symbol.pos)
            route_names = set(model["routes"])
            for layer_symbol in self.symbols["layer"].values():
                decl: Declaration = layer_symbol.node
                props_map = self.assignment_map(decl, allow_duplicates=True)
                route = props_map.get("route")
                if route:
                    ref = self.require_reference(route.parsed, "layer route", route.pos)
                    if ref and ref.name:
                        self.resolve_target_route(target_symbol, layer_symbol, "route", ref, route_names)
                distribute = props_map.get("distribute")
                if distribute and distribute.parsed:
                    expr = distribute.parsed
                    if expr.kind != "call" or expr.name != "roundrobin" or len(expr.args) != 1:
                        self.diagnostics.error("SEM046", "distribute must be roundrobin([route, ...])", distribute.pos)
                    else:
                        array = self.require_array(expr.args[0], "roundrobin routes", expr.args[0].pos)
                        if array:
                            for item in array.items:
                                ref = self.require_reference(item, "roundrobin route", item.pos)
                                if ref:
                                    self.resolve_target_route(target_symbol, layer_symbol, "distribute", ref, route_names)

            for instrument_name, realize in model["realize"].items():
                instrument = self.symbols["instrument"].get(instrument_name)
                if not instrument:
                    self.diagnostics.error("SEM047", f"target '{target_name}' realizes undeclared instrument '{instrument_name}'", realize.pos)
                    self.add_unresolved_relationship(target_symbol, "realize", "instrument", instrument_name, realize.pos)
                else:
                    self.add_resolved_relationship(target_symbol, "realize", instrument, realize.pos)
                extension_names: set[str] = set()
                for child in realize.items:
                    if isinstance(child, Assignment):
                        continue
                    if child.decl_type != "extension" or child.form != "block" or not child.name:
                        self.diagnostics.error(
                            "SEM070",
                            f"realize '{instrument_name}' only permits named extension blocks",
                            child.pos,
                        )
                        continue
                    if child.name in extension_names:
                        self.diagnostics.error(
                            "SEM071",
                            f"duplicate extension block '{child.name}' in realize '{instrument_name}'",
                            child.pos,
                        )
                    extension_names.add(child.name)
                    extension_properties: set[str] = set()
                    for nested in child.items:
                        if isinstance(nested, Declaration):
                            self.diagnostics.error(
                                "SEM072",
                                f"extension '{child.name}' may contain assignments only",
                                nested.pos,
                            )
                        elif nested.name in extension_properties:
                            self.diagnostics.error(
                                "SEM075",
                                f"duplicate extension property '{nested.name}' in realize '{instrument_name}'",
                                nested.pos,
                            )
                        else:
                            extension_properties.add(nested.name)

            endpoints: dict[str, str] = {}
            for layer_name, assignment in model["bindings"].items():
                base_layer = re.sub(r"\[[A-Za-z_][A-Za-z0-9_]*\]$", "", layer_name)
                layer = self.symbols["layer"].get(base_layer)
                if not layer:
                    self.diagnostics.error("SEM048", f"target '{target_name}' binds undeclared layer '{base_layer}'", assignment.pos)
                    self.add_unresolved_relationship(target_symbol, "bind", "layer", base_layer, assignment.pos)
                else:
                    self.add_resolved_relationship(target_symbol, "bind", layer, assignment.pos)
                rhs = assignment.parsed
                if rhs and rhs.kind == "reference" and rhs.name:
                    if rhs.name in endpoints:
                        self.diagnostics.warning(
                            "SEM049",
                            f"hardware endpoint '{rhs.name}' is bound by both '{endpoints[rhs.name]}' and '{layer_name}'",
                            rhs.pos,
                        )
                    endpoints[rhs.name] = layer_name

            required_layers = self.layers_used_by_forms()
            model["required_layers"] = sorted(required_layers)

            used_instruments: set[str] = set()
            for layer_name in required_layers:
                layer = self.symbols["layer"].get(layer_name)
                if not layer:
                    continue
                props_map = self.assignment_map(layer.node, allow_duplicates=True)
                inst = props_map.get("instrument")
                if inst and inst.parsed and inst.parsed.kind == "reference" and inst.parsed.name:
                    used_instruments.add(inst.parsed.name)
            model["used_instruments"] = sorted(used_instruments)
            for instrument_name in sorted(used_instruments):
                if instrument_name not in model["realize"]:
                    self.diagnostics.error("SEM051", f"target '{target_name}' has no realization for instrument '{instrument_name}'", target_symbol.pos)

            for name, assignment in model["limits"].items():
                self.require_positive_integer(assignment.parsed, f"limit {name}", assignment.pos)

    def resolve_target_route(
        self,
        target_symbol: Symbol,
        layer_symbol: Symbol,
        property_name: str,
        ref: Expr,
        route_names: set[str],
    ) -> None:
        assert ref.name is not None
        synthetic_name = f"{target_symbol.name}.{ref.name}"
        model = self.target_models[target_symbol.name]
        layer_routes = model["resolved_layer_routes"].setdefault(layer_symbol.name, {})
        route_list = layer_routes.setdefault(property_name, [])
        if ref.name not in route_names:
            self.diagnostics.error(
                "SEM052",
                f"layer '{layer_symbol.name}' references route '{ref.name}' not declared by target '{target_symbol.name}'",
                ref.pos,
            )
            route_list.append({"name": ref.name, "status": "unresolved", "symbol_id": None})
            self.add_unresolved_relationship(layer_symbol, property_name, "route", synthetic_name, ref.pos)
        else:
            route_id = f"route:{target_symbol.name}/{ref.name}"
            route_list.append({"name": ref.name, "status": "resolved", "symbol_id": route_id})
            self.relationships.append(
                Relationship(layer_symbol.symbol_id, property_name, "route", ref.name, ref.pos, route_id, "resolved")
            )

    # ---- graph/use helpers ---------------------------------------------

    def layers_for_scene(self, scene_name: str) -> set[str]:
        scene = self.symbols["scene"].get(scene_name)
        if not scene:
            return set()
        props = self.assignment_map(scene.node, allow_duplicates=True)
        enables = props.get("enables")
        if not enables or not enables.parsed or enables.parsed.kind != "array":
            return set()
        result: set[str] = set()
        for item in enables.parsed.items:
            if item.kind != "reference" or not item.name:
                continue
            if item.name in self.symbols["layer"]:
                result.add(item.name)
            elif item.name in self.symbols["group"]:
                result.update(self.expand_group(item.name, []))
        return result

    def layers_used_by_forms(self) -> set[str]:
        scene_names: set[str] = set()
        for form_symbol in self.symbols["form"].values():
            props = self.assignment_map(form_symbol.node, allow_duplicates=True)
            sequence = props.get("sequence")
            if not sequence or not sequence.parsed or sequence.parsed.kind != "array":
                continue
            for item in sequence.parsed.items:
                if item.kind == "call" and item.name == "play" and len(item.args) == 2:
                    scene = item.args[1]
                    if scene.kind == "reference" and scene.name:
                        scene_names.add(scene.name)
        result: set[str] = set()
        for scene_name in scene_names:
            result.update(self.layers_for_scene(scene_name))
        return result

    def expand_group(self, name: str, stack: list[str]) -> set[str]:
        if name in stack:
            return set()
        symbol = self.symbols["group"].get(name)
        if not symbol:
            return set()
        props = self.assignment_map(symbol.node, allow_duplicates=True)
        members = props.get("members")
        result: set[str] = set()
        if members and members.parsed and members.parsed.kind == "array":
            for item in members.parsed.items:
                if item.kind != "reference" or not item.name:
                    continue
                if item.name in self.symbols["layer"]:
                    result.add(item.name)
                elif item.name in self.symbols["group"]:
                    result.update(self.expand_group(item.name, stack + [name]))
        return result

    def detect_cycles(self, graph: dict[str, list[str]], kind: str) -> None:
        state: dict[str, int] = {name: 0 for name in graph}
        stack: list[str] = []

        def visit(name: str) -> None:
            state[name] = 1
            stack.append(name)
            for nxt in graph.get(name, []):
                if state.get(nxt, 0) == 0:
                    visit(nxt)
                elif state.get(nxt) == 1:
                    try:
                        start = stack.index(nxt)
                    except ValueError:
                        start = 0
                    cycle = stack[start:] + [nxt]
                    symbol = self.symbols[kind].get(name)
                    pos = symbol.pos if symbol else SourcePos(1, 1, 0)
                    self.diagnostics.error("SEM053", f"cyclic {kind} reference: {' -> '.join(cycle)}", pos)
            stack.pop()
            state[name] = 2

        for node in graph:
            if state[node] == 0:
                visit(node)

    # ---- relationship helpers ------------------------------------------

    def resolve_reference(
        self,
        source_kind: str,
        source_name: str,
        property_name: str,
        target_kind: str,
        target_name: str,
        pos: SourcePos,
        expr: Optional[Expr] = None,
    ) -> Optional[Symbol]:
        target = self.symbols.get(target_kind, {}).get(target_name)
        source_symbol = self.symbols.get(source_kind, {}).get(source_name)
        source_id = source_symbol.symbol_id if source_symbol else f"{source_kind}:{source_name}"
        if not target:
            self.diagnostics.error("SEM054", f"undeclared {target_kind} '{target_name}'", pos)
            self.relationships.append(Relationship(source_id, property_name, target_kind, target_name, pos, None, "unresolved"))
            return None
        if expr is not None:
            expr.resolved = target.ref_json()
        self.relationships.append(Relationship(source_id, property_name, target_kind, target_name, pos, target.symbol_id, "resolved"))
        return target

    def add_resolved_relationship(self, source: Symbol, property_name: str, target: Symbol, pos: SourcePos) -> None:
        self.relationships.append(
            Relationship(source.symbol_id, property_name, target.kind, target.name, pos, target.symbol_id, "resolved")
        )

    def add_unresolved_relationship(self, source: Symbol, property_name: str, target_kind: str, target_name: str, pos: SourcePos) -> None:
        self.relationships.append(Relationship(source.symbol_id, property_name, target_kind, target_name, pos, None, "unresolved"))

    # ---- basic type helpers --------------------------------------------

    def assignment_map(self, decl: Declaration, allow_duplicates: bool) -> dict[str, Assignment]:
        result: dict[str, Assignment] = {}
        for item in decl.items:
            if not isinstance(item, Assignment):
                continue
            if item.name in result and not allow_duplicates:
                self.diagnostics.error("SEM055", f"duplicate property '{item.name}' in {decl.decl_type} '{decl.name or ''}'", item.pos)
            else:
                result[item.name] = item
        return result

    def require_reference(self, expr: Optional[Expr], context: str, pos: SourcePos) -> Optional[Expr]:
        if not expr or expr.kind != "reference":
            self.diagnostics.error("SEM056", f"{context} must be an identifier", pos)
            return None
        return expr

    def require_array(self, expr: Optional[Expr], context: str, pos: SourcePos) -> Optional[Expr]:
        if not expr or expr.kind != "array":
            self.diagnostics.error("SEM057", f"{context} must be an array", pos)
            return None
        return expr

    def require_positive_integer(self, expr: Optional[Expr], context: str, pos: SourcePos) -> bool:
        if not expr or expr.kind != "integer" or expr.value <= 0:
            self.diagnostics.error("SEM058", f"{context} must be a positive integer", pos)
            return False
        return True

    def validate_call_signature(
        self,
        expr: Optional[Expr],
        name: str,
        arity: int,
        pos: SourcePos,
        positive_integer_args: bool = False,
    ) -> bool:
        if not expr or expr.kind != "call" or expr.name != name or len(expr.args) != arity:
            self.diagnostics.error("SEM059", f"value must be {name}({', '.join(['integer'] * arity)})", pos)
            return False
        if positive_integer_args:
            for arg in expr.args:
                if arg.kind != "integer" or arg.value <= 0:
                    self.diagnostics.error("SEM060", f"arguments to {name} must be positive integers", arg.pos)
                    return False
        return True

    # ---- required object checks ----------------------------------------

    def validate_required_object_types(self) -> None:
        checks = {
            "exactly_one_song": self.song is not None,
            "has_roles": bool(self.symbols["role"]),
            "has_patterns": bool(self.symbols["pattern"]),
            "has_sections": bool(self.symbols["section"]),
            "has_instruments": bool(self.symbols["instrument"]),
            "has_orchestra": bool(self.symbols["orchestra"]),
            "has_layers": bool(self.symbols["layer"]),
            "has_scenes": bool(self.symbols["scene"]),
            "has_form": bool(self.symbols["form"]),
            "has_target": bool(self.symbols["target"]),
        }
        self.required_checks = checks
        pos = self.song.pos if self.song else SourcePos(1, 1, 0)
        for key, ok in checks.items():
            if not ok:
                label = key.replace("has_", "").replace("_", " ")
                self.diagnostics.error("SEM061", f"required object type is missing: {label}", pos)

    # ---- Compiler-facing model ------------------------------------------

    def build_ir(self) -> dict[str, Any]:
        valid = self.diagnostics.error_count == 0
        song_json = self.song_to_json() if self.song else None
        targets_json = [self.target_to_json(name, model) for name, model in self.target_models.items()]
        symbols_json: dict[str, list[dict[str, Any]]] = {}
        for kind, namespace in self.symbols.items():
            symbols_json[kind] = [
                {
                    "name": symbol.name,
                    "symbol_id": symbol.symbol_id,
                    "owner": symbol.owner,
                    "location": symbol.pos.to_json(),
                }
                for symbol in sorted(namespace.values(), key=lambda s: s.name)
            ]

        return {
            "schema": "csm.semantic.v1",
            "valid": valid,
            "ready_for_backend": valid and all(self.required_checks.values()),
            "source": {
                "path": str(self.source_path),
                "sha256": hashlib.sha256(self.source.encode("utf-8")).hexdigest(),
            },
            "validation": {
                "errors": self.diagnostics.error_count,
                "warnings": self.diagnostics.warning_count,
                "required_object_types": self.required_checks,
                "all_relationships_resolved": all(rel.status == "resolved" for rel in self.relationships),
            },
            "song": song_json,
            "targets": targets_json,
            "symbols": symbols_json,
            "relationships": [rel.to_json() for rel in self.relationships],
            "diagnostics": [item.to_json() for item in self.diagnostics.sorted()],
        }

    def song_to_json(self) -> dict[str, Any]:
        assert self.song is not None
        properties = {
            name: assignment.parsed.to_json() if assignment.parsed else {"kind": "invalid", "raw": assignment.raw}
            for name, assignment in self.song_assignments.items()
        }

        def value_symbols(kind: str) -> dict[str, Any]:
            result: dict[str, Any] = {}
            for name, symbol in sorted(self.symbols[kind].items()):
                decl: Declaration = symbol.node
                bars = self.expand_pattern(name, kind, stack=[])
                result[name] = {
                    "symbol_id": symbol.symbol_id,
                    "expression": decl.pattern.to_json() if decl.pattern else None,
                    "expanded_bar_slot_counts": bars,
                    "dependencies": decl.pattern.dependencies() if decl.pattern else [],
                    "location": symbol.pos.to_json(),
                }
            return result

        sections: dict[str, Any] = {}
        for name, symbol in sorted(self.symbols["section"].items()):
            decl: Declaration = symbol.node
            streams: dict[str, Any] = {}
            for item in decl.items:
                if isinstance(item, Assignment):
                    bars = self.expand_sequence(item.pattern, "pattern", stack=[]) if item.pattern else None
                    streams[item.name] = {
                        "expression": item.pattern.to_json() if item.pattern else None,
                        "expanded_bar_slot_counts": bars,
                    }
            sections[name] = {
                "symbol_id": symbol.symbol_id,
                "tags": [tag.to_json() for tag in decl.tags],
                "bar_count": self.section_bars.get(name),
                "streams": streams,
                "location": symbol.pos.to_json(),
            }

        instruments = {
            name: self.block_symbol_to_json(symbol)
            for name, symbol in sorted(self.symbols["instrument"].items())
        }
        orchestras: dict[str, Any] = {}
        for name, symbol in sorted(self.symbols["orchestra"].items()):
            decl: Declaration = symbol.node
            layers = {
                item.name: self.block_declaration_to_json(item, self.symbols["layer"].get(item.name or ""))
                for item in decl.items
                if isinstance(item, Declaration) and item.decl_type == "layer" and item.name
            }
            groups = {
                item.name: self.block_declaration_to_json(item, self.symbols["group"].get(item.name or ""))
                for item in decl.items
                if isinstance(item, Declaration) and item.decl_type == "group" and item.name
            }
            orchestras[name] = {
                "symbol_id": symbol.symbol_id,
                "layers": layers,
                "groups": groups,
                "location": symbol.pos.to_json(),
            }

        scenes = {name: self.block_symbol_to_json(symbol) for name, symbol in sorted(self.symbols["scene"].items())}
        forms: dict[str, Any] = {}
        for name, symbol in sorted(self.symbols["form"].items()):
            data = self.block_symbol_to_json(symbol)
            data["computed_total_bars"] = getattr(symbol.node, "computed_total_bars", None)
            forms[name] = data

        return {
            "name": self.song.name,
            "location": self.song.pos.to_json(),
            "properties": properties,
            "roles": {
                name: {"symbol_id": symbol.symbol_id, "slots_per_bar": self.role_slots[name], "location": symbol.pos.to_json()}
                for name, symbol in sorted(self.symbols["role"].items())
            },
            "patterns": value_symbols("pattern"),
            "rhythms": value_symbols("rhythm"),
            "sections": sections,
            "instruments": instruments,
            "orchestras": orchestras,
            "scenes": scenes,
            "forms": forms,
        }

    def block_symbol_to_json(self, symbol: Symbol) -> dict[str, Any]:
        return self.block_declaration_to_json(symbol.node, symbol)

    def block_declaration_to_json(self, decl: Declaration, symbol: Optional[Symbol]) -> dict[str, Any]:
        properties: dict[str, Any] = {}
        extensions: dict[str, Any] = {}
        for item in decl.items:
            if isinstance(item, Assignment):
                properties[item.name] = item.parsed.to_json() if item.parsed else {"kind": "invalid", "raw": item.raw}
            elif item.decl_type == "extension" and item.form == "block" and item.name:
                extensions[item.name] = self.block_declaration_to_json(item, None)
        result = {
            "symbol_id": symbol.symbol_id if symbol else None,
            "properties": properties,
            "tags": [tag.to_json() for tag in decl.tags],
            "location": decl.pos.to_json(),
        }
        if extensions:
            result["extensions"] = extensions
        return result

    def target_to_json(self, name: str, model: dict[str, Any]) -> dict[str, Any]:
        target_symbol = self.symbols["target"][name]
        return {
            "name": name,
            "symbol_id": target_symbol.symbol_id,
            "properties": {
                key: assignment.parsed.to_json() if assignment.parsed else {"kind": "invalid", "raw": assignment.raw}
                for key, assignment in model["properties"].items()
            },
            "routes": {
                key: assignment.parsed.to_json() if assignment.parsed else {"kind": "invalid", "raw": assignment.raw}
                for key, assignment in model["routes"].items()
            },
            "realizations": {
                key: self.block_declaration_to_json(decl, None) for key, decl in model["realize"].items()
            },
            "bindings": {
                key: assignment.parsed.to_json() if assignment.parsed else {"kind": "invalid", "raw": assignment.raw}
                for key, assignment in model["bindings"].items()
            },
            "limits": {
                key: assignment.parsed.to_json() if assignment.parsed else {"kind": "invalid", "raw": assignment.raw}
                for key, assignment in model["limits"].items()
            },
            "extensions": {
                key: self.block_declaration_to_json(decl, None)
                for key, decl in model["extensions"].items()
            },
            "resolved_layer_routes": model["resolved_layer_routes"],
            "required_layers": model["required_layers"],
            "used_instruments": model["used_instruments"],
            "location": target_symbol.pos.to_json(),
        }


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def compile_source(path: Path) -> tuple[dict[str, Any], Diagnostics]:
    diagnostics = Diagnostics()
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        diagnostics.error("IO001", f"cannot read source file: {exc}", SourcePos(1, 1, 0))
        return {
            "schema": "csm.semantic.v1",
            "valid": False,
            "ready_for_backend": False,
            "diagnostics": [item.to_json() for item in diagnostics.items],
        }, diagnostics

    tokens = Lexer(source, diagnostics).lex()
    document = StructuralParser(tokens, source, diagnostics).parse_document()
    analyzer = Analyzer(document, path, source, diagnostics)
    ir = analyzer.analyze()
    return ir, diagnostics


def print_diagnostics(path: Path, diagnostics: Diagnostics) -> None:
    for item in diagnostics.sorted():
        print(
            f"{path}:{item.pos.line}:{item.pos.column}: {item.severity} {item.code}: {item.message}",
            file=sys.stderr if item.severity == "error" else sys.stdout,
        )
        if item.context:
            print(f"    context: {item.context}", file=sys.stderr if item.severity == "error" else sys.stdout)
