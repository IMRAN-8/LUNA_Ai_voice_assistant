# Luna server

Node.js server providing the voice pipeline and memory API.

## Run locally

```bash
npm install
cp .env.example .env    # fill in your keys
npm start
```

Requires **Node 18+** (global `fetch` and web streams).

## Environment variables

| Variable | Purpose |
|---|---|
| `DATABASE_URL` | Postgres / Supabase connection string |
| `GROQ_API_KEY` | Whisper transcription |
| `OPENROUTER_API_KEY` | `gpt-audio-mini` and `gpt-4o-mini` |
| `PORT` | Optional, defaults to 3000 |

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/converse` | Fast path — audio in, audio out |
| `POST` | `/api/stt` | Speech-to-text (fallback) |
| `POST` | `/api/chat` | Text chat (fallback + memory mode) |
| `POST` | `/api/tts` | Text-to-speech (fallback) |
| `GET` | `/api/memory` | List stored facts |
| `POST` | `/api/memory` | Add a fact |
| `DELETE` | `/api/memory/:id` | Delete a fact |
| `GET` | `/memory` | Browser memory UI |
| `GET` | `/health` | Uptime probe |

See [../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) for design detail.
