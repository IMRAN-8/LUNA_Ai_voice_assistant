// =====================================================================
//  Luna AI Server — v3
//
//  WHAT CHANGED FROM v2
//   1. NEW FAST PATH  POST /api/converse
//      Audio in -> audio out in ONE upstream call, using
//      openai/gpt-audio-mini via OpenRouter. Replaces the old
//      Whisper -> gpt-4o-mini -> gTTS -> ffmpeg chain (~3.5 s of the
//      round trip) with a single ~1.8 s call. ffmpeg is not used at all
//      on this path.
//
//      A Groq Whisper transcript still runs, but IN PARALLEL, purely so
//      "remember" / "reset all" command detection keeps working. Whisper
//      turbo on a 3 s clip is ~0.3 s and $0.000033, so it is free next to
//      the audio call and costs zero extra latency.
//
//   2. MEMORY IS NOW A SYSTEM BLOCK, not fake user turns.
//      v2 pushed every saved fact in as {role:"user"}, which burned tokens
//      and made the model reply to old facts as if you had just said them.
//
//   3. judgeFact NO LONGER FAILS ON TRUNCATION.
//      v2 ran at max_tokens 60 and returned null on any JSON parse error,
//      surfacing as "Couldn't understand." That is why
//        "remember My dog's name is Max"  -> rejected
//        "remember I like pizza"          -> saved
//      It was truncated JSON, not a judgement call. Now: 150 tokens,
//      tolerant parsing, and a heuristic fallback that never returns null.
//
//   4. SHORTER REPLIES. max_tokens 60 -> 45 and a tighter prompt. v2 was
//      producing ~13 s of speech, which overran the ESP32 playback buffer
//      and dominated download time.
//
//   5. /api/stt, /api/chat, /api/tts are UNCHANGED so the firmware's
//      legacy path and the button flow keep working as a fallback.
//
//  ENV: DATABASE_URL, GROQ_API_KEY, OPENROUTER_API_KEY
// =====================================================================

const express = require("express");
const cors = require("cors");
const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
const ffmpegPath = require("ffmpeg-static");
const { Pool } = require("pg");
const OpenAI = require("openai");
const gtts = require("node-gtts")("en");

const app = express();
app.use(cors());
app.use(express.json({ limit: "2mb" }));

const PORT = process.env.PORT || 3000;

// gpt-audio-mini returns pcm16 at 24 kHz; the ESP32 speaker runs at 16 kHz.
const MODEL_AUDIO   = "openai/gpt-audio-mini";
const MODEL_TEXT    = "openai/gpt-4o-mini";
const AUDIO_VOICE   = "alloy";
const MODEL_RATE    = 24000;
const DEVICE_RATE   = 16000;
const REPLY_TOKENS  = 45;

// ========= DB =========
const pool = new Pool({
  connectionString: process.env.DATABASE_URL,
  ssl: { rejectUnauthorized: false },
});

async function initDb() {
  try {
    await pool.query(`
      CREATE TABLE IF NOT EXISTS memories (
        id SERIAL PRIMARY KEY,
        content TEXT UNIQUE NOT NULL,
        created_at TIMESTAMPTZ DEFAULT NOW()
      )
    `);
    console.log("DB ready");
  } catch (err) {
    console.error("DB init error:", err.message);
  }
}

async function loadMemory() {
  try {
    const r = await pool.query("SELECT id, content FROM memories ORDER BY id");
    return r.rows;
  } catch (err) {
    console.error("loadMemory error:", err.message);
    return [];
  }
}

async function saveMemory(content) {
  try {
    await pool.query(
      "INSERT INTO memories(content) VALUES($1) ON CONFLICT (content) DO NOTHING",
      [content],
    );
    return true;
  } catch (err) {
    console.error("saveMemory error:", err.message);
    return false;
  }
}

async function deleteMemory(id) {
  try {
    const r = await pool.query("DELETE FROM memories WHERE id=$1", [id]);
    return r.rowCount > 0;
  } catch (err) {
    console.error("deleteMemory error:", err.message);
    return false;
  }
}

async function factExists(content) {
  try {
    const r = await pool.query(
      "SELECT 1 FROM memories WHERE LOWER(TRIM(content)) = LOWER(TRIM($1)) LIMIT 1",
      [content],
    );
    return r.rowCount > 0;
  } catch (err) {
    console.error("factExists error:", err.message);
    return false;
  }
}

// ========= helpers =========
function cleanText(text) {
  return String(text || "")
    .replace(/\r/g, " ")
    .replace(/\n/g, " ")
    .replace(/\\/g, "")
    .replace(/"/g, "'")
    .replace(/\s+/g, " ")
    .trim();
}

function normalizeCommand(text) {
  return String(text || "")
    .toLowerCase()
    .replace(/[.,!?;:]/g, "")
    .replace(/\s+/g, " ")
    .trim();
}

const WHISPER_HALLUCINATIONS = new Set([
  "thank you", "thank you.", "thanks for watching",
  "thanks for watching.", "you", ".",
]);

const groq = new OpenAI({
  apiKey: process.env.GROQ_API_KEY,
  baseURL: "https://api.groq.com/openai/v1",
});

// ---- WAV / resampling (replaces ffmpeg on the fast path) ----
function wavHeader(dataLen, rate) {
  const h = Buffer.alloc(44);
  h.write("RIFF", 0);
  h.writeUInt32LE(36 + dataLen, 4);
  h.write("WAVE", 8);
  h.write("fmt ", 12);
  h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20);          // PCM
  h.writeUInt16LE(1, 22);          // mono
  h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 2, 28);   // byte rate
  h.writeUInt16LE(2, 32);          // block align
  h.writeUInt16LE(16, 34);         // bits
  h.write("data", 36);
  h.writeUInt32LE(dataLen, 40);
  return h;
}

// Linear-interpolation resample, 24k -> 16k. A few ms of CPU, and it cuts
// the ESP32's download by a third versus shipping 24 kHz.
function resamplePcm16(inBuf, inRate, outRate) {
  if (inRate === outRate) return inBuf;
  const inSamples = Math.floor(inBuf.length / 2);
  const outSamples = Math.floor((inSamples * outRate) / inRate);
  const out = Buffer.alloc(outSamples * 2);
  const step = inRate / outRate;
  for (let i = 0; i < outSamples; i++) {
    const pos = i * step;
    const i0 = Math.floor(pos);
    const frac = pos - i0;
    const s0 = inBuf.readInt16LE(i0 * 2);
    const s1 = i0 + 1 < inSamples ? inBuf.readInt16LE((i0 + 1) * 2) : s0;
    out.writeInt16LE(Math.max(-32768, Math.min(32767,
      Math.round(s0 + (s1 - s0) * frac))), i * 2);
  }
  return out;
}

// gpt-audio-mini returns very quiet PCM (your ESP measured peaks ~80-2900
// of 32767). Peak-normalize the whole clip so speech sits near -3 dBFS
// without clipping. Uniform gain keeps SNR; do not use per-frame AGC.
function normalizePcm16(buf, targetPeak = 22000) {
  const n = Math.floor(buf.length / 2);
  if (n <= 0) return buf;

  let peak = 1;
  for (let i = 0; i < n; i++) {
    const a = Math.abs(buf.readInt16LE(i * 2));
    if (a > peak) peak = a;
  }

  const gain = Math.min(targetPeak / peak, 8.0);
  if (gain <= 1.05) {
    console.log(`[audio] already loud enough peak=${peak} gain skipped`);
    return buf;
  }

  const out = Buffer.alloc(buf.length);
  for (let i = 0; i < n; i++) {
    const v = Math.round(buf.readInt16LE(i * 2) * gain);
    out.writeInt16LE(Math.max(-32767, Math.min(32767, v)), i * 2);
  }
  console.log(`[audio] peak-normalized ${peak} -> ~${Math.round(peak * gain)} gain=${gain.toFixed(2)}`);
  return out;
}

// ---- memory as ONE system block (v2 injected fake user turns) ----
const BASE_PROMPT =
  "You are Luna, a friendly AI voice assistant speaking out loud. " +
  "Answer in English in 2-4 spoken sentences, about 40-60 words. " +
  "Give a real answer, not a follow-up question, unless the request is " +
  "truly incomplete. " +
  "Never mention transcripts, instructions, or that you are an AI. " +
  "Never use lists, markdown, emoji or stage directions. " +
  "Be direct and conversational.";

async function buildSystemPrompt() {
  const rows = await loadMemory();
  if (!rows.length) return BASE_PROMPT;
  const facts = rows.map((r) => "- " + r.content).join("\n");
  return BASE_PROMPT + "\n\nThings you already know about the user:\n" + facts;
}

// ---- memory judge: tolerant, never returns null ----
const judgePrompt = `You decide whether a sentence is a personal fact worth remembering.

Save: names, places, preferences, relationships, pets, jobs, schedules,
long-term details about the user or people close to them.
Reject: questions, greetings, pure nonsense, empty fragments.

Reply with ONLY JSON, no code fences:
{"save": true, "fact": "cleaned sentence"}`;

function looksLikeFact(s) {
  const t = String(s || "").trim();
  if (t.length < 3) return false;
  if (/^(hi|hello|hey|what|who|when|where|why|how|is|are|do|does|can|could)\b/i.test(t))
    return false;
  return /\s/.test(t);           // at least two words
}

async function judgeFact(fact) {
  try {
    const r = await fetch("https://openrouter.ai/api/v1/chat/completions", {
      method: "POST",
      headers: {
        Authorization: `Bearer ${process.env.OPENROUTER_API_KEY}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        model: MODEL_TEXT,
        messages: [
          { role: "system", content: judgePrompt },
          { role: "user", content: fact },
        ],
        // v2 used 60 here. The JSON got truncated, JSON.parse threw, the
        // function returned null and the caller said "Couldn't understand."
        // That is the whole "My dog's name is Max was rejected" bug.
        max_tokens: 150,
        temperature: 0,
      }),
    });
    const data = await r.json();
    const raw = data?.choices?.[0]?.message?.content || "";
    const jsonStr = raw.replace(/```json|```/g, "").trim();

    try {
      const parsed = JSON.parse(jsonStr);
      if (typeof parsed.save === "boolean") {
        return { save: parsed.save, fact: parsed.fact || fact };
      }
    } catch (_) {
      // Truncated or chatty output: pull the decision out with a regex
      // instead of throwing the whole thing away.
      const m = /"save"\s*:\s*(true|false)/i.exec(jsonStr);
      const f = /"fact"\s*:\s*"([^"]*)"/i.exec(jsonStr);
      if (m) return { save: m[1].toLowerCase() === "true", fact: f ? f[1] : fact };
    }
  } catch (err) {
    console.error("judgeFact error:", err.message);
  }
  // Never return null. Fall back to a heuristic so a flaky judge can't
  // block a legitimate fact.
  return { save: looksLikeFact(fact), fact };
}

// ---- shared command handling (used by both /api/chat and /api/converse) ----
async function handleCommand(normalized, original) {
  if (normalized === "reset all") {
    await pool.query("DELETE FROM memories").catch(() => {});
    return { handled: true, reply: "Memory cleared" };
  }
  if (normalized === "remember") {
    return { handled: true, reply: "MEMORY_MODE" };
  }
  if (normalized.startsWith("remember ")) {
    let fact = cleanText(original.replace(/^\s*remember[\s.,!?:;]+/i, ""))
      .replace(/[.,!?;:]+$/, "")
      .trim();
    if (!fact) return { handled: true, reply: "Nothing to remember." };

    const decision = await judgeFact(fact);
    if (!decision.save) return { handled: true, reply: "Say a real fact." };

    const finalFact = cleanText(decision.fact || fact);
    if (!finalFact) return { handled: true, reply: "Nothing to remember." };
    if (await factExists(finalFact)) return { handled: true, reply: "Already know that." };

    await saveMemory(finalFact);
    console.log("Saved:", finalFact);
    return { handled: true, reply: "Got it, saved." };
  }
  return { handled: false };
}

// ========= ROUTES =========
app.get("/", (req, res) => res.send("Luna AI Server v3"));
app.get("/health", (req, res) => res.send("OK"));

// ---------- transcription used for command detection ----------
async function transcribeBuffer(buf) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "stt-"));
  const wavPath = path.join(tempDir, "input.wav");
  try {
    fs.writeFileSync(wavPath, buf);
    const result = await groq.audio.transcriptions.create({
      file: fs.createReadStream(wavPath),
      model: "whisper-large-v3-turbo",
      response_format: "text",
      language: "en",
      temperature: 0,
    });
    const t = cleanText(typeof result === "string" ? result : result?.text || "");
    const n = t.toLowerCase().trim();
    return !n || WHISPER_HALLUCINATIONS.has(n) ? "" : t;
  } finally {
    try { fs.rmSync(tempDir, { recursive: true, force: true }); } catch {}
  }
}

// ---------- the audio-to-audio call ----------
// transcriptHint: Whisper's transcript of the SAME clip. gpt-audio-mini has
// its own, weaker internal speech understanding and will confidently answer
// a DIFFERENT question (sometimes in a different language) when the audio
// is quiet -- observed live: Whisper correctly heard "What is the color of
// sun?" while the audio model answered an unrelated question about
// "galaxy fan" in Bengali. Passing the accurate transcript as text grounds
// the model in what was actually said, instead of relying only on its own
// audio perception.
async function askAudioModel(wavBuffer, systemPrompt, transcriptHint) {
  // Keep the Whisper hint in the SYSTEM prompt so the model does not speak
  // phrases like "I'll trust the transcript" out loud.
  let sys = systemPrompt;
  if (transcriptHint) {
    sys +=
      `\n\nThe user just said (accurate speech-to-text): "${transcriptHint}". ` +
      `Answer THAT request. Ignore any different words you think you hear in the audio.`;
  }

  const userContent = [
    {
      type: "input_audio",
      input_audio: { data: wavBuffer.toString("base64"), format: "wav" },
    },
  ];

  const r = await fetch("https://openrouter.ai/api/v1/chat/completions", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${process.env.OPENROUTER_API_KEY}`,
      "Content-Type": "application/json",
      "X-Title": "Luna ESP32 Voice Assistant",
    },
    body: JSON.stringify({
      model: MODEL_AUDIO,
      modalities: ["text", "audio"],
      audio: { voice: AUDIO_VOICE, format: "pcm16" },
      // OpenRouter REQUIRES streaming for audio output. Without this the
      // request is rejected with 400 "Audio output requires stream: true"
      // and the model returns no audio, which is what forced the ESP32 back
      // onto the legacy stt/chat/tts pipeline.
      stream: true,
      messages: [
        { role: "system", content: sys },
        { role: "user", content: userContent },
      ],
      // 320 is a middle ground: 220 made answers too short/hedgy, 400 let
      // replies run 12-19s and starve the ESP32 playback buffer.
      max_tokens: 320,
      temperature: 0.7,
    }),
  });

  if (!r.ok) {
    const body = await r.text().catch(() => "");
    console.error(`audio model HTTP ${r.status}:`, body.slice(0, 400));
    return null;
  }

  // ---- Server-Sent Events: audio arrives as incremental chunks ----
  // IMPORTANT: accumulate the base64 as a STRING and decode ONCE at the end.
  // Chunk boundaries are not guaranteed to be 4-character aligned, so
  // base64-decoding each chunk separately corrupts the PCM stream.
  let b64        = "";
  let transcript = "";
  let text       = "";
  let carry      = "";
  let streamErr  = null;

  const reader  = r.body.getReader();
  const decoder = new TextDecoder();

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    carry += decoder.decode(value, { stream: true });

    let nl;
    while ((nl = carry.indexOf("\n")) >= 0) {
      const line = carry.slice(0, nl).trim();
      carry = carry.slice(nl + 1);

      if (!line || line.startsWith(":")) continue;      // keepalive / comment
      if (!line.startsWith("data:")) continue;

      const payload = line.slice(5).trim();
      if (payload === "[DONE]") continue;

      let j;
      try { j = JSON.parse(payload); } catch { continue; }

      if (j.error) { streamErr = j.error; continue; }

      const d = j.choices?.[0]?.delta;
      if (!d) continue;
      if (d.audio?.data)       b64        += d.audio.data;
      if (d.audio?.transcript) transcript += d.audio.transcript;
      if (typeof d.content === "string") text += d.content;
    }
  }

  if (streamErr) {
    console.error("audio model stream error:", JSON.stringify(streamErr).slice(0, 400));
    return null;
  }
  if (!b64) {
    console.error("audio model returned no audio (0 audio chunks in stream)");
    return null;
  }

  return {
    pcm: Buffer.from(b64, "base64"),      // PCM16 mono @ MODEL_RATE (24 kHz)
    transcript: cleanText(transcript || text || ""),
  };
}

// Build a 16 kHz WAV from text using the legacy gTTS path. Only used for
// short command confirmations ("Got it, saved."), never on the hot path.
function gttsWav(text) {
  return new Promise((resolve, reject) => {
    const ff = spawn(ffmpegPath, [
      "-y", "-i", "pipe:0",
      "-ar", String(DEVICE_RATE), "-ac", "1",
      "-filter:a", "loudnorm=I=-14:TP=-1.5:LRA=11",
      "-acodec", "pcm_s16le", "-f", "wav", "pipe:1",
    ]);
    const chunks = [];
    gtts.stream(text).pipe(ff.stdin);
    ff.stdout.on("data", (c) => chunks.push(c));
    ff.stdout.on("end", () => resolve(Buffer.concat(chunks)));
    ff.on("error", reject);
    ff.stderr.on("data", () => {});
  });
}

// =====================================================================
//  FAST PATH:  audio in  ->  audio out,  one upstream call
// =====================================================================
app.post(
  "/api/converse",
  express.raw({
    type: ["audio/wav", "audio/x-wav", "application/octet-stream"],
    limit: "12mb",
  }),
  async (req, res) => {
    const t0 = Date.now();
    try {
      const audio = Buffer.isBuffer(req.body) ? req.body : Buffer.from(req.body || []);
      if (!audio.length) return res.status(400).send("No audio");
      if (!process.env.OPENROUTER_API_KEY) return res.status(500).send("Missing OPENROUTER_API_KEY");

      const systemPrompt = await buildSystemPrompt();

      // Whisper now runs FIRST, not in parallel. Groq turbo is ~0.3s on a
      // short clip, so this adds negligible latency, and its transcript is
      // used for two things: command detection (as before) AND as a
      // grounding hint passed into the audio model so it stops answering
      // the wrong question on quiet/noisy audio (see askAudioModel).
      const transcript = await transcribeBuffer(audio).catch((e) => {
        console.error("stt error:", e.message);
        return "";
      });
      const tSTT = Date.now() - t0;

      // Commands bypass the audio model entirely and use the text path +
      // gTTS -- no point spending an audio-model call on "remember".
      const cmd = await handleCommand(normalizeCommand(transcript), transcript);
      if (cmd.handled) {
        console.log(`[converse] command "${transcript}" -> ${cmd.reply}  (${tSTT}ms)`);
        if (cmd.reply === "MEMORY_MODE") {
          res.setHeader("X-Reply", "MEMORY_MODE");
          res.setHeader("X-Transcript", encodeURIComponent(transcript));
          return res.status(204).end();          // firmware switches to memory mode
        }
        const wav = await gttsWav(cmd.reply);
        res.setHeader("Content-Type", "audio/wav");
        res.setHeader("X-Reply", encodeURIComponent(cmd.reply));
        res.setHeader("X-Transcript", encodeURIComponent(transcript));
        res.setHeader("Content-Length", wav.length);
        return res.end(wav);
      }

      const audioReply = await askAudioModel(audio, systemPrompt, transcript).catch((e) => {
        console.error("audio model error:", e.message);
        return null;
      });
      const tUpstream = Date.now() - t0;
      if (!audioReply) return res.status(502).send("Audio model failed");

      const pcm16k = normalizePcm16(
        resamplePcm16(audioReply.pcm, MODEL_RATE, DEVICE_RATE),
        22000,
      );
      const wav = Buffer.concat([wavHeader(pcm16k.length, DEVICE_RATE), pcm16k]);

      console.log(
        `[converse] heard="${transcript}" -> "${audioReply.transcript}"  ` +
        `stt=${tSTT}ms upstream=${tUpstream}ms total=${Date.now() - t0}ms ` +
        `audio=${(pcm16k.length / (DEVICE_RATE * 2)).toFixed(1)}s ${Math.round(wav.length / 1024)}KB`,
      );

      res.setHeader("Content-Type", "audio/wav");
      res.setHeader("X-Reply", encodeURIComponent(audioReply.transcript.slice(0, 180)));
      res.setHeader("X-Transcript", encodeURIComponent(transcript.slice(0, 180)));
      res.setHeader("Content-Length", wav.length);
      res.end(wav);
    } catch (err) {
      console.error("CONVERSE ERROR:", err.message);
      res.status(500).send("Server error");
    }
  },
);

// =====================================================================
//  LEGACY PATH — unchanged behaviour, kept as the firmware's fallback
// =====================================================================
app.post("/api/chat", async (req, res) => {
  try {
    const userMessage = cleanText(req.body?.message || "");
    if (!userMessage) return res.status(400).send("Empty message");

    const normalized = normalizeCommand(userMessage);
    if (!normalized) return res.send("");

    const cmd = await handleCommand(normalized, userMessage);
    if (cmd.handled) return res.send(cmd.reply);

    const systemPrompt = await buildSystemPrompt();
    const response = await fetch("https://openrouter.ai/api/v1/chat/completions", {
      method: "POST",
      headers: {
        Authorization: `Bearer ${process.env.OPENROUTER_API_KEY}`,
        "Content-Type": "application/json",
        "X-Title": "Luna ESP32 Voice Assistant",
      },
      body: JSON.stringify({
        model: MODEL_TEXT,
        messages: [
          { role: "system", content: systemPrompt },
          { role: "user", content: userMessage },
        ],
        max_tokens: REPLY_TOKENS,
        temperature: 0.7,
      }),
    });

    const data = await response.json().catch(() => ({}));
    let reply = "AI error";
    if (response.ok && data?.choices?.[0]?.message?.content) {
      reply = data.choices[0].message.content;
    } else if (data?.error?.message) {
      reply = data.error.message;
    }
    res.send(cleanText(reply) || "AI error");
  } catch (err) {
    console.error("CHAT ERROR:", err.message);
    res.status(500).send("Server error");
  }
});

app.post("/api/tts", async (req, res) => {
  try {
    const text = cleanText(req.body?.text || "") || "Hello";
    const wav = await gttsWav(text);
    res.setHeader("Content-Type", "audio/wav");
    res.setHeader("Content-Length", wav.length);
    res.end(wav);
  } catch (err) {
    console.error("TTS ERROR:", err.message);
    if (!res.headersSent) res.status(500).send("TTS Error");
  }
});

app.post(
  "/api/stt",
  express.raw({
    type: ["audio/wav", "audio/x-wav", "application/octet-stream"],
    limit: "12mb",
  }),
  async (req, res) => {
    try {
      if (!process.env.GROQ_API_KEY) return res.status(500).send("Missing GROQ_API_KEY");
      const audioBuffer = Buffer.isBuffer(req.body) ? req.body : Buffer.from(req.body || []);
      if (!audioBuffer.length) return res.status(400).send("No audio");
      res.send(await transcribeBuffer(audioBuffer));
    } catch (err) {
      console.error("STT ERROR:", err.message);
      res.status(500).send("STT Error");
    }
  },
);

// ========= MEMORY API =========
app.get("/api/memory", async (req, res) => res.json(await loadMemory()));

app.post("/api/memory", async (req, res) => {
  const content = cleanText(req.body?.content || "");
  if (!content) return res.status(400).json({ error: "Empty content" });
  await saveMemory(content);
  res.json({ ok: true });
});

app.delete("/api/memory/:id", async (req, res) => {
  const id = parseInt(req.params.id, 10);
  if (isNaN(id)) return res.status(400).json({ error: "Bad id" });
  const ok = await deleteMemory(id);
  if (!ok) return res.status(404).json({ error: "Not found" });
  res.json({ ok: true });
});

// ========= MEMORY PAGE =========
app.get("/memory", (req, res) => {
  res.setHeader("Content-Type", "text/html; charset=utf-8");
  res.send(`<!doctype html>
<html><head><meta charset="utf-8"/>
<title>Luna Memory</title>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<style>
  body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:720px;margin:24px auto;padding:0 16px;background:#0e0e10;color:#eee}
  h1{font-size:22px;margin:0 0 16px}
  .row{display:flex;gap:8px;margin:8px 0;align-items:center}
  .row span{flex:1;padding:10px;background:#1a1a1d;border:1px solid #333;border-radius:8px}
  input[type=text]{flex:1;padding:10px;border-radius:8px;border:1px solid #333;background:#1a1a1d;color:#eee;font-size:15px}
  button{padding:10px 14px;border-radius:8px;border:0;background:#5b8cff;color:#fff;cursor:pointer;font-size:14px}
  button.del{background:#c0392b}
  button.add{background:#27ae60}
  .empty{opacity:.6;font-style:italic;margin-top:20px}
</style></head>
<body>
<h1>Luna's Memory</h1>
<div id="list"></div>
<div class="row">
  <input type="text" id="newFact" placeholder="Add a new fact..."/>
  <button class="add" onclick="addFact()">Add</button>
</div>
<script>
async function load(){
  const r = await fetch('/api/memory');
  const data = await r.json();
  const list = document.getElementById('list');
  if(!data.length){ list.innerHTML='<div class="empty">No memories saved yet.</div>'; return; }
  list.innerHTML='';
  data.forEach(m=>{
    const row=document.createElement('div'); row.className='row';
    const span=document.createElement('span'); span.textContent=m.content;
    const del=document.createElement('button'); del.className='del'; del.textContent='X';
    del.onclick=async()=>{ await fetch('/api/memory/'+m.id,{method:'DELETE'}); load(); };
    row.appendChild(span); row.appendChild(del); list.appendChild(row);
  });
}
async function addFact(){
  const inp=document.getElementById('newFact');
  const v=inp.value.trim(); if(!v) return;
  await fetch('/api/memory',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({content:v})});
  inp.value=''; load();
}
load();
</script>
</body></html>`);
});

// ========= START =========
initDb().then(() => {
  app.listen(PORT, "0.0.0.0", () => {
    console.log(`Luna server v3 on ${PORT}  audio-model=${MODEL_AUDIO}`);
  });
});
