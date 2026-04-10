# Cursory: AI Agent Guidelines

This document provides essential context and rules for AI agents (like Gemini) working on the Cursory codebase.

## Architectural Standards

### 1. Unified Buffer Management
- **Rule**: All text data must be managed using the `TextBuffer` structure defined in `cursory.h`.
- **API**: Use the functions in `buffer.c` (`buffer_insert_char`, `buffer_delete_char`, `buffer_load_file`, etc.). Avoid manual allocation or manipulation of `char **lines` arrays.
- **Components**: The Editor, AI Chat Input, and Terminal all build upon this unified API.

### 2. Modular UI Component Pattern
- **Rule**: Each UI panel must reside in its own source file (e.g., `ui_tree.c`, `ui_editor.c`).
- **Control Flow**: `ui.c` acts as the central controller for layout and event dispatching. Do not add component-specific rendering or input logic directly back into `ui.c`.
- **Handling Input**: Panel-specific input handlers should be named `handle_<panel>_input` and prototypes should be in `cursory.h`.

### 3. Thread Safety & AI Integration
- **Rule**: Always lock `state->ai.mutex` before modifying `state->ai` or `state->last_action`.
- **Streaming**: AI responses are handled in a separate thread. Ensure any UI updates during streaming are safe and don't clash with the main loop's panel rendering.

### 4. Patch & Tool Protocol
- **JSON Schemas**: Cursory uses a specific JSON patch format for file edits. Stick to the established `"type": "replace" | "insert" | "delete"` operations.
- **Indices**: Use 1-based line numbers when communicating patches to the IDE; the internal logic handles the 0-based conversion.

## Common Pitfalls
- **Implicit Declarations**: Always check if a new modular function needs a prototype in `cursory.h`.
- **ncurses State**: Be careful with `attron`/`attroff` consistency to avoid "bleeding" styles across panels.
- **Scroll Clamping**: Always call `clamp_scroll()` after any operation that modifies buffer height or viewport dimensions.
