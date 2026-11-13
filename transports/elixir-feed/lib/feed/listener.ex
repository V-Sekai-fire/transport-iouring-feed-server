defmodule Feed.Listener do
  @moduledoc """
  Owns the listening socket and a pool of acceptors. Each acceptor accepts one connection, starts
  its replacement, and then serves that connection itself (no controlling_process hand-off).
  """
  use GenServer

  @acceptors 8

  def start_link(arg), do: GenServer.start_link(__MODULE__, arg, name: __MODULE__)

  @impl true
  def init({ip, port}) do
    opts = [:binary, packet: :raw, active: false, reuseaddr: true, backlog: 4096, nodelay: true,
            ip: ip]

    case :gen_tcp.listen(port, opts) do
      {:ok, ls} ->
        Enum.each(1..@acceptors, fn _ -> spawn_acceptor(ls) end)
        {:ok, ls}

      {:error, reason} ->
        {:stop, reason}
    end
  end

  def spawn_acceptor(ls), do: spawn(fn -> accept(ls) end)

  defp accept(ls) do
    case :gen_tcp.accept(ls) do
      {:ok, sock} ->
        spawn_acceptor(ls)
        Feed.Conn.serve(sock)

      {:error, :closed} ->
        :ok

      {:error, _reason} ->
        # emfile and friends: back off briefly and try again in this same process.
        Process.sleep(10)
        accept(ls)
    end
  end
end
