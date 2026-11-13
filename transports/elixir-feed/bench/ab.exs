# A/B: the Elixir feed server against the Rust leader (datasources/reference), same machine, same
# seed, same load. This is the CI entry point (.github/workflows/elixir-feed-bench.yml).
#
#   elixir bench/ab.exs <repo root> <twelve-dollar-server-challenge checkout with seed/feed.db>
#
# Steps, halting with exit 1 on the first failure:
#   1. Conformance: each server, on a fresh seed copy, must pass the challenge's test/test.sh 42/42.
#   2. Bench: each server pinned to CPU 0 on a fresh seed copy; k6 on the other CPUs.
#        rate  open loop at RATE iterations/s: same request rate for both, so CPU µs/req compares.
#        sat   closed loop at each SAT_VUS: req/s. A row whose server sat under 90 % CPU is marked
#              loadgen-bound (k6 was the limit, not the server).
#        load  the challenge's own bench/load.js (think time) at LOAD_VUS: latency only.
#      rate and sat repeat REPS times, alternating which server goes first.
#   3. Gates: no run errors; every run under 1 % failed requests; and, if MAX_RATIO is set, the
#      median Elixir/Rust CPU µs/req ratio at the matched rate is at most MAX_RATIO.
#
# This compares Elixir with Rust on the same runner; it is never a leaderboard number.
# Env: SERVERS ("rust,elixir,lean"; rust is the baseline), REPS (3), RATES ("600,4000"), SAT_VUS ("128"), DUR (20s), LOAD_VUS (2000), LOAD_HOLD (20s),
#      MAX_RATIO (unset = report only), RESULTS (JSON path), GITHUB_STEP_SUMMARY.
# Expected failures (a server that does not start, a missing summary) return {:error, reason} and end
# in exit 1. A k6 summary that is not JSON makes :json.decode raise, which also exits non-zero.

defmodule AB do
  @secret "twelve-dollar-challenge"
  @hz 100
  @base "http://127.0.0.1:3000"

  def main([root, chal]) do
    cfg = config(Path.expand(root), Path.expand(chal))
    IO.puts("cpus=#{cfg.ncpu} server=0 load=#{cfg.load_cpus} kernel=#{cfg.kernel} cpu=#{cfg.cpu_model}")

    with {:ok, cfg} <- preflight(cfg),
         {:ok, cfg} <- conformance(cfg) do
      finish(cfg, bench(cfg))
    else
      {:error, reason} ->
        IO.puts("FAIL: #{inspect(reason)}")
        System.halt(1)
    end
  end

  def main(["--self-test"]), do: SelfTest.run()

  def main(_) do
    IO.puts("usage: elixir bench/ab.exs <repo root> <challenge checkout>")
    System.halt(2)
  end

  # ---- configuration -------------------------------------------------------------------------

  defp config(root, chal) do
    ncpu = nproc()

    %{
      root: root,
      chal: chal,
      servers: "SERVERS" |> System.get_env("rust,elixir,lean") |> String.split(",", trim: true),
      reps: int_env("REPS", 3),
      rates: "RATES" |> System.get_env("600,4000") |> String.split(",", trim: true) |> positive_ints([600, 4000]),
      gated: "GATED" |> System.get_env("rust,elixir") |> String.split(",", trim: true),
      skipped: [],
      sat_vus: "SAT_VUS" |> System.get_env("128") |> String.split(",", trim: true),
      dur: System.get_env("DUR", "20s"),
      load_vus: System.get_env("LOAD_VUS", "2000"),
      load_hold: System.get_env("LOAD_HOLD", "20s"),
      max_ratio: float_env("MAX_RATIO"),
      results: System.get_env("RESULTS", "/tmp/ab-results.json"),
      ncpu: ncpu,
      load_cpus: "1-#{max(ncpu - 1, 1)}",
      kernel: cmd_out("uname", ["-r"]),
      cpu_model: cpu_model()
    }
  end

  defp nproc do
    case Integer.parse(cmd_out("nproc", [])) do
      {n, _} -> n
      :error -> 1
    end
  end

  defp int_env(name, default) do
    case Integer.parse(System.get_env(name, "")) do
      {n, ""} when n > 0 -> n
      _ -> default
    end
  end

  def positive_ints(parts, default) do
    ns = for p <- parts, {n, ""} <- [Integer.parse(p)], n > 0, uniq: true, do: n
    if ns == [], do: default, else: ns
  end

  defp float_env(name) do
    case Float.parse(System.get_env(name, "")) do
      {f, ""} when f > 0 -> f
      _ -> nil
    end
  end

  defp cmd_out(cmd, args) do
    case System.find_executable(cmd) do
      nil -> "unknown"
      exe -> exe |> System.cmd(args, stderr_to_stdout: true) |> elem(0) |> String.trim()
    end
  end

  defp cpu_model do
    case File.read("/proc/cpuinfo") do
      {:ok, text} ->
        text
        |> String.split("\n")
        |> Enum.find_value("unknown", fn line ->
          case String.split(line, ":", parts: 2) do
            [k, v] -> if String.trim(k) == "model name", do: String.trim(v), else: nil
            _ -> nil
          end
        end)

      {:error, _} ->
        "unknown"
    end
  end

  # ---- preflight -----------------------------------------------------------------------------

  defp preflight(cfg) do
    missing =
      for exe <- ["taskset", "timeout", "k6", "curl", "bash", "elixir", "kill"], System.find_executable(exe) == nil, do: exe

    unbuilt = for s <- cfg.servers, s != "elixir", not File.exists?(elem(command(cfg, s), 0)), do: s
    {must, may} = Enum.split_with(unbuilt, &(&1 in cfg.gated))

    cond do
      missing != [] -> {:error, {:missing_tools, missing}}
      cfg.ncpu < 2 -> {:error, {:need_two_cpus, cfg.ncpu}}
      "rust" not in cfg.servers -> {:error, :rust_is_the_baseline_and_must_run}
      must != [] -> {:error, {:not_built, must}}
      not File.exists?(Path.join(cfg.chal, "seed/feed.db")) -> {:error, :seed_missing}
      not File.exists?(Path.join(cfg.chal, "seed/tokens.json")) -> {:error, :tokens_missing}
      true -> {:ok, Enum.reduce(may, cfg, &skip(&2, &1, "not built"))}
    end
  end

  # A report-only server that cannot run is dropped with its reason; the report lists it.
  defp skip(cfg, server, why) do
    IO.puts("skipped #{server}: #{why}")
    %{cfg | servers: List.delete(cfg.servers, server), skipped: cfg.skipped ++ [{server, why}]}
  end

  defp rust_bin(cfg), do: Path.join(cfg.root, "datasources/reference/target/release/feed-server")

  # ---- server lifecycle ----------------------------------------------------------------------

  defp command(cfg, "rust"), do: {rust_bin(cfg), []}
  # The repo's Lean 4 port (lake build feed-server): poll(2) loop, shim.c B-tree reads.
  defp command(cfg, "lean"), do: {Path.join(cfg.root, ".lake/build/bin/feed-server"), []}
  # The challenge's lean-derive-hargup entry, unmodified: C generated from a Lean spec, io_uring runtime.
  defp command(cfg, "hargup"), do: {Path.join(cfg.chal, "submissions/lean-derive-hargup/bin/server"), []}

  defp command(_cfg, "elixir") do
    {System.find_executable("elixir"),
     ["--erl", "+S 1:1 +SDio 1 +sbwt none +sbwtdcpu none +sbwtdio none", "-S", "mix", "run", "--no-halt"]}
  end

  defp start(cfg, server) do
    db = "/tmp/ab-#{server}.db"
    _ = File.rm(db <> "-wal")
    _ = File.rm(db <> "-shm")
    {exe, args} = command(cfg, server)

    env = [{~c"SQLITE_PATH", ~c"#{db}"}, {~c"JWT_SECRET", ~c"#{@secret}"}, {~c"HOST", ~c"127.0.0.1"},
           {~c"PORT", ~c"3000"}, {~c"MIX_ENV", ~c"prod"}]

    with :down <- health(),
         :ok <- File.cp(Path.join(cfg.chal, "seed/feed.db"), db) |> tag(:seed_copy),
         port = Port.open({:spawn_executable, System.find_executable("taskset")},
                  [:binary, :exit_status, :stderr_to_stdout, cd: Path.join(cfg.root, "transports/elixir-feed"),
                   env: env, args: ["-c", "0", exe | args]]),
         {:ok, pid} <- os_pid(port),
         :ok <- wait_up(port, 60),
         true <- File.exists?("/proc/#{pid}") || {:error, {:server_exited, server}} do
      {:ok, %{server: server, port: port, pid: pid}}
    else
      :up -> {:error, :port_3000_already_serving}
      {:error, reason} -> {:error, {server, reason}}
    end
  end

  defp tag(:ok, _what), do: :ok
  defp tag({:error, reason}, what), do: {:error, {what, reason}}

  defp os_pid(port) do
    case Port.info(port, :os_pid) do
      {:os_pid, pid} -> {:ok, pid}
      nil -> {:error, :no_os_pid}
    end
  end

  defp wait_up(port, 0), do: {:error, {:did_not_start, tail(port, "")}}

  defp wait_up(port, n) do
    receive do
      {^port, {:exit_status, code}} -> {:error, {:exited, code, tail(port, "")}}
    after
      0 ->
        case health() do
          :up -> :ok
          :down -> Process.sleep(1000) && wait_up(port, n - 1)
        end
    end
  end

  # The last 500 bytes the server printed (perror lines), drained from the mailbox.
  defp tail(port, acc) do
    receive do
      {^port, {:data, d}} -> tail(port, acc <> d)
    after
      0 -> binary_part(acc, max(byte_size(acc) - 500, 0), min(byte_size(acc), 500))
    end
  end

  defp health do
    case System.cmd("curl", ["-s", "-m", "2", "-o", "/dev/null", "-w", "%{http_code}", @base <> "/health"]) do
      {"200", 0} -> :up
      {_out, _code} -> :down
    end
  end

  defp stop(h) do
    _ = System.cmd("kill", ["#{h.pid}"], stderr_to_stdout: true)
    drain(h.port, 10_000)
    wait_down(10)
  end

  defp drain(port, ms) do
    receive do
      {^port, {:exit_status, _}} -> :ok
      {^port, _data} -> drain(port, ms)
    after
      ms ->
        _ = System.cmd("kill", ["-9", "#{elem(os_pid(port), 1)}"], stderr_to_stdout: true)
        :ok
    end
  end

  defp wait_down(0), do: :ok

  defp wait_down(n) do
    case health() do
      :down -> :ok
      :up -> Process.sleep(500) && wait_down(n - 1)
    end
  end

  # ---- conformance ---------------------------------------------------------------------------

  defp conformance(cfg) do
    Enum.reduce_while(cfg.servers, {:ok, cfg}, fn server, {:ok, acc} ->
      gated = server in cfg.gated

      case start(cfg, server) do
        {:ok, h} ->
          {out, _code} = System.cmd("timeout", ["300", "bash", "test/test.sh", @base], cd: cfg.chal, stderr_to_stdout: true)
          stop(h)
          last = out |> String.split("\n", trim: true) |> List.last()
          IO.puts("conformance #{server}: #{last}")

          cond do
            last == "passed=42 failed=0" -> {:cont, {:ok, acc}}
            gated -> IO.puts(out) && {:halt, {:error, {:conformance, server, last}}}
            true -> {:cont, {:ok, skip(acc, server, "conformance #{last}")}}
          end

        {:error, reason} when gated ->
          {:halt, {:error, reason}}

        {:error, reason} ->
          {:cont, {:ok, skip(acc, server, "did not start: #{inspect(reason)}")}}
      end
    end)
  end

  # ---- bench ---------------------------------------------------------------------------------

  defp bench(cfg) do
    sat_js = Path.join(cfg.root, "transports/elixir-feed/bench/sat.js")
    load_js = load_wrapper(cfg)

    repeated =
      for rep <- 1..cfg.reps,
          {kind, param, args} <-
            for(r <- cfg.rates, do: {"rate", r, ["-e", "MODE=rate", "-e", "RATE=#{r}", "-e", "VUS=256", sat_js]}) ++
              for(v <- cfg.sat_vus, do: {"sat", v, ["-e", "MODE=sat", "-e", "VUS=#{v}", sat_js]}),
          server <- order(cfg.servers, rep) do
        {rep, kind, param, server, ["-e", "DURATION=#{cfg.dur}" | args]}
      end

    latency =
      for server <- cfg.servers do
        {1, "load", cfg.load_vus, server,
         ["-e", "VUS=#{cfg.load_vus}", "-e", "RAMP_UP=10s", "-e", "DURATION=#{cfg.load_hold}", load_js]}
      end

    Enum.map(repeated ++ latency, fn run -> one(cfg, run) end)
  end

  defp order(servers, rep), do: if(rem(rep, 2) == 1, do: servers, else: Enum.reverse(servers))

  # load.js plus a handleSummary that writes the JSON summary where SUMMARY says.
  defp load_wrapper(cfg) do
    path = "/tmp/ab-load-wrap.js"
    load = Path.join(cfg.chal, "bench/load.js")

    _ = File.write(path, """
    export { default, options } from '#{load}';
    export function handleSummary(data) { return { [__ENV.SUMMARY]: JSON.stringify(data) }; }
    """)

    path
  end

  defp one(cfg, {rep, kind, param, server, k6args}) do
    summary = "/tmp/ab-#{server}-#{kind}-#{param}-#{rep}.json"
    _ = File.rm(summary)
    IO.puts("run rep=#{rep} #{kind}=#{param} #{server}")

    case start(cfg, server) do
      {:ok, h} ->
        t0 = cpu_ticks(h.pid)
        w0 = System.monotonic_time(:millisecond)
        args = ["-c", cfg.load_cpus, "k6", "run", "-q", "-e", "TOKENS=#{Path.join(cfg.chal, "seed/tokens.json")}",
                "-e", "SUMMARY=#{summary}" | k6args]
        # A hung server must cost one run, not the job's timeout.
        {_out, _code} = System.cmd("timeout", ["300", "taskset" | args], stderr_to_stdout: true)
        t1 = cpu_ticks(h.pid)
        w1 = System.monotonic_time(:millisecond)
        stop(h)
        summarize(%{rep: rep, kind: kind, param: param, server: server}, summary, t0, t1, w1 - w0)

      {:error, reason} ->
        {:error, %{rep: rep, kind: kind, param: param, server: server, reason: inspect(reason)}}
    end
  end

  # utime + stime of the server process (all threads), in clock ticks; nil if unreadable.
  defp cpu_ticks(pid) do
    with {:ok, s} <- File.read("/proc/#{pid}/stat"),
         [_pre, post] <- String.split(s, ") ", parts: 2),
         f = String.split(post),
         {u, ""} <- Integer.parse(Enum.at(f, 11, "")),
         {k, ""} <- Integer.parse(Enum.at(f, 12, "")) do
      u + k
    else
      _ -> nil
    end
  end

  defp summarize(run, file, t0, t1, wall_ms) do
    with {:ok, text} <- File.read(file),
         %{"metrics" => m} <- :json.decode(text),
         reqs when is_number(reqs) and reqs > 0 <- get_in(m, ["http_reqs", "values", "count"]) do
      ticks = if is_integer(t0) and is_integer(t1), do: t1 - t0, else: nil
      cpu_s = if ticks, do: ticks / @hz, else: nil

      {:ok,
       Map.merge(run, %{
         requests: reqs,
         rps: get_in(m, ["http_reqs", "values", "rate"]),
         p95: get_in(m, ["http_req_duration", "values", "p(95)"]),
         p99: get_in(m, ["http_req_duration", "values", "p(99)"]),
         failed: get_in(m, ["http_req_failed", "values", "rate"]) || 0.0,
         # Open-loop iterations k6 could not start: the server did not hold the rate.
         dropped: get_in(m, ["dropped_iterations", "values", "count"]) || 0,
         cpu_pct: if(cpu_s, do: cpu_s * 100_000 / max(wall_ms, 1), else: nil),
         us_per_req: if(cpu_s, do: cpu_s * 1.0e6 / reqs, else: nil)
       })}
    else
      other -> {:error, Map.put(run, :reason, "no k6 summary (#{inspect(other) |> String.slice(0, 120)})")}
    end
  end

  # ---- report and gates ----------------------------------------------------------------------

  # Pure: results in, {oks, errors, ratios, gates} out. The self-test drives this with fixtures.
  def evaluate(cfg, results) do
    oks = for {:ok, r} <- results, do: r
    errors = for {:error, e} <- results, do: e
    gated_errors = Enum.filter(errors, &(&1.server in cfg.gated))
    top = Enum.max(cfg.rates)
    ratios = for s <- cfg.servers, s != "rust", r <- cfg.rates, into: %{}, do: {{s, r}, median_ratio(oks, s, r)}
    ratio = Map.get(ratios, {"elixir", top})

    gates = [
      {"every gated run completed", gated_errors == [], "#{length(gated_errors)} gated run(s) failed to complete"},
      {"under 1 % failed requests (#{Enum.join(cfg.gated, ", ")}; others report only)",
       Enum.all?(oks, &(&1.server not in cfg.gated or &1.failed < 0.01)), "a gated run had >= 1 % failed requests"},
      {"Elixir/Rust CPU µs/req at #{top} iterations/s",
       cfg.max_ratio == nil or (is_number(ratio) and ratio <= cfg.max_ratio),
       "ratio #{fmt(ratio, 2)} over MAX_RATIO #{fmt(cfg.max_ratio, 2)}"}
    ]

    {oks, errors, ratios, gates}
  end

  defp finish(cfg, results) do
    {oks, errors, ratios, gates} = evaluate(cfg, results)
    text = report(cfg, oks, errors, ratios, gates)
    IO.puts(text)

    case System.get_env("GITHUB_STEP_SUMMARY") do
      nil -> :ok
      path -> File.write(path, text, [:append])
    end

    meta = %{kernel: cfg.kernel, cpu_model: cfg.cpu_model, ncpu: cfg.ncpu, sha: System.get_env("GITHUB_SHA", ""),
             rates: cfg.rates, reps: cfg.reps, max_ratio: cfg.max_ratio,
             ratios: for({{s, r}, v} <- ratios, do: %{server: s, rate: r, ratio_to_rust: v})}
    _ = File.write(cfg.results, :json.encode(%{meta: meta, runs: oks, errors: errors}))

    case Enum.all?(gates, fn {_n, ok, _why} -> ok end) do
      true -> System.halt(0)
      false -> System.halt(1)
    end
  end

  # Median over reps of (server µs/req) / (rust µs/req) in the matched-rate runs at one rate.
  defp median_ratio(oks, server, rate) do
    ratios =
      oks
      # A run that dropped iterations did not hold the rate, so its CPU per request is not comparable.
      |> Enum.filter(&(&1.kind == "rate" and &1.param == rate and is_number(&1.us_per_req) and &1.dropped == 0))
      |> Enum.group_by(& &1.rep)
      |> Enum.flat_map(fn {_rep, rs} ->
        case {Enum.find(rs, &(&1.server == server)), Enum.find(rs, &(&1.server == "rust"))} do
          {%{us_per_req: e}, %{us_per_req: r}} when r > 0 -> [e / r]
          {_e, _r} -> []
        end
      end)
      |> Enum.sort()

    case ratios do
      [] -> nil
      _ -> Enum.at(ratios, div(length(ratios), 2))
    end
  end

  defp report(cfg, oks, errors, ratios, gates) do
    ratio_rows =
      for {{s, r}, v} <- Enum.sort(ratios), do: "| #{s} | #{r} | #{fmt(v, 2)}× |\n"

    head = """
    ## Feed servers vs the Rust leader (same runner)

    `#{cfg.cpu_model}`, #{cfg.ncpu} vCPU, kernel `#{cfg.kernel}`. Server pinned to CPU 0, k6 on CPUs #{cfg.load_cpus}.
    Same-runner comparison only; never a leaderboard number. Every server passed test/test.sh 42/42 first.

    Server CPU per request relative to Rust at a matched rate (median of #{cfg.reps}; parity = 1.00\
    #{if cfg.max_ratio, do: "; Elixir gate ≤ #{fmt(cfg.max_ratio, 2)} at #{Enum.max(cfg.rates)}", else: "; no gate set"}):

    | server | iterations/s | CPU µs/req ÷ Rust |
    |---|---|---|
    #{ratio_rows}
    | rep | load | param | server | req/s | p95 ms | p99 ms | failed % | server CPU % | CPU µs/req | note |
    |---|---|---|---|---|---|---|---|---|---|---|
    """

    rows =
      Enum.map(oks, fn r ->
        note =
          cond do
            r.kind == "rate" and r.dropped > 0 -> "did not hold the rate (#{r.dropped} dropped)"
            r.kind == "sat" and is_number(r.cpu_pct) and r.cpu_pct < 90 -> "loadgen-bound"
            true -> ""
          end

        "| #{r.rep} | #{r.kind} | #{r.param} | #{r.server} | #{fmt(r.rps, 0)} | #{fmt(r.p95, 2)} | #{fmt(r.p99, 2)} | " <>
          "#{fmt(r.failed * 100, 2)} | #{fmt(r.cpu_pct, 0)} | #{fmt(r.us_per_req, 1)} | #{note} |\n"
      end)

    errs = Enum.map(errors, fn e -> "- run error: #{inspect(e)}\n" end)
    skips = Enum.map(cfg.skipped, fn {s, why} -> "- skipped #{s} (report only): #{why}\n" end)
    gate_lines = Enum.map(gates, fn {name, ok, why} -> "- #{if ok, do: "pass", else: "FAIL"}: #{name}#{if ok, do: "", else: " (#{why})"}\n" end)
    IO.iodata_to_binary([head, rows, "\n", skips, errs, gate_lines, "\n"])
  end

  defp fmt(x, d) when is_number(x), do: :erlang.float_to_binary(x * 1.0, decimals: d)
  defp fmt(_x, _d), do: "n/a"
end

defmodule SelfTest do
  # Fixtures through AB.evaluate/2: each case names the gate outcomes it must produce.
  @cfg %{servers: ["rust", "elixir", "hargup"], rates: [600, 4000], gated: ["rust", "elixir"], max_ratio: nil}

  defp run_row(server, rep, us, extra \\ %{}) do
    Map.merge(%{server: server, kind: "rate", param: 4000, rep: rep, us_per_req: us, dropped: 0, failed: 0.0}, extra)
  end

  defp clean, do: for(rep <- 1..3, {s, us} <- [{"rust", 10.0}, {"elixir", 50.0}], do: {:ok, run_row(s, rep, us)})

  defp gates(cfg, results), do: AB.evaluate(cfg, results) |> elem(3) |> Enum.map(&elem(&1, 1))
  defp ratio(cfg, results), do: AB.evaluate(cfg, results) |> elem(2) |> Map.get({"elixir", 4000})

  def run do
    cases = [
      {"clean runs pass every gate", gates(@cfg, clean()) == [true, true, true]},
      {"clean ratio is the median 5.00", ratio(@cfg, clean()) == 5.0},
      {"MAX_RATIO under the ratio fails the ratio gate", gates(%{@cfg | max_ratio: 4.0}, clean()) == [true, true, false]},
      {"MAX_RATIO over the ratio passes", gates(%{@cfg | max_ratio: 6.0}, clean()) == [true, true, true]},
      {"a report-only server's error does not fail",
       gates(@cfg, clean() ++ [{:error, %{server: "hargup", reason: "x"}}]) == [true, true, true]},
      {"a gated server's error fails", gates(@cfg, clean() ++ [{:error, %{server: "elixir", reason: "x"}}]) == [false, true, true]},
      {"a report-only server's failed requests do not fail",
       gates(@cfg, clean() ++ [{:ok, run_row("hargup", 1, 9.0, %{failed: 0.5})}]) == [true, true, true]},
      {"a gated server's 2 % failed requests fail",
       gates(@cfg, clean() ++ [{:ok, run_row("elixir", 4, 50.0, %{failed: 0.02})}]) == [true, false, true]},
      {"a run that dropped iterations leaves the ratio",
       ratio(@cfg, clean() ++ [{:ok, run_row("elixir", 1, 1000.0, %{dropped: 5})}]) == 5.0},
      {"without the dropped filter that run would move it (control)",
       ratio(@cfg, [{:ok, run_row("rust", 1, 10.0)}, {:ok, run_row("elixir", 1, 1000.0)}]) == 100.0},
      {"RATES keeps positive integers once", AB.positive_ints(["a", "0", "600", "600", "4000"], [1]) == [600, 4000]},
      {"RATES with nothing usable falls back", AB.positive_ints(["x", "-3"], [600, 4000]) == [600, 4000]}
    ]

    Enum.each(cases, fn {name, ok} -> IO.puts("#{if ok, do: "ok  ", else: "FAIL"} #{name}") end)
    failed = Enum.count(cases, fn {_n, ok} -> not ok end)
    IO.puts("#{length(cases) - failed} of #{length(cases)} self-test cases pass")
    System.halt(if failed == 0, do: 0, else: 1)
  end
end

AB.main(System.argv())
