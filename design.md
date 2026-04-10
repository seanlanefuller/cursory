# Cursory: System Design & AI Grounding

## Architecture
Cursory leverages a unified single-threaded `ncurses` event loop for the terminal UI, isolating heavy LLM stream decoding into detached POSIX threads. The architecture is modularized to ensure high maintainability and performance.

### Core Components
1. **Modular UI Component System**: The UI is decoupled into specialized components, each isolated in its own source file (`ui_tree.c`, `ui_editor.c`, `ui_chat.c`, `ui_terminal.c`). A central controller (`ui.c`) manages layout orchestration and global event dispatching.
2. **Unified `TextBuffer` API**: All text-based components (Editor, Chat Input, Terminal) utilize a standardized `TextBuffer` structure. This unifies buffer management logic (line allocation, insertion, deletion) into a single, hardened API (`buffer.c`).
3. **Thread-Safe AppState**: Globally accessible state architecture securely mapping string mutations through `pthread_mutex`, blocking dynamic buffer crashes from asynchronous AI responses.
4. **Interactive Command Palette**: Replaces strictly static keyboard bindings with an interactively parsed `ncurses` pop-up struct array (invoked via `F1`), natively processing global overrides and IDE state changes.
5. **Internal Parsing Tool Loop**:
   - Seamless internal hooks evaluating exact payloads directly inside the JSON interpreter stream block to trigger `tool_read_file`, `tool_list_dir`, and native grep search callbacks.

## AI Stability Breakthroughs
To make code-optimized local models reliably execute edits, Cursory enacts aggressive LLM structural constraints:

### 1. Robust JSON Structure Handlers
The internal payload scanner `parse_patch_ops` processes LLM output safely using custom string scanners:
- **Recursive Stream Reading**: Accumulates subsequent `"patch"` blocks sequentially down the buffer, satisfying multi-patch schema replies.
- **Payload Banning**: Restricting JSON execution strictly to tool triggers to avoid serialization overhead.

### 2. Multi-Turn Edit Grounding & Interpolation
- **`1`-Based Mapping Conversions**: Cursory absorbs `1`-based document indices mapping dynamically, inverting them into `0`-based indices for safe C memory array execution.
- **Bounds Clamping & Padding Rewrites**: Identification of hallucinated bounds and translation into valid `replace` overwrites to lock synthetic padding into the stream.

## Status & Roadmap
- [x] Unified Panel Scrolling & Interactive Command Palette Setup
- [x] Hardened Local Ollama API Output Parsers
- [x] Systemic `1`-based diff interpolation and OOB mitigation
- [x] Modular UI Architecture & Unified `TextBuffer` API
- [ ] Regex-based Syntax Highlighting Engine
- [ ] Cross-Panel Global IDE Configuration (`.cursoryrc`)

