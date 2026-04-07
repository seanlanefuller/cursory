# Cursory

**Cursory** is a lightweight, natively compiled `ncurses`-based IDE designed for stable, project-aware AI pair programming powered by **Qwen2.5-Coder** (via Ollama). It provides a terminal-native workspace integrated with real-time AI tool execution, allowing the LLM backend to iteratively explore, parse, and strictly patch your codebase structurally via a robust local framework environment.

## Features
- **Project-Aware Context**: Multi-turn conversation logic empowering the AI to dynamically execute tools (`list_dir`, `read_file`, `grep_file`) to understand the active code structure before generating context.
- **Robust Multi-Patch Editing**: Structural JSON patch loop utilizing strict `[{"type": "insert" | "replace"}]` formatting schemas. Seamlessly maps logical `1`-based LLM coordinates onto raw `C` memory `0`-based indices, while mitigating hallucinated drops securely using bounds-clamping and synthetic multi-string array padding.
- **Command Palette Execution (F1)**: VS Code-esque keystroke UI hub mapped functionally into an `ncurses` array overlaid mechanically upon the active viewport, exposing global bindings (Quick Save As, File Tree toggle, and Python script deployment) uniformly.
- **Unified Editor Viewport**: Cross-panel synchronized input scrolling covering File Trees, Editor Buffers, LLM streaming Chat interfaces, and dynamic sub-process Terminal shells smoothly.

## Installation
Ensure you have `ncurses`, `libcurl`, `pthreads`, and `libutil` installed.
```bash
sudo apt-get install libncurses5-dev libcurl4-openssl-dev
```
Clone and build:
```bash
make
./cursory
```

## AI Setup & Diagnostics
Cursory assumes a local [Ollama](https://ollama.com/) interface running at `http://localhost:11434` targeting standard models like `qwen2.5-coder:7b`.
- **System Constraints**: The AI is securely prevented from hallucinating redundant JSON elements (like `{"append_file"}`) via rigorous internal prompt schema mapping rules.
- **Diagnostic Logging**: Track crash traces internally via `/tmp/cursory.log`.
- **Persistent Archival**: Automated conversational arrays dynamically stored at `/tmp/cursory_chat.log`.

## License
MIT License. Feel free to use, modify, and distribute.

---
*Built natively for the Linux terminal ecosystem.*
