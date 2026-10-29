# Console-Based Download Manager Using C

A full-stack download manager with a **C backend** (download engine + REST API) and a **React frontend** (modern dark dashboard).

> **Academic Integrity Note**: The backend and download engine are implemented entirely in C.  
> The user interface uses React, HTML, CSS, and JavaScript.  
> Node.js is used **only** as a frontend build tool — **not** as the application backend.

---

## Architecture

```
┌─────────────────────────┐        HTTP/JSON        ┌──────────────────────────┐
│     React Frontend      │ ◄──── (polling 1s) ────► │      C Backend           │
│    (Vite dev :5173)     │                          │   (Winsock2 :8080)       │
│                         │                          │                          │
│  • Dashboard Cards      │    /api/downloads        │  • API Server (Winsock2) │
│  • URL Input Form       │    /api/stats            │  • Download Engine       │
│  • Download Table       │    /api/history          │    (WinINet HTTP/HTTPS)  │
│  • Progress Bars        │    /api/settings         │  • Download Queue        │
│  • History Panel        │                          │  • File Manager          │
│  • Settings Panel       │                          │  • History (JSON file)   │
│  • Toast Notifications  │                          │  • Logger                │
└─────────────────────────┘                          └──────────────────────────┘
```

## Features

| # | Feature | Status |
|---|---------|--------|
| 1 | Download files from HTTP/HTTPS URLs | ✅ Tested |
| 2 | Download task queue | ✅ Tested |
| 3 | Live progress, speed, size, ETA | ✅ Tested |
| 4 | Multiple queued downloads | ✅ Tested |
| 5 | Pause/Resume (when server supports Range) | ✅ Implemented |
| 6 | Retry and cancellation | ✅ Tested |
| 7 | Persistent download history (JSON) | ✅ Tested |
| 8 | Configurable download directory | ✅ Tested |
| 9 | Prevent overwriting existing files | ✅ Tested |
| 10 | Error handling (invalid URLs, network errors) | ✅ Tested |
| 11 | Task states (queued/downloading/completed/failed/cancelled/paused) | ✅ Tested |
| 12 | Input validation & path sanitization | ✅ Implemented |
| 13 | Timestamped logging to file + stdout | ✅ Tested |

## Project Structure

```
ACP_Project/
├── backend/                        ← C BACKEND
│   ├── build.ps1                   # PowerShell build script
│   ├── download_manager.exe        # Compiled binary
│   ├── downloads/                  # Default download directory
│   ├── history.json                # Persistent history
│   ├── download_manager.log        # Log file
│   └── src/
│       ├── main.c                  # Entry point, initialization
│       ├── common.h                # Shared types & constants
│       ├── api_server.c/.h         # Winsock2 HTTP REST server
│       ├── download_engine.c/.h    # WinINet download logic
│       ├── download_queue.c/.h     # Thread-safe task queue
│       ├── file_manager.c/.h       # Safe file operations
│       ├── history.c/.h            # JSON-based history
│       ├── logger.c/.h             # Thread-safe logging
│       └── cjson/
│           ├── cJSON.c             # JSON library (MIT license)
│           └── cJSON.h
│
└── frontend/                       ← REACT FRONTEND
    ├── package.json
    ├── vite.config.js              # Proxy /api → backend:8080
    ├── index.html
    └── src/
        ├── main.jsx                # React entry point
        ├── App.jsx                 # All components
        ├── api/client.js           # API client (fetch wrapper)
        ├── hooks/useDownloads.js   # Polling hooks
        └── index.css               # Design system (dark theme)
```

## Dependencies

### C Backend (no package manager needed)
| Dependency | Source | Purpose |
|------------|--------|---------|
| GCC 6.3.0 (MinGW) | Pre-installed | C compiler |
| WinINet | Windows SDK (`wininet.h`) | HTTP/HTTPS downloads |
| Winsock2 | Windows SDK (`winsock2.h`) | TCP API server |
| Windows Threads | Windows SDK (`windows.h`) | Concurrent downloads |
| cJSON 1.7.19 | Embedded (MIT license) | JSON parsing/generation |

### React Frontend (Node.js build tools only)
| Dependency | Purpose |
|------------|---------|
| Vite 8.x | Dev server + bundler |
| React 19.x | UI framework |
| @vitejs/plugin-react | JSX support |

## Build & Run

### Prerequisites
- GCC (MinGW) — `gcc --version`
- Node.js 18+ — `node --version`

### Step 1: Build the C backend
```powershell
cd backend
powershell -ExecutionPolicy Bypass -File build.ps1
# Or manually:
# gcc -Wall -Wextra -O2 -DCJSON_HIDE_SYMBOLS src\main.c src\logger.c src\file_manager.c src\download_engine.c src\download_queue.c src\history.c src\api_server.c src\cjson\cJSON.c -o download_manager.exe -lwininet -lws2_32
```

### Step 2: Start the C backend
```powershell
cd backend
.\download_manager.exe
# Output: API server listening on http://127.0.0.1:8080
```

### Step 3: Start the React frontend (separate terminal)
```powershell
cd frontend
npm install          # First time only
npm run dev
# Output: http://localhost:5173/
```

### Step 4: Open http://localhost:5173 in your browser

Press **Ctrl+C** in the backend terminal to shut down.

## REST API Documentation

Base URL: `http://127.0.0.1:8080` (loopback only)

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/downloads` | List all active/queued downloads |
| `POST` | `/api/downloads` | Add new download `{"url": "..."}` |
| `POST` | `/api/downloads/{id}/pause` | Pause a download |
| `POST` | `/api/downloads/{id}/resume` | Resume a paused download |
| `POST` | `/api/downloads/{id}/cancel` | Cancel a download |
| `POST` | `/api/downloads/{id}/retry` | Retry a failed download |
| `DELETE` | `/api/downloads/{id}` | Remove from list |
| `GET` | `/api/stats` | Get dashboard statistics |
| `GET` | `/api/history` | Get download history |
| `DELETE` | `/api/history` | Clear history |
| `GET` | `/api/settings` | Get current settings |
| `PUT` | `/api/settings` | Update settings |

### Example API calls:
```powershell
# Add a download
Invoke-WebRequest -Uri "http://127.0.0.1:8080/api/downloads" -Method POST -Body '{"url":"https://example.com/file.pdf"}' -ContentType "application/json"

# Check stats
Invoke-WebRequest -Uri "http://127.0.0.1:8080/api/stats" -UseBasicParsing
# → {"total":1,"active":0,"completed":1,"failed":0}
```

## Security

- API server binds to **127.0.0.1** only (loopback — not network-accessible)
- CORS restricted to `http://localhost:5173`
- Filenames sanitized against directory traversal (`..`, `/`, `\`, special chars)
- Input validated on every API endpoint
- No arbitrary filesystem access — downloads go to a configured directory only

## Key C Concepts (Viva Preparation)

### 1. Structs and Memory Layout
The `DownloadTask` struct in `common.h` uses fixed-size char arrays instead of heap pointers. This avoids `malloc`/`free` complexity but limits string lengths.

### 2. Pointers and Function Parameters
Functions like `engine_start_download(DownloadTask *task)` receive a pointer to the task, allowing direct modification of the original struct from worker threads.

### 3. Thread Safety (CRITICAL_SECTION)
`CRITICAL_SECTION` is the Windows equivalent of `pthread_mutex_t`. Each task has its own lock (`TASK_LOCK/TASK_UNLOCK`) to protect fields modified by download threads.

### 4. Windows Threads (CreateThread)
Each download runs in a separate thread created with `CreateThread()`. The thread function signature is `DWORD WINAPI func(LPVOID param)`.

### 5. WinINet API
`InternetOpenA()` → session handle; `InternetOpenUrlA()` → URL handle; `InternetReadFile()` → data. Similar to opening a file: open, read in loop, close.

### 6. Winsock2 (TCP Server)
`socket()` → `bind()` → `listen()` → `accept()` → `recv()`/`send()` → `closesocket()`. The accept loop creates a new thread per client.

### 7. File I/O
`fopen("wb")` for binary write, `fwrite()` for writing chunks, `fclose()`. Binary mode prevents `\n` → `\r\n` conversion on Windows.

### 8. Volatile Variables
`volatile` on `cancel_flag` tells the compiler not to optimize away reads — essential for cross-thread communication.

### 9. HTTP Range Headers
Pause/resume uses `Range: bytes=N-` to request only the remaining portion. The server must support `Accept-Ranges: bytes`.

### 10. JSON with cJSON
`cJSON_CreateObject()`, `cJSON_AddStringToObject()`, `cJSON_Print()`, `cJSON_Parse()` — simple API for building and parsing JSON in C.

## Viva Questions & Answers

**Q: Why not use libcurl?**  
A: libcurl was not installed in our MinGW environment. WinINet is a Windows-native HTTP/HTTPS library that's always available and handles SSL certificates automatically.

**Q: How does the frontend communicate with the C backend?**  
A: Via REST API over HTTP. The React frontend polls `GET /api/downloads` every second and sends commands via POST/DELETE. The C backend runs a Winsock2 HTTP server.

**Q: How is thread safety maintained?**  
A: Each download task has its own `CRITICAL_SECTION` mutex. The queue has a global lock. All shared state is accessed within lock/unlock pairs.

**Q: How does pause/resume work?**  
A: Pause sets a `cancel_flag` that exits the download loop, preserving `bytes_downloaded`. Resume sends a new request with `Range: bytes=<offset>-` header and appends to the partial file.

**Q: How is file overwriting prevented?**  
A: `fm_build_unique_path()` checks if a file exists and appends `_1`, `_2`, etc. to the filename.

**Q: Is the API secure?**  
A: It binds to 127.0.0.1 (loopback only), validates all inputs, sanitizes filenames, and restricts CORS to the dev server origin.

## Limitations

1. No SOCKS/authenticated proxy support (WinINet uses system proxy)
2. No multi-segment parallel download (single stream per file)
3. History is stored in a flat JSON file (not a database)
4. Queue is in-memory (resets on restart; history persists)
5. No FTP/BitTorrent support (HTTP/HTTPS only)
6. GetTickCount() wraps after ~49 days (negligible for downloads)

## Test Results

| Test Case | Result |
|-----------|--------|
| Download small PDF (13KB) | ✅ Pass — file integrity verified |
| Download PNG image | ✅ Pass — file downloaded correctly |
| Stats API returns correct counts | ✅ Pass — `{"total":2,"active":0,"completed":2,"failed":0}` |
| Add download via POST API | ✅ Pass — returns `{"id":1,"status":"queued"}` |
| Invalid URL handling | ✅ Pass — sets STATE_FAILED with error message |
| Unique filename generation | ✅ Pass — creates `dummy_1.pdf` on collision |
| React UI renders dashboard | ✅ Pass — screenshot verified |
| Real-time progress updates | ✅ Pass — 1-second polling shows live data |
| Compilation with strict warnings | ✅ Pass — zero errors, zero warnings |

## License

This is a university project. cJSON is used under the MIT License.
