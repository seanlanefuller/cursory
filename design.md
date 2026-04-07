# Cursory: System Design & AI Grounding

## Architecture
Cursory leverages a unified single-threaded `ncurses` event loop for the terminal UI, isolating heavy LLM stream decoding safely into explicitly detached POSIX threads.

### Core Components
1. **Thread-Safe AppState**: Globally accessible state architecture securely mapping string mutations through `pthread_mutex`, blocking dynamic buffer crashes from asynchronous AI responses.
2. **Interactive Command Palette**: Replaces strictly static keyboard bindings with an interactively parsed `ncurses` pop-up struct array (invoked via `F1`). It acts functionally identical to VS Code's Command Palette, natively processing global overrides (Panel Toggles, Save As, and Select All bindings).
3. **Internal Parsing Tool Loop**:
   - Seamless internal hooks evaluating exact payloads directly inside the JSON interpreter stream block to explicitly trigger `tool_read_file`, `tool_list_dir`, and native grep search callbacks, automatically routing the systemic payload query back into the AI generation loop recursively.

## AI Stability Breakthroughs
To make code-optimized open-source local models (like `Qwen2.5-Coder`) reliably execute complex multi-block file overwrites, Cursory enacts aggressive LLM structural constraints:

### 1. Robust JSON Structure Handlers
The internal payload scanner `parse_patch_ops` processes LLM output safely using custom string scanners structurally resistant against hallucinated non-string formats:
- **Recursive Stream Reading**: Cursory does NOT halt scanning after encountering the first chunk; it intelligently accumulates subsequent `"patch"` blocks sequentially down the buffer, fully satisfying multi-patch schema replies.
- **Payload Banning**: Bypassing recursive serialization by strictly ordering the AI to output plain text conversing loops, aggressively restricting JSON execution strictly to tool triggers.

### 2. Multi-Turn Edit Grounding & Interpolation
- **`1`-Based Mapping Conversions**: Since large local models naturally hallucinate their targets natively according to real-world `1`-based document indices mapping, Cursory absorbs the `1`-based values dynamically internally, inverting `(idx - 1)` mapping dynamically right before buffer insertion natively executing onto the C memory array structure safely enforcing parity.
- **Bounds Clamping & Padding Rewrites**: OOB (Out-of-Bounds) drops naturally floor natively down to `0`. If multiple consecutive `insert` string directives hallucinate dynamically above `line_count` (which previously produced whitespace artifacts using intermediate space shifts), the parser identifies identical bounds and translates them natively into `replace` overwrites mathematically locking the synthetic padding into the stream.

## Status & Roadmap
- [x] Unified Panel Scrolling & Interactive Command Palette Setup
- [x] Hardened Local Ollama API Output Parsers
- [x] Systemic `1`-based diff interpolation and OOB mitigation
- [ ] Regex-based Syntax Highlighting Engine
- [ ] Cross-Panel Global IDE Configuration (`.cursoryrc`)

