# Spark2026 — AI Code Debugger

An AI-powered code review tool that analyzes GitHub repositories using AWS Bedrock (Claude 3 Sonnet). Submit a repo URL and get per-file issue reports streamed back in real time.

---

## How It Works

1. Submit a GitHub repo URL from the frontend
2. The server clones the repo and queues each file for analysis
3. Each file is sent to AWS Bedrock (Claude 3 Sonnet) with a code review prompt
4. Results stream back to the frontend via Server-Sent Events (SSE)
5. Issues are displayed grouped by category with severity badges

---

## Stack

| Layer | Technology |
|---|---|
| Backend | C++17, [cpp-httplib](https://github.com/yhirose/cpp-httplib) |
| AI | AWS Bedrock — Claude 3 Sonnet |
| Frontend | Vanilla HTML/CSS/JS |
| Build | CMake 3.15+ |

---

## Prerequisites

- CMake 3.15+
- C++17 compiler (clang++ or g++)
- AWS account with Bedrock access and Claude 3 Sonnet enabled
- AWS credentials (access key, secret key, session token, region)
- Git (must be installed at `/usr/bin/git`)

---

## Build

```bash
cmake -S backEnd -B build
cmake --build build --target server
```

---

## Run

Set your AWS credentials and allowed frontend origin, then start the server:

```bash
export AWS_ACCESS_KEY_ID=<your_access_key>
export AWS_SECRET_ACCESS_KEY=<your_secret_key>
export AWS_SESSION_TOKEN=<your_session_token>
export AWS_DEFAULT_REGION=<your_region>
export ALLOWED_ORIGINS=http://127.0.0.1:5500

./build/server
```

Serve the frontend:

```bash
cd frontEnd && python3 -m http.server 5500
```

Open `http://127.0.0.1:5500` in your browser.

---

## Configuration

| Environment Variable | Description | Default |
|---|---|---|
| `ALLOWED_ORIGINS` | Comma-separated list of allowed CORS origins | Wildcard (`*`) |
| `AWS_ACCESS_KEY_ID` | AWS access key | — |
| `AWS_SECRET_ACCESS_KEY` | AWS secret key | — |
| `AWS_SESSION_TOKEN` | AWS session token (for temporary credentials) | — |
| `AWS_DEFAULT_REGION` | AWS region for Bedrock | — |

---

## Rate Limits

| Endpoint | Limit |
|---|---|
| `POST /debug` | 5 requests / 60s per IP |
| `GET /stream` | 30 requests / 60s per IP |
| `GET /health` | 60 requests / 60s per IP |

---

## Project Structure

```
├── backEnd/
│   ├── server.cpp          # Main HTTP server
│   ├── cors_validator.hpp  # CORS origin validation
│   ├── rate_limiter.hpp    # Per-IP rate limiting
│   ├── threadpool.hpp      # Thread pool (10 workers)
│   ├── classes.hpp         # Shared data structures
│   └── CMakeLists.txt
├── frontEnd/
│   ├── index.html
│   ├── linking.js          # API calls and UI logic
│   └── style.css
└── AWS_Bedrock_Prompt/
    └── Prompt.txt          # System prompt for Claude
```
