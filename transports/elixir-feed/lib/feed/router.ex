defmodule Feed.Router do
  @moduledoc """
  Routing and the non-database answers. Database-backed answers come from Feed.Nif as finished
  binaries; the atom :error from the NIF becomes a canned 500.
  """

  @busy_tries 4
  @ise "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}"

  def route("GET", "/feed", _auth, _body), do: ok(Feed.Nif.feed())

  def route("GET", "/health", _auth, _body) do
    up = System.monotonic_time(:second) - :persistent_term.get(:feed_started)
    ok(Feed.Nif.health(up))
  end

  def route("GET", "/posts/" <> id, _auth, _body) do
    case :binary.match(id, "/") do
      :nomatch ->
        case post_id(id) do
          {:ok, n} -> ok(Feed.Nif.post(n))
          :error -> error(400, "invalid post id")
        end

      _slash ->
        error(404, "not found")
    end
  end

  def route("POST", "/posts", auth, body) do
    case Feed.Jwt.verify(auth) do
      {:ok, uid, user} -> write(fn -> Feed.Nif.create(uid, user, body) end, @busy_tries)
      {:error, msg} -> error(401, msg)
    end
  end

  def route("POST", "/posts/" <> rest, auth, _body) do
    size = byte_size(rest)

    case size >= 5 and binary_part(rest, size - 5, 5) == "/like" do
      true ->
        id = binary_part(rest, 0, size - 5)

        case :binary.match(id, "/") do
          :nomatch -> like(auth, id)
          _slash -> error(404, "not found")
        end

      false ->
        error(404, "not found")
    end
  end

  def route(_meth, _path, _auth, _body), do: error(404, "not found")

  defp like(auth, id) do
    case Feed.Jwt.verify(auth) do
      {:ok, uid, _user} ->
        case post_id(id) do
          {:ok, n} -> write(fn -> Feed.Nif.like(uid, n) end, @busy_tries)
          :error -> error(400, "invalid post id")
        end

      {:error, msg} ->
        error(401, msg)
    end
  end

  @doc "A positive decimal integer: digits only, no leading zero, at most 18 digits."
  def post_id(<<c, _::binary>> = s) when c in ?1..?9 and byte_size(s) <= 18 do
    case digits?(s) do
      true -> {:ok, :erlang.binary_to_integer(s)}
      false -> :error
    end
  end

  def post_id(_), do: :error

  defp digits?(<<>>), do: true
  defp digits?(<<c, rest::binary>>) when c in ?0..?9, do: digits?(rest)
  defp digits?(_), do: false

  defp ok(:error), do: @ise
  defp ok(bin), do: bin

  # A write NIF answers :busy while the tail checkpoint holds writes. The writer waits for the state
  # machine's release message (no clock, no polling), then retries; after @busy_tries it answers 500.
  defp write(_f, 0), do: @ise

  defp write(f, n) do
    case f.() do
      :busy ->
        :ok = Feed.Checkpoint.wait()
        write(f, n - 1)

      other ->
        ok(other)
    end
  end

  def error(status, msg) do
    body = ["{\"error\":\"", msg, "\"}"]
    [status_line(status), "Content-Type: application/json\r\nContent-Length: ",
     Integer.to_string(:erlang.iolist_size(body)), "\r\n\r\n", body]
  end

  defp status_line(400), do: "HTTP/1.1 400 Bad Request\r\n"
  defp status_line(401), do: "HTTP/1.1 401 Unauthorized\r\n"
  defp status_line(404), do: "HTTP/1.1 404 Not Found\r\n"
  defp status_line(_), do: "HTTP/1.1 500 Internal Server Error\r\n"
end
