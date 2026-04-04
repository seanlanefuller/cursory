# Cursory: System Design & AI Grounding

## Architecture
Cursory is built as a single-threaded ncurses event loop with detached POSIX threads for AI communication.

### Core Components
1. **AppState**: Centralized state management using `pthread_mutex` for thread safety.
2. **Ncurses TUI**: A panel-based rendering system with manual layout calculation.
3. **AI Dispatcher**: 
   - Uses `libcurl` to stream responses from Ollama.
   - Implements a **Binary-Choice Grounding** strategy to prevent model hallucinations.
   - **Greedy Last-Occurrence Parser**: A robust JSON extractor that handles AI repetitions by favoring the last occurrence of a key.

## AI Stability Breakthroughs
To make Llama 3 8B reliable for tool usage in a terminal environment, Cursory implements:

### 1. Completion-State Persona
Instead of a chat-like prompt, Cursory uses a robotic "Completion-State" structure:
```text
### PERSONALITY: ...
### TOOLS: ...
### TASK: Choose ONE (Tool or Final Reply)
### CONTEXT: ...
### RESPONSE: 
```
This forces the model into a strict instruction-following mode.

### 2. Multi-Turn Feedback Loop
If the dispatcher detects an unknown tool call or a malformed JSON action, it automatically injects an error message into the next turn's history. The model perceives this as an "execution failure" and re-plans using the approved toolset.

### 3. Context Pruning
To prevent the 8B-parameter model from getting "lost" or becoming recursive, the conversation history is pruned to the last 4 messages, maintaining a tight focus on the immediate task.

## Future Roadmap (Phase 7+)
- Full-featured Editor (Insertion/Deletion)
- Regex-based Syntax Highlighting
- Global `.cursoryrc` configuration support
