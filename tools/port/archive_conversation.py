#!/usr/bin/env python3
"""Writes a Claude Code session transcript (.jsonl) as Markdown: user and assistant text, and tool
calls in one line each.

    python3 tools/port/archive_conversation.py SESSION.jsonl ["Claude Conversation.md"]
"""
import json
import sys


def text(content):
    if isinstance(content, str):
        return content, []
    parts, tools = [], []
    for c in content:
        if c.get("type") == "text":
            parts.append(c["text"])
        elif c.get("type") == "tool_use":
            inp = c.get("input", {})
            tools.append("`%s`: %s" % (c["name"], (inp.get("description") or inp.get("command") or
                                                    inp.get("file_path") or "")[:160].replace("\n", " ")))
    return "\n\n".join(parts), tools


def main():
    src, out = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "Claude Conversation.md"
    lines = ["# Claude Conversation", "", "Silent Hill 1 PS2 port: the working conversation with Claude Code.", ""]
    for raw in open(src):
        try:
            e = json.loads(raw)
        except ValueError:
            continue
        if e.get("type") not in ("user", "assistant") or e.get("isMeta"):
            continue
        body, tools = text(e.get("message", {}).get("content", ""))
        if e["type"] == "user" and (not body.strip() or "<task-notification>" in body or
                                    body.lstrip().startswith("<system-reminder>")):
            continue
        if body.strip():
            lines += ["## %s" % ("User" if e["type"] == "user" else "Claude"), "", body.strip(), ""]
        lines += ["- tool %s" % t for t in tools]
        if tools:
            lines.append("")
    open(out, "w").write("\n".join(lines) + "\n")
    print("%s: %d lines" % (out, len(lines)))


if __name__ == "__main__":
    main()
