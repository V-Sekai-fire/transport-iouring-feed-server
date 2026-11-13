defmodule Feed.Conn do
  @moduledoc """
  One process per keep-alive connection: passive recv, parse every complete request in the buffer
  (pipelining), send all their responses in one write. Idle connections stay open (no timeout, so
  Nginx-style reuse never meets a socket we closed).
  """

  @max_head 16_384
  @max_body 65_536

  def serve(sock), do: loop(sock, <<>>)

  defp loop(sock, buf) do
    case :gen_tcp.recv(sock, 0) do
      {:ok, data} ->
        buf = if buf == <<>>, do: data, else: buf <> data
        drain(sock, buf, [])

      {:error, _closed} ->
        :gen_tcp.close(sock)
    end
  end

  # Handle every complete request in buf; then send the batch and wait for more.
  defp drain(sock, buf, acc) do
    case parse(buf) do
      {:ok, resp, rest, close?} ->
        case close? do
          true -> send_and_close(sock, [acc, resp])
          false -> drain(sock, rest, [acc, resp])
        end

      :more ->
        case flush(sock, acc) do
          :ok -> loop(sock, buf)
          :closed -> :gen_tcp.close(sock)
        end

      :bad ->
        send_and_close(sock, [acc, Feed.Router.error(400, "bad request")])
    end
  end

  defp flush(_sock, []), do: :ok

  defp flush(sock, acc) do
    case :gen_tcp.send(sock, acc) do
      :ok -> :ok
      {:error, _} -> :closed
    end
  end

  defp send_and_close(sock, iodata) do
    _ = :gen_tcp.send(sock, iodata)
    :gen_tcp.close(sock)
  end

  # {:ok, response, rest, close?} | :more | :bad
  defp parse(buf) do
    case :binary.match(buf, "\r\n\r\n") do
      :nomatch ->
        if byte_size(buf) > @max_head, do: :bad, else: :more

      {pos, 4} ->
        head = binary_part(buf, 0, pos)
        after_head = pos + 4

        case :binary.split(head, "\r\n") do
          [line, hdrs] -> request(line, hdrs, buf, after_head)
          [line] -> request(line, <<>>, buf, after_head)
        end
    end
  end

  defp request(line, hdrs, buf, start) do
    case :binary.split(line, " ", [:global]) do
      [meth, path, _ver] ->
        {clen, auth, close?} = headers(hdrs)
        total = start + clen

        cond do
          clen > @max_body -> :bad
          byte_size(buf) < total -> :more
          true ->
            body = binary_part(buf, start, clen)
            rest = binary_part(buf, total, byte_size(buf) - total)
            {:ok, Feed.Router.route(meth, path, auth, body), rest, close?}
        end

      _ ->
        :bad
    end
  end

  # Scan header lines for Content-Length, Authorization and Connection: close.
  defp headers(<<>>), do: {0, nil, false}

  defp headers(hdrs) do
    hdrs
    |> :binary.split("\r\n", [:global])
    |> Enum.reduce({0, nil, false}, &header/2)
  end

  defp header(line, {clen, auth, close?} = acc) do
    case :binary.split(line, ":") do
      [name, value] ->
        case byte_size(name) do
          14 -> if lower(name) == "content-length", do: {int(trim(value)), auth, close?}, else: acc
          13 -> if lower(name) == "authorization", do: {clen, trim(value), close?}, else: acc
          10 ->
            if lower(name) == "connection" and lower(trim(value)) == "close",
              do: {clen, auth, true}, else: acc
          _ -> acc
        end

      _ ->
        acc
    end
  end

  defp lower(b), do: for(<<c <- b>>, into: <<>>, do: <<(if c in ?A..?Z, do: c + 32, else: c)>>)

  defp trim(<<" ", rest::binary>>), do: trim(rest)
  defp trim(<<"\t", rest::binary>>), do: trim(rest)
  defp trim(v), do: v

  defp int(v) do
    case Integer.parse(v) do
      {n, _} when n >= 0 -> n
      _ -> 0
    end
  end
end
