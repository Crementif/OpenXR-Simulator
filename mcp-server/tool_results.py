"""Transport-independent tool values; only the entrypoint creates MCP objects."""

from dataclasses import dataclass


@dataclass(frozen=True)
class TextResult:
    text: str


@dataclass(frozen=True)
class ImageResult:
    data: bytes
    mime_type: str = "image/jpeg"
