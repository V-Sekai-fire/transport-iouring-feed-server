defmodule Feed.Application do
  @moduledoc """
  Reads SQLITE_PATH, JWT_SECRET, HOST, PORT; opens the database; starts the listener.
  A failure returns {:error, reason} from start/2 (no raise).
  """
  use Application

  @impl true
  def start(_type, _args) do
    with {:ok, path} <- env("SQLITE_PATH"),
         {:ok, secret} <- env("JWT_SECRET"),
         {:ok, ip} <- host(System.get_env("HOST", "0.0.0.0")),
         {:ok, port} <- port(System.get_env("PORT", "3000")),
         :ok <- Feed.Nif.open(path) do
      :persistent_term.put(:feed_secret, secret)
      :persistent_term.put(:feed_started, System.monotonic_time(:second))
      children = [Feed.Checkpoint, {Feed.Listener, {ip, port}}]
      Supervisor.start_link(children, strategy: :one_for_one, name: Feed.Supervisor)
    else
      {:error, reason} -> {:error, reason}
    end
  end

  defp env(name) do
    case System.get_env(name) do
      nil -> {:error, {:missing_env, name}}
      "" -> {:error, {:missing_env, name}}
      v -> {:ok, v}
    end
  end

  defp host(h) do
    case :inet.parse_address(String.to_charlist(h)) do
      {:ok, ip} -> {:ok, ip}
      {:error, _} -> {:error, {:bad_host, h}}
    end
  end

  defp port(p) do
    case Integer.parse(p) do
      {n, ""} when n > 0 and n < 65536 -> {:ok, n}
      _other -> {:error, {:bad_port, p}}
    end
  end
end
