# Cursory

**Cursory** is a lightweight, ncurses-based IDE designed for stable, project-aware AI pair programming using Llama 3 (via Ollama). It provides a terminal-native workspace with real-time AI tool execution, allowing the model to browse, read, and search your codebase directly.

## Features
- **Project-Aware AI**: Multi-turn conversation loop where the AI uses tools (`list_dir`, `read_file`, `grep_file`) to explore your project.
- **Ncurses Interface**: Blazing fast, terminal-native TUI with split panels for File Tree, Editor, AI Chat, and Terminal.
- **Llama 3 Optimized**: Custom prompt-grounding strategies to force reliable JSON-only tool usage from medium-sized local models.
- **Zero Configuration**: Single C file implementation with minimal dependencies.

## Installation
Ensure you have `ncurses`, `libcurl`, and `libutil` installed.
```bash
sudo apt-get install libncurses5-dev libcurl4-openssl-dev
```
Clone and build:
```bash
make
./cursory
```

## AI Setup
Cursory expects a local [Ollama](https://ollama.com/) instance running with the `llama3` model:
```bash
ollama run llama3
```
AI interaction is logged to `cursory.log` for transparency.

## License
MIT License. Feel free to use, modify, and distribute.

---
*Built for the community by Sean.*
