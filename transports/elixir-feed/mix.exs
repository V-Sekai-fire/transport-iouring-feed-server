defmodule Feed.MixProject do
  use Mix.Project

  def project do
    [
      app: :feed,
      version: "0.1.0",
      elixir: "~> 1.18",
      compilers: [:feed_nif] ++ Mix.compilers(),
      start_permanent: Mix.env() == :prod,
      deps: []
    ]
  end

  def application do
    [extra_applications: [:logger, :crypto], mod: {Feed.Application, []}]
  end
end

defmodule Mix.Tasks.Compile.FeedNif do
  @moduledoc """
  Builds priv/feed_nif.so: the pinned SQLite amalgamation as its own translation unit (third-party,
  compiled with -w and the reference's -D flags), and c_src/feed_nif.c with -Werror.
  Returns {:ok, []} or {:error, [diagnostic]}; never raises.
  """
  use Mix.Task.Compiler

  @sqlite_defines ~w(SQLITE_THREADSAFE=2 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DQS=0 SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_MAX_EXPR_DEPTH=0 SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_PROGRESS_CALLBACK SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_AUTOINIT SQLITE_OMIT_UTF16 SQLITE_OMIT_TRACE SQLITE_OMIT_COMPLETE SQLITE_OMIT_GET_TABLE
    SQLITE_OMIT_TCL_VARIABLE SQLITE_USE_ALLOCA HAVE_FDATASYNC=1 HAVE_USLEEP=1)

  @impl true
  def run(_args) do
    root = File.cwd!()
    vendor = Path.expand("../../datasources/reference/vendor", root)
    erts = Path.join([to_string(:code.root_dir()), "erts-#{:erlang.system_info(:version)}", "include"])
    out = Path.join(root, "priv/feed_nif.so")
    obj = Path.join(root, "priv/sqlite3.o")
    src = Path.join(root, "c_src/feed_nif.c")
    defs = Enum.map(@sqlite_defines, &"-D#{&1}")
    darwin? = match?({:unix, :darwin}, :os.type())
    so_flags = if darwin?, do: ["-dynamiclib", "-undefined", "dynamic_lookup"], else: ["-shared"]
    # Same code-generation flags as the leader's build.rs (-O3 -march=native -fno-plt), on Linux.
    opt = if darwin?, do: ["-O3"], else: ["-O3", "-march=native", "-fno-plt"]

    steps = [
      {not File.exists?(obj) or stale?(Path.join(vendor, "sqlite3.c"), obj),
       opt ++ ["-fPIC", "-w", "-c", "-I", vendor] ++ defs ++ [Path.join(vendor, "sqlite3.c"), "-o", obj]},
      {stale?(src, out) or stale?(obj, out),
       opt ++ ["-fPIC", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-I", vendor, "-I", erts] ++ defs ++ test_flags() ++ so_flags ++ [src, obj, "-o", out, "-lpthread", "-lm"]}
    ]

    case File.mkdir_p(Path.join(root, "priv")) do
      :ok -> build(steps)
      {:error, reason} -> {:error, [diag("cannot create priv/: #{inspect(reason)}")]}
    end
  end

  # Test builds only, e.g. FEED_NIF_CFLAGS="-DFEED_CKPT_FRAMES=16"; remove priv/ first, the stale check ignores it.
  defp test_flags, do: "FEED_NIF_CFLAGS" |> System.get_env("") |> String.split(" ", trim: true)

  defp build(steps) do
    Enum.reduce_while(steps, {:ok, []}, fn {needed, args}, acc ->
      case needed do
        false -> {:cont, acc}
        true -> cc(args, acc)
      end
    end)
  end

  defp cc(args, acc) do
    case System.find_executable("cc") do
      nil ->
        {:halt, {:error, [diag("cc not found on PATH")]}}

      cc ->
        case System.cmd(cc, args, stderr_to_stdout: true) do
          {_, 0} -> {:cont, acc}
          {text, code} -> Mix.shell().error(text) && {:halt, {:error, [diag("cc exited #{code}")]}}
        end
    end
  end

  defp stale?(src, out) do
    case {File.stat(src, time: :posix), File.stat(out, time: :posix)} do
      {{:ok, s}, {:ok, o}} -> s.mtime > o.mtime
      {_s, _o} -> true
    end
  end

  defp diag(msg) do
    %Mix.Task.Compiler.Diagnostic{compiler_name: "feed_nif", file: "c_src/feed_nif.c", message: msg,
      position: 0, severity: :error}
  end
end
