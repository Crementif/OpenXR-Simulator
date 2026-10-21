#!/usr/bin/env python3
"""OpenXR Simulator MCP transport. Domain owners contain all runtime state."""

import asyncio
import base64
import sys

try:
    from mcp.server import Server
    from mcp.server.stdio import stdio_server
    from mcp.types import Tool, TextContent, ImageContent
except ImportError:
    print("Error: MCP SDK not installed. Run: pip install mcp", file=sys.stderr)
    raise SystemExit(1)

from mcp_application import McpApplication
from tool_catalog import tool_definitions
from tool_results import ImageResult, TextResult


def create_server(application):
    server = Server("openxr-simulator")

    @server.list_tools()
    async def list_tools():
        return [Tool(**definition) for definition in tool_definitions()]

    @server.call_tool()
    async def call_tool(name, arguments):
        values = await application.call_async(name, arguments)
        content = []
        for value in values:
            if isinstance(value, TextResult):
                content.append(TextContent(type="text", text=value.text))
            elif isinstance(value, ImageResult):
                content.append(
                    ImageContent(
                        type="image",
                        data=base64.standard_b64encode(value.data).decode("ascii"),
                        mimeType=value.mime_type,
                    )
                )
            else:
                raise TypeError("Tool domain returned an unsupported result")
        return content

    return server


async def main():
    application = McpApplication()
    server = create_server(application)
    try:
        async with stdio_server() as (read_stream, write_stream):
            await server.run(
                read_stream, write_stream, server.create_initialization_options()
            )
    finally:
        await asyncio.to_thread(application.close)


def cli():
    asyncio.run(main())


if __name__ == "__main__":
    cli()
