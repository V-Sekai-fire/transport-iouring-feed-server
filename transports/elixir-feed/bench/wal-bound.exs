# Does the NIF's WAL checkpoint copy frames back? Two builds, same write load against a fresh seed:
#   with     FEED_CKPT_FRAMES=16          the log must stay under a fifth of the control's peak
#   control  FEED_CKPT_FRAMES=1000000000  checkpoints never start: the log must grow past MIN_CONTROL_BYTES,
#                                         or the load is too small to tell the two apart and the check says so
# Also: every like answers 201, PRAGMA integrity_check is ok, and the likes table gained exactly COUNT rows.
#   elixir bench/wal-bound.exs <repo root> <challenge checkout> [count]
# Exit 0 on pass, 1 on a failed check, 2 on usage.

defmodule WalBound do
  @min_control 20_000_000
  @secret "twelve-dollar-challenge"

  def main([root, chal | rest]) do
    count = case rest do [n] -> String.to_integer(n); _ -> 4000 end
    dir = Path.join(Path.expand(root), "transports/elixir-feed")
    chal = Path.expand(chal)
    with {:ok, a} <- run_case(dir, chal, "with checkpoints", "-DFEED_CKPT_FRAMES=16", count),
         {:ok, b} <- run_case(dir, chal, "control, checkpoints off", "-DFEED_CKPT_FRAMES=1000000000", count) do
      verdict(a, b)
    else
      {:error, why} -> IO.puts("FAIL: #{why}"); System.halt(1)
    end
  end

  def main(_), do: (IO.puts("usage: elixir bench/wal-bound.exs <repo root> <challenge> [count]"); System.halt(2))

  defp verdict(a, b) do
    checks = [
      {"control grew past #{@min_control} bytes (the load can tell the two apart)", b.peak > @min_control},
      {"checkpoints kept the log under a fifth of the control's peak", a.peak * 5 < b.peak},
      {"every like answered 201 in both builds", a.codes == %{201 => a.count} and b.codes == %{201 => b.count}},
      {"integrity_check ok in both", a.integrity == "ok" and b.integrity == "ok"},
      {"likes gained == requests in both", a.gained == a.count and b.gained == b.count}
    ]
    Enum.each(checks, fn {n, ok} -> IO.puts("#{if ok, do: "pass", else: "FAIL"}: #{n}") end)
    IO.puts("log peak: with=#{a.peak} control=#{b.peak} bytes")
    System.halt(if Enum.all?(checks, &elem(&1, 1)), do: 0, else: 1)
  end

  defp run_case(dir, chal, label, cflags, count) do
    IO.puts("== #{label}")
    File.rm_rf(Path.join(dir, "_build")); File.rm_rf(Path.join(dir, "priv"))
    db = "/tmp/walbound.db"
    with {_, 0} <- System.cmd("mix", ["compile", "--warnings-as-errors"], cd: dir, stderr_to_stdout: true,
                       env: [{"MIX_ENV", "prod"}, {"FEED_NIF_CFLAGS", cflags}]),
         :ok <- File.cp(Path.join(chal, "seed/feed.db"), db),
         _ <- File.rm(db <> "-wal"),
         base = likes(db),
         {:ok, port} <- start(dir, db),
         :ok <- wait_up(30, port) do
      out = hammer(chal, db, count)
      stop(port)
      {n, _} = System.cmd("sqlite3", [db, "PRAGMA integrity_check"])
      {:ok, Map.merge(out, %{count: count, integrity: String.trim(n), gained: likes(db) - base})}
    else
      other -> {:error, "#{label}: #{inspect(other) |> String.slice(0, 300)}"}
    end
  end

  defp likes(db) do
    case System.cmd("sqlite3", [db, "select count(*) from likes"], stderr_to_stdout: true) do
      {out, 0} -> out |> String.trim() |> Integer.parse() |> then(fn {n, _} -> n; :error -> -1 end)
      {_out, _code} -> -1
    end
  end

  defp start(dir, db) do
    env = [{~c"SQLITE_PATH", ~c"#{db}"}, {~c"JWT_SECRET", ~c"#{@secret}"}, {~c"HOST", ~c"127.0.0.1"}, {~c"PORT", ~c"3000"}, {~c"MIX_ENV", ~c"prod"}]
    {:ok, Port.open({:spawn_executable, System.find_executable("elixir")},
      [:binary, :exit_status, :stderr_to_stdout, cd: dir, env: env, args: ["--erl", "+S 1:1", "-S", "mix", "run", "--no-halt"]])}
  end

  # A server that never comes up is stopped here, so a failed case leaves nothing holding port 3000.
  defp wait_up(0, port), do: stop(port) && {:error, :did_not_start}
  defp wait_up(n, port) do
    case System.cmd("curl", ["-s", "-m", "2", "-o", "/dev/null", "-w", "%{http_code}", "http://127.0.0.1:3000/health"]) do
      {"200", 0} -> :ok
      _ -> Process.sleep(1000) && wait_up(n - 1, port)
    end
  end

  defp stop(port) do
    {:os_pid, pid} = Port.info(port, :os_pid)
    System.cmd("kill", ["#{pid}"])
    receive do {^port, {:exit_status, _}} -> :ok after 10_000 -> :ok end
    Process.sleep(500)
  end

  # Distinct-user likes, 32 at a time; samples the -wal file every 50 ms.
  defp hammer(chal, db, count) do
    :inets.start()
    tokens = chal |> Path.join("seed/tokens.json") |> File.read!() |> :json.decode() |> Enum.map(& &1["token"])
    n = length(tokens)
    wal = db <> "-wal"
    me = self()
    sampler = spawn(fn -> sample(me, wal, 0) end)
    codes =
      1..count
      |> Task.async_stream(fn i ->
        tok = Enum.at(tokens, rem(i, n))
        post = 1 + div(i, n) * 7 + rem(i, 997)
        req = {~c"http://127.0.0.1:3000/posts/#{post}/like", [{~c"authorization", ~c"Bearer " ++ String.to_charlist(tok)}], ~c"application/json", ""}
        case :httpc.request(:post, req, [timeout: 10_000], []) do
          {:ok, {{_, code, _}, _, _}} -> code
          {:error, r} -> {:error, r}
        end
      end, max_concurrency: 32, timeout: 30_000)
      |> Enum.map(fn {:ok, c} -> c end)
      |> Enum.frequencies()
    send(sampler, :stop)
    peak = receive do {:peak, p} -> p after 5000 -> 0 end
    %{codes: codes, peak: peak}
  end

  defp sample(parent, wal, peak) do
    receive do
      :stop -> send(parent, {:peak, peak})
    after
      50 ->
        size = case File.stat(wal) do {:ok, s} -> s.size; _ -> 0 end
        sample(parent, wal, max(peak, size))
    end
  end
end

WalBound.main(System.argv())
