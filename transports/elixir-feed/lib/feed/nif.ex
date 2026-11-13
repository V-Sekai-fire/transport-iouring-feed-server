defmodule Feed.Nif do
  @moduledoc """
  The SQLite/JSON NIF (c_src/feed_nif.c). Response functions return a finished HTTP/1.1 binary,
  or the atom :error (mapped to a canned 500 by the caller). They never raise.

  The stubs below are replaced when the library loads. If it does not load, @on_load fails and
  the module is not loaded at all, so the stubs are never reached; `:erlang.nif_error/1` keeps
  their return type opaque, so the compiler does not prune the callers' {:ok, ...} clauses.
  """
  @on_load :load

  def load do
    path = :filename.join(:code.priv_dir(:feed), ~c"feed_nif")

    case :erlang.load_nif(path, 0) do
      :ok -> :ok
      {:error, reason} -> {:error, reason}
    end
  end

  def open(_path), do: :erlang.nif_error(:not_loaded)
  def feed, do: :erlang.nif_error(:not_loaded)
  def post(_id), do: :erlang.nif_error(:not_loaded)
  def like(_uid, _id), do: :erlang.nif_error(:not_loaded)
  def create(_uid, _user, _body), do: :erlang.nif_error(:not_loaded)
  def claims(_h, _p), do: :erlang.nif_error(:not_loaded)
  def health(_uptime), do: :erlang.nif_error(:not_loaded)
  def ck_register, do: :erlang.nif_error(:not_loaded)
  def ck_bulk, do: :erlang.nif_error(:not_loaded)
  def ck_tail, do: :erlang.nif_error(:not_loaded)
end
