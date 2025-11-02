import * as React from "react";
import { useEffect, useState } from "react";
import {
  useDashboard,
  type BrokerConfig,
  type UiSettings,
} from "@/services/Services"; // 👈 sửa path
import * as Switch from "@radix-ui/react-switch";
import * as Label from "@radix-ui/react-label";
import * as Separator from "@radix-ui/react-separator";
import {
  Wifi,
  Radio,
  Rss,
  Cpu,
  Thermometer,
  Droplets,
  RefreshCcw,
  Power,
  Link,
  Settings2,
} from "lucide-react";
import {
  ResponsiveContainer,
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
} from "recharts";

// -------- Small UI helpers (Tailwind only) --------
function Pill({
  icon,
  label,
  value,
  tone,
}: {
  icon: React.ReactNode;
  label: string;
  value: React.ReactNode;
  tone?: "ok" | "warn" | "err";
}) {
  const toneCls =
    tone === "ok"
      ? "text-emerald-400"
      : tone === "warn"
      ? "text-amber-400"
      : tone === "err"
      ? "text-red-400"
      : "text-slate-300";
  return (
    <div className="inline-flex items-center gap-2 rounded-full border border-slate-700 bg-slate-900/60 px-3 py-1">
      <span className="opacity-80">{icon}</span>
      <span className="text-xs text-slate-400">{label}</span>
      <span className={`text-sm font-medium ${toneCls}`}>{value}</span>
    </div>
  );
}

function Metric({
  icon,
  label,
  value,
  sub,
}: {
  icon: React.ReactNode;
  label: string;
  value: React.ReactNode;
  sub?: string;
}) {
  return (
    <div className="rounded-2xl border border-slate-800 bg-slate-900/60 p-4 flex items-center gap-3">
      <div className="rounded-xl border border-slate-700 bg-slate-800/70 p-2">
        {icon}
      </div>
      <div>
        <div className="text-slate-400 text-xs">{label}</div>
        <div className="text-lg font-semibold">{value}</div>
        {sub && <div className="text-slate-500 text-xs">{sub}</div>}
      </div>
    </div>
  );
}

function ChartCard({
  title,
  unit,
  color,
  data,
}: {
  title: string;
  unit: string;
  color: string;
  data: { time: string; value: number | null }[];
}) {
  const last = data.at(-1)?.value ?? null;
  return (
    <div className="rounded-2xl border border-slate-800 bg-slate-900/60">
      <div className="px-4 pt-4 text-base font-semibold">
        {title}{" "}
        {last != null ? (
          <span className="text-slate-400 text-sm">
            ({Number(last).toFixed(1)} {unit})
          </span>
        ) : null}
      </div>
      <div className="p-4">
        <div className="h-60 w-full">
          <ResponsiveContainer width="100%" height="100%">
            <LineChart
              data={data}
              margin={{ top: 8, right: 12, bottom: 4, left: 0 }}
            >
              <CartesianGrid strokeDasharray="3 3" stroke="#233047" />
              <XAxis
                dataKey="time"
                tick={{ fill: "#9aa4b2", fontSize: 12 }}
                minTickGap={32}
              />
              <YAxis
                tick={{ fill: "#9aa4b2", fontSize: 12 }}
                domain={["auto", "auto"]}
              />
              <Tooltip
                contentStyle={{
                  background: "#0b1326",
                  border: "1px solid #233047",
                  color: "#e6eaf0",
                }}
              />
              <Line
                type="monotone"
                dataKey="value"
                stroke={color}
                dot={false}
                strokeWidth={2}
                isAnimationActive={false}
              />
            </LineChart>
          </ResponsiveContainer>
        </div>
      </div>
    </div>
  );
}

const fmtUptime = (s?: number | null) => {
  if (!s && s !== 0) return "—";
  const h = Math.floor(s / 3600),
    m = Math.floor((s % 3600) / 60);
  return h ? `${h}h ${m}m` : `${m}m`;
};

const fmtBytes = (n?: number | null) =>
  typeof n === "number" ? `${(n / 1024).toFixed(0)} KB` : "—";

function normalizeWssUrl(raw: string): string {
  let s = (raw || "").trim();
  if (!s) return s;
  if (/^https?:\/\//i.test(s)) s = s.replace(/^https?/i, "wss"); // https -> wss
  if (!/^wss?:\/\//i.test(s)) s = "wss://" + s.replace(/^\/+/, "");
  try {
    const u = new URL(s);
    if (/\.hivemq\.cloud$/i.test(u.hostname)) {
      if (!u.port) u.port = "8884";
      if (!/\/mqtt\/?$/i.test(u.pathname || "")) {
        u.pathname =
          (u.pathname?.endsWith("/") ? u.pathname : u.pathname + "/") + "mqtt";
      }
    }
    return u.toString();
  } catch {
    return s;
  }
}

export default function Dashboard() {
  const {
    conn,
    device,
    temp,
    hum,
    outs,
    busy,
    svc,
    config,
    updateConfig,
    clearConfig,
    ui,
    updateUi,
  } = useDashboard();
  // useDashboard({ url: "", root: "esp32", username: "", password: "" });

  const [form, setForm] = useState<BrokerConfig>(config);
  const [uiForm, setUiForm] = useState<UiSettings>(ui);
  const [showSettings, setShowSettings] = useState(false);
  const mqttTone =
    conn === "connected" ? "ok" : conn === "reconnecting" ? "warn" : "err";

  useEffect(() => {
    if (showSettings) setForm(config);
  }, [showSettings]);
  useEffect(() => {
    if (showSettings) setUiForm(ui);
  }, [showSettings]);

  return (
    <div className="min-h-screen bg-gradient-to-b from-slate-950 to-slate-900 text-slate-100">
      <div className="mx-auto max-w-6xl p-4">
        {/* Header / Status bar */}
        <div className="sticky top-0 z-10 mb-4 rounded-2xl border border-slate-800/80 bg-slate-900/70 backdrop-blur">
          <div className="flex flex-wrap items-center gap-2 px-4 py-3">
            <span className="rounded-full border border-slate-700 bg-slate-900/60 px-3 py-1 text-xs text-slate-300">
              ESP32 MQTT Dashboard
            </span>
            <div className="ms-auto flex flex-wrap items-center gap-2">
              <Pill
                icon={<Radio size={16} />}
                label="MQTT"
                value={conn}
                tone={mqttTone as any}
              />
              <Pill
                icon={<Wifi size={16} />}
                label="Wi-Fi"
                value={device.wifi}
              />
              <Pill
                icon={<Cpu size={16} />}
                label="Device"
                value={device.deviceId}
              />
              <Pill
                icon={<Rss size={16} />}
                label="RSSI"
                value={device.rssi != null ? `${device.rssi} dBm` : "—"}
              />
              <button
                className="ms-2 inline-flex items-center gap-2 rounded-lg border border-slate-700 bg-slate-800/70 px-3 py-1.5 text-sm hover:bg-slate-800"
                onClick={() => setShowSettings(true)}
                title="Cài đặt"
              >
                <Settings2 size={16} /> Cài đặt
              </button>
            </div>
          </div>
        </div>

        {/* Metrics */}
        <div className="grid grid-cols-1 gap-4 md:grid-cols-4">
          <Metric
            icon={<Thermometer className="text-sky-400" />}
            label="Nhiệt độ"
            value={<>{temp.at(-1)?.value ?? "—"} °C</>}
            sub="Realtime"
          />
          <Metric
            icon={<Droplets className="text-emerald-400" />}
            label="Độ ẩm"
            value={<>{hum.at(-1)?.value ?? "—"} %</>}
            sub="Realtime"
          />
          <Metric
            icon={<Cpu className="text-fuchsia-400" />}
            label="Free heap"
            value={fmtBytes(device.heap)}
            sub="ESP32"
          />
          <Metric
            icon={<Link className="text-amber-400" />}
            label="Uptime"
            value={fmtUptime(device.uptime)}
            sub="Thiết bị"
          />
        </div>

        {/* Charts */}
        <div className="mt-4 grid grid-cols-1 gap-4 md:grid-cols-2">
          <ChartCard
            title="Biểu đồ nhiệt độ"
            unit="°C"
            color="#4f8cff"
            data={temp}
          />
          <ChartCard
            title="Biểu đồ độ ẩm"
            unit="%"
            color="#22c55e"
            data={hum}
          />
        </div>

        {/* Outputs */}
        <div className="mt-4 rounded-2xl border border-slate-800 bg-slate-900/60">
          <div className="flex items-center justify-between p-4">
            <div className="flex items-center gap-2 text-base font-semibold">
              <Power size={18} /> Điều khiển Outputs
            </div>
            <div className="flex gap-2">
              {conn !== "connected" ? (
                <button
                  className="rounded-lg border border-slate-700 bg-slate-800/70 px-3 py-2 text-sm"
                  onClick={() => svc.connect()}
                >
                  Kết nối
                </button>
              ) : (
                <button
                  className="rounded-lg border border-slate-700 bg-slate-800/70 px-3 py-2 text-sm"
                  onClick={() => svc.disconnect()}
                >
                  Ngắt
                </button>
              )}
              <button
                className="rounded-lg border border-slate-700 bg-slate-800/70 px-3 py-2 text-sm"
                onClick={() => svc.requestStatus()}
              >
                <span className="inline-flex items-center gap-1">
                  <RefreshCcw size={16} /> Trạng thái
                </span>
              </button>
            </div>
          </div>
          <Separator.Root className="h-px w-full bg-slate-800" />
          <div className="p-4 flex flex-wrap items-center gap-8">
            <div className="flex items-center gap-3">
              <Switch.Root
                id="out1"
                checked={outs.out1}
                disabled={conn !== "connected" || busy || ui.mode === "auto"}
                onCheckedChange={(v) => svc.toggleOutput(1, Boolean(v))}
                className="relative h-6 w-11 cursor-pointer rounded-full bg-slate-700 data-[state=checked]:bg-emerald-500 outline-none"
              >
                <Switch.Thumb className="block h-5 w-5 translate-x-0.5 rounded-full bg-white transition-transform will-change-transform data-[state=checked]:translate-x-[22px]" />
              </Switch.Root>
              <Label.Root htmlFor="out1" className="select-none text-sm">
                OUT1:{" "}
                <span
                  className={`font-semibold ${
                    outs.out1 ? "text-emerald-400" : "text-slate-400"
                  }`}
                >
                  {outs.out1 ? "ON" : "OFF"}
                </span>
              </Label.Root>
            </div>
            <div className="flex items-center gap-3">
              <Switch.Root
                id="out2"
                checked={outs.out2}
                disabled={conn !== "connected" || busy}
                onCheckedChange={(v) => svc.toggleOutput(2, Boolean(v))}
                className="relative h-6 w-11 cursor-pointer rounded-full bg-slate-700 data-[state=checked]:bg-emerald-500 outline-none"
              >
                <Switch.Thumb className="block h-5 w-5 translate-x-0.5 rounded-full bg-white transition-transform will-change-transform data-[state=checked]:translate-x-[22px]" />
              </Switch.Root>
              <Label.Root htmlFor="out2" className="select-none text-sm">
                OUT2:{" "}
                <span
                  className={`font-semibold ${
                    outs.out2 ? "text-emerald-400" : "text-slate-400"
                  }`}
                >
                  {outs.out2 ? "ON" : "OFF"}
                </span>
              </Label.Root>
            </div>
          </div>
        </div>

        {/* Settings Modal (centered) */}
{/* Settings Modal (centered) */}
{showSettings && (
  <div
    className="fixed inset-0 z-40 flex items-center justify-center p-4"
    role="dialog"
    aria-modal="true"
  >
    {/* overlay */}
    <div
      className="absolute inset-0 bg-black/50"
      onClick={() => setShowSettings(false)}
    />

    {/* modal */}
    <div className="relative z-10 w-full max-w-[92vw] sm:max-w-lg md:max-w-2xl rounded-2xl border border-slate-800 bg-slate-900 shadow-2xl max-h-[85vh] overflow-y-auto">
      {/* header sticky */}
      <div className="sticky top-0 z-10 flex items-center justify-between border-b border-slate-800 px-4 py-3 bg-slate-900/95 backdrop-blur">
        <div className="text-base font-semibold">Cài đặt</div>
        <button
          className="rounded-md border border-slate-700 bg-slate-800/70 px-2 py-1 text-xs hover:bg-slate-800"
          onClick={() => setShowSettings(false)}
        >
          Đóng
        </button>
      </div>

      {/* content */}
      <div className="p-4 sm:p-6 space-y-6">
        {/* Nhóm 1: Kết nối MQTT */}
        <section className="rounded-xl border border-slate-800 bg-slate-900/60 p-4">
          <div className="mb-3 text-sm font-semibold">Kết nối MQTT</div>
          <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
            <div className="col-span-full">
              <label htmlFor="url" className="text-sm text-slate-300">
                WSS URL
              </label>
              <input
                id="url"
                className="mt-1 w-full min-w-0 rounded-lg border border-slate-700 bg-slate-900 px-3 py-2 text-sm outline-none focus:border-slate-500"
                value={form.url}
                onChange={(e) => setForm({ ...form, url: e.target.value })}
                placeholder="wss://host:8884/mqtt"
              />
              <div className="mt-1 text-xs text-slate-500">
                Preview: {form.url ? normalizeWssUrl(form.url) : "—"}
              </div>
            </div>

            <div className="sm:col-span-1">
              <label htmlFor="root" className="text-sm text-slate-300">
                Root Topic
              </label>
              <input
                id="root"
                className="mt-1 w-full min-w-0 rounded-lg border border-slate-700 bg-slate-900 px-3 py-2 text-sm outline-none focus:border-slate-500"
                value={form.root}
                onChange={(e) =>
                  setForm({ ...form, root: e.target.value || "esp32" })
                }
              />
            </div>
          </div>
        </section>

        {/* Nhóm 2: Chứng thực */}
        <section className="rounded-xl border border-slate-800 bg-slate-900/60 p-4">
         
          <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
            <div className="sm:col-span-1">
              <label htmlFor="user" className="text-sm text-slate-300">
                Username
              </label>
              <input
                id="user"
                className="mt-1 w-full min-w-0 rounded-lg border border-slate-700 bg-slate-900 px-3 py-2 text-sm outline-none focus:border-slate-500"
                value={form.username || ""}
                onChange={(e) =>
                  setForm({ ...form, username: e.target.value })
                }
              />
            </div>
            <div className="sm:col-span-1">
              <label htmlFor="pw" className="text-sm text-slate-300">
                Password
              </label>
              <input
                id="pw"
                type="password"
                className="mt-1 w-full min-w-0 rounded-lg border border-slate-700 bg-slate-900 px-3 py-2 text-sm outline-none focus:border-slate-500"
                value={form.password || ""}
                onChange={(e) =>
                  setForm({ ...form, password: e.target.value })
                }
              />
            </div>
          </div>
        </section>

        {/* Nhóm 3: Hành vi thiết bị */}
        <section className="rounded-xl border border-slate-800 bg-slate-900/60 p-4">
          <div className="mb-3 text-sm font-semibold">Thiết bị</div>

          {/* Hàng switch */}
          <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
            {/* Auto reconnect */}
            <div className="flex items-start sm:items-center gap-3">
              <Switch.Root
                id="auto"
                checked={uiForm.autoReconnect}
                onCheckedChange={(v) =>
                  setUiForm({ ...uiForm, autoReconnect: Boolean(v) })
                }
                className="relative h-6 w-11 cursor-pointer rounded-full bg-slate-700 data-[state=checked]:bg-emerald-500 outline-none"
              >
                <Switch.Thumb className="block h-5 w-5 translate-x-0.5 rounded-full bg-white transition-transform will-change-transform data-[state=checked]:translate-x-[22px]" />
              </Switch.Root>
              <Label.Root htmlFor="auto" className="select-none text-sm">
                Tự động reconnect
                <div className="text-xs text-slate-500 mt-1">
                  Tự kết nối lại khi có cấu hình hợp lệ.
                </div>
              </Label.Root>
            </div>

            {/* Auto mode */}
            <div className="flex items-start sm:items-center gap-3">
              <Switch.Root
                id="mode"
                checked={uiForm.mode === "auto"}
                onCheckedChange={(v) => {
                  const next = {
                    ...uiForm,
                    mode: (v ? "auto" : "manual") as UiSettings["mode"],
                  } satisfies UiSettings;
                  setUiForm(next);
                  // Gửi ngay xuống ESP32 nếu đang kết nối
                  if (conn === "connected") {
                    (svc as any).setMode?.(v, next.maxTemp, next.hysteresis);
                  }
                }}
                className="relative h-6 w-11 cursor-pointer rounded-full bg-slate-700 data-[state=checked]:bg-emerald-500 outline-none"
              >
                <Switch.Thumb className="block h-5 w-5 translate-x-0.5 rounded-full bg-white transition-transform will-change-transform data-[state=checked]:translate-x-[22px]" />
              </Switch.Root>
              <Label.Root htmlFor="mode" className="select-none text-sm">
                Chế độ tự động
                <span
                  className={`ml-2 rounded-full border px-2 py-0.5 text-xs ${
                    uiForm.mode === "auto"
                      ? "border-emerald-600 text-emerald-400"
                      : "border-slate-600 text-slate-300"
                  }`}
                >
                  {uiForm.mode === "auto" ? "AUTO" : "MANUAL"}
                </span>
                <div className="text-xs text-slate-500 mt-1">
                  {uiForm.mode === "auto"
                    ? `OUT1 bật khi T ≥ ${uiForm.maxTemp}°C, tắt khi T ≤ ${(
                        uiForm.maxTemp - (uiForm.hysteresis ?? 0.5)
                      ).toFixed(1)}°C.`
                    : "MANUAL, bật công tắc trong Outputs."}
                </div>
              </Label.Root>
            </div>
          </div>

          <Separator.Root className="my-4 h-px w-full bg-slate-800" />

          {/* Ngưỡng/Hysteresis */}
          <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
            <div className="sm:col-span-1">
              <label htmlFor="max" className="text-sm text-slate-300">
                Nhiệt độ tối đa (cảnh báo)
              </label>
              <input
                id="max"
                type="number"
                className="mt-1 w-full min-w-0 rounded-lg border border-slate-700 bg-slate-900 px-3 py-2 text-sm outline-none focus:border-slate-500"
                value={uiForm.maxTemp}
                onChange={(e) =>
                  setUiForm({
                    ...uiForm,
                    maxTemp: Number(e.target.value),
                  })
                }
              />
            </div>
            <div className="sm:col-span-1">
              <label htmlFor="hys" className="text-sm text-slate-300">
                Hysteresis (°C)
              </label>
              <input
                id="hys"
                type="number"
                step="0.1"
                className="mt-1 w-full min-w-0 rounded-lg border border-slate-700 bg-slate-900 px-3 py-2 text-sm outline-none focus:border-slate-500"
                value={uiForm.hysteresis ?? 0.5}
                onChange={(e) =>
                  setUiForm({
                    ...uiForm,
                    hysteresis: Number(e.target.value),
                  })
                }
              />
            </div>
          </div>
        </section>
      </div>

      {/* actions sticky */}
      <div className="sticky bottom-0 z-10 -mb-0 bg-slate-900/95 px-4 py-3 sm:bg-transparent sm:px-4">
        <div className="flex flex-col sm:flex-row gap-2">
          <button
            className="w-full sm:w-auto rounded-lg border border-slate-700 bg-slate-800/70 px-3 py-2 text-sm hover:bg-slate-800"
            onClick={async () => {
              const fixed = { ...form, url: normalizeWssUrl(form.url) };
              const changed =
                JSON.stringify(fixed) !== JSON.stringify(config);
              await updateUi(uiForm, true);
              if (changed) {
                await updateConfig(fixed, {
                  persist: true,
                  reconnect: true,
                });
              } else if (conn !== "connected" && uiForm.autoReconnect) {
                await svc.connect();
              }
              // Đẩy cấu hình auto xuống ESP32 (nếu đã kết nối)
              if (conn === "connected") {
                (svc as any).setMode?.(
                  uiForm.mode === "auto",
                  uiForm.maxTemp,
                  uiForm.hysteresis
                );
              }
              setShowSettings(false);
            }}
          >
            Lưu & Kết nối
          </button>

          <button
            className="w-full sm:w-auto rounded-lg border border-slate-700 bg-slate-800/70 px-3 py-2 text-sm hover:bg-slate-800"
            onClick={async () => {
              await svc.disconnect();
              clearConfig();
              setForm({
                url: "",
                root: "esp32",
                username: "",
                password: "",
              });
              setUiForm({
                autoReconnect: true,
                maxTemp: 80,
                mode: "manual",
                hysteresis: 0.5,
              });
            }}
          >
            Xoá cấu hình
          </button>
        </div>
      </div>
    </div>
  </div>
)}

        <div className="mt-8 text-center text-xs text-slate-500">
          React + TypeScript • Tailwind v4 • Radix UI • Recharts • MQTT.js (WSS)
        </div>
      </div>
    </div>
  );
}
