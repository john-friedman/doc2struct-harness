"""PDF parsing with interchangeable presets, models, and reasoning effort."""
from . import parser
from .parser import parse, parse_async, parse_many

harness = parser


def markdown_converter(target):
    """Load the optional benchmark output adapter."""
    if target != "parsebench":
        raise ValueError(f"Unknown conversion target: {target}")
    from .parsebench import convert
    return convert


__all__ = ["parse", "parse_async", "parse_many", "parser", "harness", "markdown_converter"]
