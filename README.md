This is a project I built with codecrafters (https://app.codecrafters.io/) with their Build-your-own-claude-code challenge , but there are many extensions to it now in this project . Only tested with GROQ provider , so please open an issue if you encounter any problems with other providers.

# Claude Code C - AI-Agnostic Coding Assistant

[![progress-banner](https://backend.codecrafters.io/progress/claude-code/377cd9e8-269e-4897-9029-2cdfe18852d6)](https://app.codecrafters.io/users/ashishk15678?r=2qF)
[![Language](https://img.shields.io/badge/language-C23-blue.svg)](#)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](#)
[![Dependency-libcurl](https://img.shields.io/badge/dependency-libcurl-brightgreen.svg)](#)
[![Dependency-cJSON](https://img.shields.io/badge/dependency-cJSON-blueviolet.svg)](#)
[![Providers](https://img.shields.io/badge/providers-multi--compatible-purple.svg)](#)

Claude Code C is an advanced, terminal-based AI coding assistant written in C (using the C23 standard). Inspired by Anthropic's Claude Code and Agy, it features a complete multi-turn agent loop, interactive REPL, an automated tool execution system, and multi-provider compatibility.

This project is AI provider-agnostic, meaning it can interface with OpenRouter, OpenAI, Anthropic, Google Gemini, Groq, or local Ollama servers. It translates payloads transparently between standard OpenAI Chat Completions formats and native Anthropic Messages API formats under the hood.

---

## Key Features

| Feature | Description | Supported Backends |
| :--- | :--- | :--- |
| **Interactive REPL** | Multi-turn chat interface maintaining full session conversation history. | All |
| **Interactive Model Dropdown** | Choose providers and models interactively on startup, or switch dynamically using `/settings` and `/model`. | All |
| **Tool Calling Suite** | 7 pre-configured tools enabling the LLM to inspect, read, search, and edit the filesystem. | All |
| **Provider Agnostic** | Standard API client for multiple AI services and local LLMs. | OpenAI, Anthropic, Gemini, Groq, Ollama, OpenRouter |
| **Anthropic Adapter** | In-memory translation layer converting OpenAI formats into Anthropic Messages schemas. | Anthropic |
| **Custom System Prompts** | Prepend custom rules combined with default system prompt instructions. | All |
| **Resilient Agent Loop** | Automatic retries and feedback loops providing tool error output back to the LLM for correction. | All |

---

## Tool Calling Suite

Claude Code C implements the following tools that the AI can call dynamically to explore codebases, inspect environment metadata, or perform writes and commands:

| Tool Name | Parameters | Description |
| :--- | :--- | :--- |
| `Read` | `file_path` | Reads and returns the contents of a file on the local filesystem. |
| `Write` | `file_path`, `content` | Writes or overwrites contents of a file. Automatically creates directories. |
| `Bash` | `command` | Runs a shell script/command inside an interactive Bash environment and returns output. |
| `ListDirectory` | `path` (optional) | Lists files, links, and directories inside the given path. |
| `FindFiles` | `directory`, `pattern` | Recursively walks directories to locate files matching the pattern. |
| `FileSearch` | `directory`, `query` | Recursive search for text occurrences inside files (similar to grep). |
| `SysInfo` | None | Returns system details including OS, machine type, user, CWD, and shell. |

---

## Compilation and Setup

### Prerequisites
- A C compiler supporting C23 (GCC 13+ or Clang 16+)
- CMake (version 3.13+)
- Libcurl and cJSON library. If using `vcpkg`, dependencies are handled automatically via `vcpkg.json`.

### Building
Compile using CMake:
```sh
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
cmake --build ./build
```

Alternatively, use the convenience wrapper script:
```sh
./your_program.sh --help
```

---

## Command Line Usage

| Option (Short) | Option (Long) | Argument | Description |
| :--- | :--- | :--- | :--- |
| `-p` | `--prompt` | `string` | Runs a single user prompt non-interactively and exits. |
| `-i` | `--interactive` | None | Starts an interactive REPL session (default). |
| `-c` | `--choose-model` | None | Launches interactive dropdown menus to choose provider, model, and input API key. |
| `-r` | `--provider` | `provider` | Selects LLM backend: `openrouter`, `openai`, `anthropic`, `gemini`, `groq`, `ollama`. |
| `-m` | `--model` | `string` | LLM model identifier. |
| `-k` | `--api-key` | `string` | API token/secret key. |
| `-u` | `--base-url` | `string` | Custom API endpoint URL. |
| `-s` | `--system-prompt`| `file` | Path to a file containing custom system prompt rules. |
| None | `--user-system-prompt`| `string` | String containing custom system prompt rules directly. |
| `-h` | `--help` | None | Prints the help menu and lists options. |

---

## Environment Variables

You can configure the application without command-line arguments using these environment variables:

| Variable | Description | Default (per provider) |
| :--- | :--- | :--- |
| `AI_PROVIDER` | AI service provider (`openai`, `anthropic`, `gemini`, `groq`, `ollama`, `openrouter`). | `openrouter` |
| `AI_MODEL` | The LLM model to request. | Provider specific (e.g. `gpt-4o`, `claude-3-5-sonnet-20241022`) |
| `AI_API_KEY` | API secret key (falls back to provider-specific environment variables). | None |
| `AI_BASE_URL` | Override endpoint for custom hosts or local deployments. | Provider specific (e.g. `http://localhost:11434/v1` for Ollama) |
| `AI_SYSTEM_PROMPT_FILE` | Path to file containing custom instructions. | None |

---

## Architecture and Implementation Details

```
                    +------------------------------------+
                    |             main.c                 |
                    |   (CLI Parser & Env configuration)  |
                    +-----------------+------------------+
                                      |
                                      v
                    +------------------------------------+
                    |             app.c                  |
                    |   (Interactive REPL & Agent Loop)   |
                    +--------+------------------+--------+
                             |                  |
                             v                  v
              +--------------+----+        +----+--------------+
              |    provider.c     |        |  tool_registry.c  |
              |                   |        |                   |
              | (cURL HTTP Client |        |  (Read, Write,    |
              |  Anthropic Schema |        |   Bash, Grep,     |
              |  Translation      |        |   Find, SysInfo)  |
              +-------------------+        +-------------------+
```

### 1. In-Memory Request & Response Translation
Anthropic's Messages API specifies system prompts, tool schemas, and assistant replies differently than the standard OpenAI Chat Completions API. Instead of duplicating the agent loop, `provider.c` houses a dual translation layer:
- **Request Translator**: Converts messages by moving system messages to top-level fields, renaming parameter fields to `input_schema`, and combining contiguous tool call messages.
- **Response Translator**: Reconstructs standard OpenAI tool call formats from Anthropic's output structure.

### 2. Multi-turn History and Interactive Agent Loop
The REPL is powered by a JSON messages array that grows during the session. When the LLM issues tool calls, the agent loop pauses, invokes the tool registry, displays status updates to the terminal, and posts the responses back to the LLM. The agent runs up to 20 sub-iterations dynamically before yielding to the user.
