// src/services.ts — Minimal & push-based service layer for React TS + MQTT
// Works with Vite + Tailwind v4 + Radix/shadcn. No external state libs.

import mqtt, { type MqttClient, type IClientOptions } from "mqtt";
import { useEffect, useMemo, useState } from "react";

// =============== Types ===============
export type MqttConnState = "disconnected" | "reconnecting" | "connected";

export type BrokerConfig = {
  /** Browser must use WebSocket endpoint, e.g. wss://host:8884/mqtt */
  url: string;
  username?: string;
  password?: string;
  /** Root topic, e.g. "esp32" */
  root: string;
};

export type StatusPayload = {
  device?: string;
  status?: "online" | "offline";
  uptime?: number;
  wifi_rssi?: number;
  free_heap?: number;
  timestamp?: number;
};

export type TempPayload = {
  temperature?: number | null; // new key
  humidity?: number | null; // new key
  Temperature?: number | null; // legacy
  Humidity?: number | null; // legacy
  autoMode?: boolean;
  maxTemp?: number;
  unit?: string;
  timestamp?: number;
};

export type OutputsPayload = {
  outputs?: { out1?: boolean; out2?: boolean };
  timestamp?: number;
};

export type DeviceCommand =
  | { command: "setOutput"; value: { output: 1 | 2; state: boolean } }
  | { command: "getStatus" }
  | { command: "restart" }
  | {
      command: "setMode";
      value: { auto: boolean; threshold: number; hysteresis?: number };
    };

export type TelemetryPoint = { time: string; value: number | null };
export type OutputsState = { out1: boolean; out2: boolean };

// ==== UI settings ====
export type UiSettings = {
  autoReconnect: boolean; // tự động kết nối lại khi có config
  maxTemp: number; // ngưỡng cảnh báo UI
  mode: "manual" | "auto";
  hysteresis?: number; // độ trễ °C để tránh nhấp nháy (mặc định 0.5)
};

// =============== LocalStorage Settings ===============
const KEY_BROKER = "esp32.mqtt.broker";
export const DEFAULT_BROKER: BrokerConfig = {
  url: "",
  root: "esp32",
  username: "",
  password: "",
};

export const Settings = {
  load(): BrokerConfig | null {
    try {
      const s = localStorage.getItem(KEY_BROKER);
      return s ? (JSON.parse(s) as BrokerConfig) : null;
    } catch {
      return null;
    }
  },
  save(cfg: BrokerConfig) {
    try {
      localStorage.setItem(KEY_BROKER, JSON.stringify(cfg));
    } catch {}
  },
  clear() {
    try {
      localStorage.removeItem(KEY_BROKER);
    } catch {}
  },
};

// UI settings persist
const KEY_UI = "esp32.ui.settings";
export const SettingsUI = {
  load(): UiSettings | null {
    try {
      const s = localStorage.getItem(KEY_UI);
      return s ? (JSON.parse(s) as UiSettings) : null;
    } catch {
      return null;
    }
  },
  save(u: UiSettings) {
    try {
      localStorage.setItem(KEY_UI, JSON.stringify(u));
    } catch {}
  },
  clear() {
    try {
      localStorage.removeItem(KEY_UI);
    } catch {}
  },
};

// =============== Topics & helpers ===============
export type Topics = { status: string; temp: string; out: string; cmd: string };
export function buildTopics(root: string): Topics {
  const r = root.replace(/\/+$/, "");
  return {
    status: `${r}/status`,
    temp: `${r}/temperature`,
    out: `${r}/outputs`,
    cmd: `${r}/commands`,
  };
}
const fmtTime = (ms: number) => new Date(ms).toLocaleTimeString();

class RingBuffer<T> {
  private a: T[] = [];
  private cap: number;
  constructor(cap: number) {
    this.cap = cap;
  }
  push(v: T) {
    this.a.push(v);
    if (this.a.length > this.cap) this.a.splice(0, this.a.length - this.cap);
  }
  snapshot() {
    return [...this.a];
  }
  clear() {
    this.a = [];
  }
}

// Small, typed event emitter
type Handler<T> = (p: T) => void;
class Emitter {
  private m = new Map<string, Set<Function>>();
  on<T>(evt: string, fn: Handler<T>) {
    (this.m.get(evt) ?? this.m.set(evt, new Set()).get(evt))!.add(fn as any);
    return () => this.off(evt, fn as any);
  }
  off(evt: string, fn: Function) {
    this.m.get(evt)?.delete(fn);
  }
  emit<T>(evt: string, p: T) {
    this.m.get(evt)?.forEach((fn) => (fn as Handler<T>)(p));
  }
}

// =============== MQTT wrapper ===============
export class MqttService {
  private client: MqttClient | null = null;

  private connecting: Promise<void> | null = null;

  private em = new Emitter();
  private _state: MqttConnState = "disconnected";
  private _id = `webui-${Math.random().toString(16).slice(2)}`;

  get state() {
    return this._state;
  }
  get clientId() {
    return this._id;
  }
  onState(cb: Handler<MqttConnState>) {
    return this.em.on("state", cb);
  }
  onMsg(cb: Handler<{ topic: string; text: string }>) {
    return this.em.on("msg", cb);
  }

  //   async connect(cfg: BrokerConfig) {
  //     await this.disconnect();
  //     const opts: { clientId?: string; username?: string; password?: string; clean?: boolean; reconnectPeriod?: number } = {
  //       clientId: this._id,
  //       username: cfg.username || undefined,
  //       password: cfg.password || undefined,
  //       clean: true,
  //       reconnectPeriod: 2000,
  //     };
  //     const c = mqtt.connect(cfg.url, opts as any);
  //     this.client = c;

  async connect(cfg: BrokerConfig) {
    // Guard: nếu đã có client hoặc đang kết nối thì không tạo thêm
    if (this.client) return;
    if (this.connecting) return this.connecting;
    const opts: IClientOptions = {
      clientId: this._id,
      username: cfg.username || undefined,
      password: cfg.password || undefined,
      clean: true,
      keepalive: 30,
      reconnectPeriod: 3000, // dịu lại tần suất reconnect
      connectTimeout: 5000,
      resubscribe: true, // đảm bảo tự resubscribe khi reconnect
    };
    const c = mqtt.connect(cfg.url, opts);
    this.client = c;
    this.setState("reconnecting");
    this.connecting = new Promise<void>((resolve) => {
      c.once("connect", () => {
        this.setState("connected");
        this.connecting = null;
        resolve();
      });
      c.once("error", () => {
        this.connecting = null; /* sẽ vào cycle reconnect */
      });
    });

    c.on("connect", () => this.setState("connected"));
    c.on("reconnect", () => this.setState("reconnecting"));
    c.on("close", () => this.setState("disconnected"));
    c.on("error", (e) => console.error("[MQTT]", e));
    c.on("message", (topic, payload) => {
      const text = new TextDecoder().decode(payload);
      this.em.emit("msg", { topic, text });
    });
    return this.connecting;
  }
  async disconnect() {
    // if (!this.client) return;
    // const c = this.client; this.client = null;
    if (!this.client) return;
    const c = this.client;
    this.client = null;
    this.connecting = null;
    await new Promise<void>((res) => c.end(true, {}, res as any));
    this.setState("disconnected");
  }
  subscribe(topics: string[]) {
    if (!this.client) throw new Error("MQTT not connected");
    this.client.subscribe(topics, (err) => {
      if (err) console.error("[MQTT subscribe]", err);
    });
  }
  publishJson(topic: string, obj: unknown, qos: 0 | 1 | 2 = 1) {
    if (!this.client) throw new Error("MQTT not connected");
    this.client.publish(topic, JSON.stringify(obj), { qos });
  }
  private setState(s: MqttConnState) {
    this._state = s;
    this.em.emit("state", s);
    this.em.emit("tick", undefined as any);
  }
}

// =============== Domain service (Device + Telemetry + Outputs) ===============
export type DeviceState = {
  deviceId: string;
  wifi: "connected" | "ap" | "offline" | "—";
  rssi: number | null;
  uptime: number | null;
  heap: number | null;
  lastStatusAt: number | null;
};

export class DashboardService {
  readonly mqtt = new MqttService();
  private em = new Emitter();

  private cfg: BrokerConfig;
  private topics: Topics;
  private ui: UiSettings = {
    autoReconnect: true,
    maxTemp: 80,
    mode: "manual",
    hysteresis: 0.5,
  };

  private device: DeviceState = {
    deviceId: "—",
    wifi: "—",
    rssi: null,
    uptime: null,
    heap: null,
    lastStatusAt: null,
  };
  private tempBuf = new RingBuffer<TelemetryPoint>(180); // ~3 phút nếu 1s/update
  private humBuf = new RingBuffer<TelemetryPoint>(180);
  private outs: OutputsState = { out1: false, out2: false };
  private busy = false;

  constructor(initial?: BrokerConfig) {
    const persisted = Settings.load();
    const uiSaved = SettingsUI.load();
    // this.cfg = initial ?? persisted ?? DEFAULT_BROKER;
    this.cfg = persisted ?? initial ?? DEFAULT_BROKER;
    // if (uiSaved) this.ui = uiSaved;
    if (uiSaved) this.ui = { ...this.ui, ...uiSaved }; // merge để giữ default cho field mới
    this.topics = buildTopics(this.cfg.root);

    this.mqtt.onMsg(({ topic, text }) => this.route(topic, text));
    this.mqtt.onState(() => this.emitChange());
  }

  // ---- lifecycle ----
  async connect(cfg?: BrokerConfig) {
    if (cfg) {
      await this.setConfig(cfg, { persist: true, reconnect: false });
    }
    await this.mqtt.connect(this.cfg);
    this.mqtt.subscribe([
      this.topics.status,
      this.topics.temp,
      this.topics.out,
    ]);
    this.requestStatus();

    if (this.ui.mode === "auto") {
      this.setMode(true, this.ui.maxTemp, this.ui.hysteresis);
    }
  }
  async disconnect() {
    await this.mqtt.disconnect();
  }

  // ---- commands ----
  requestStatus() {
    const cmd: DeviceCommand = { command: "getStatus" };
    this.safePub(this.topics.cmd, cmd);
  }
  async toggleOutput(ch: 1 | 2, want?: boolean) {
    const next =
      typeof want === "boolean"
        ? want
        : ch === 1
        ? !this.outs.out1
        : !this.outs.out2;
    const cmd: DeviceCommand = {
      command: "setOutput",
      value: { output: ch, state: next },
    };
    try {
      this.busy = true;
      this.emitChange();
      this.safePub(this.topics.cmd, cmd);
      if (ch === 1) this.outs.out1 = next;
      else this.outs.out2 = next;
    } finally {
      this.busy = false;
      this.emitChange();
    }
  }
  clearTelemetry() {
    this.tempBuf.clear();
    this.humBuf.clear();
    this.emitChange();
  }

  setMode(auto: boolean, threshold: number, hysteresis?: number) {
    const cmd: DeviceCommand = {
      command: "setMode",
      value: { auto, threshold, hysteresis },
    };
    this.safePub(this.topics.cmd, cmd);
  }

  // ---- config ----
  //   get config(): BrokerConfig { return { ...this.cfg }; }
  get config(): BrokerConfig {
    return this.cfg;
  }
  async setConfig(
    next: BrokerConfig,
    opts: { persist?: boolean; reconnect?: boolean } = {
      persist: true,
      reconnect: true,
    }
  ) {
    // this.cfg = next; this.topics = buildTopics(next.root);
    // if (opts.persist !== false) Settings.save(next);
    // if (opts.reconnect) { await this.disconnect(); await this.connect(next); } else { this.emitChange(); }
    const changed = JSON.stringify(next) !== JSON.stringify(this.cfg);
    this.cfg = next;
    this.topics = buildTopics(next.root);
    if (opts.persist !== false) Settings.save(next);
    if (opts.reconnect && changed) {
      await this.disconnect();
      await this.connect(next);
    } else {
      this.emitChange();
    }
  }

  // ---- UI settings ----
  get uiSnap(): UiSettings {
    return { ...this.ui };
  }
  setUiSettings(next: UiSettings, persist = true) {
    // this.ui = { ...this.ui, ...next };
    const prev = this.ui;
    this.ui = { ...this.ui, ...next };
    if (this.ui.hysteresis == null) this.ui.hysteresis = 0.5;
    if (persist) SettingsUI.save(this.ui);
    this.emitChange();

    //  nếu vừa bật autoReconnect và đã có URL, tự connect
    if (
      this.ui.autoReconnect &&
      this.cfg.url &&
      this.mqtt.state === "disconnected"
    ) {
      this.connect().catch(() => {});
    }

    // Nếu user đổi chế độ/nguỡng/hysteresis → đẩy xuống ESP32 ngay (nếu đang kết nối)
    if (
      prev.mode !== this.ui.mode ||
      prev.maxTemp !== this.ui.maxTemp ||
      prev.hysteresis !== this.ui.hysteresis
    ) {
      if (this.mqtt.state === "connected") {
        this.setMode(
          this.ui.mode === "auto",
          this.ui.maxTemp,
          this.ui.hysteresis
        );
      }
    }
  }

  // ---- snapshots ----
  get conn(): MqttConnState {
    return this.mqtt.state;
  }
  get deviceSnap(): DeviceState {
    return { ...this.device };
  }
  get temperature(): TelemetryPoint[] {
    return this.tempBuf.snapshot();
  }
  get humidity(): TelemetryPoint[] {
    return this.humBuf.snapshot();
  }
  get outputs(): OutputsState {
    return { ...this.outs };
  }
  get isBusy() {
    return this.busy;
  }

  getSnapshot() {
    return {
      conn: this.conn,
      device: this.deviceSnap,
      temp: this.temperature,
      hum: this.humidity,
      outs: this.outputs,
      busy: this.isBusy,
      config: this.config,
      ui: this.uiSnap,
    } as const;
  }

  // ---- subscribe for React ----
  subscribe(fn: () => void) {
    return this.em.on("update", fn);
  }
  private emitChange() {
    this.em.emit("update", undefined as any);
  }

  // ---- router ----
  private route(topic: string, text: string) {
    try {
      const j = JSON.parse(text) as any;
      const now = Date.now();
      if (topic === this.topics.status) {
        const s = j as StatusPayload;
        if (s.device) this.device.deviceId = s.device;
        if (typeof s.wifi_rssi === "number") this.device.rssi = s.wifi_rssi;
        if (typeof s.uptime === "number") this.device.uptime = s.uptime;
        if (typeof s.free_heap === "number") this.device.heap = s.free_heap;
        if (s.status)
          this.device.wifi = s.status === "online" ? "connected" : "offline";
        this.device.lastStatusAt = s.timestamp ?? now;
      } else if (topic === this.topics.temp) {
        const t = j as TempPayload;
        const ts = t.timestamp ?? now;
        const T = t.temperature ?? t.Temperature ?? null;
        const H = t.humidity ?? t.Humidity ?? null;
        if (T !== null) this.tempBuf.push({ time: fmtTime(ts), value: T });
        if (H !== null) this.humBuf.push({ time: fmtTime(ts), value: H });
      } else if (topic === this.topics.out) {
        const o = (j as OutputsPayload).outputs ?? {};
        if (typeof o.out1 === "boolean") this.outs.out1 = o.out1;
        if (typeof o.out2 === "boolean") this.outs.out2 = o.out2;
      }
      this.emitChange();
    } catch (e) {
      console.warn("[router] bad JSON", e, { topic, text });
    }
  }

  // ---- helpers ----
  private safePub(topic: string, obj: unknown) {
    try {
      this.mqtt.publishJson(topic, obj, 1);
    } catch (e) {
      console.warn("publish failed", e);
    }
  }
}

// =============== React hook (push-based, no polling) ===============
export function useDashboard(initial?: BrokerConfig) {
  const svc = useMemo(() => new DashboardService(initial), []);
  const [snap, setSnap] = useState(svc.getSnapshot());

  useEffect(() => {
    const off = svc.subscribe(() => setSnap(svc.getSnapshot()));
    const s = svc.getSnapshot();
    // chỉ auto-connect khi autoReconnect bật và có URL
    if (s.config.url && s.ui.autoReconnect) {
      svc.connect().catch(() => {
        /* ignore */
      });
    }
    return () => {
      off();
      svc.disconnect();
    };
  }, [svc]);

  return {
    svc,
    ...snap,
    updateConfig: (
      next: BrokerConfig,
      opts?: { persist?: boolean; reconnect?: boolean }
    ) => svc.setConfig(next, opts),
    updateUi: (next: UiSettings, persist?: boolean) =>
      svc.setUiSettings(next, persist),
    clearConfig: () => {
      Settings.clear();
      SettingsUI.clear();
    },
  } as const;
}
