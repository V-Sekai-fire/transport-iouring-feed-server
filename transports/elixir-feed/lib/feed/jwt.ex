defmodule Feed.Jwt do
  @moduledoc """
  HS256 verification, matching datasources/reference/src/jwt.rs: "Bearer " prefix, three parts,
  32-byte signature, constant-time compare, header alg HS256, payload object, exp > now,
  sub a positive integer string, username a string. The header/payload JSON is read by the NIF
  (SQLite's JSON functions), which never raises.

  The last verified token is kept in the connection process (k6 users reuse one token per
  connection); exp is still checked on every request.
  """

  @missing {:error, "missing bearer token"}
  @invalid {:error, "invalid or expired token"}
  @payload {:error, "invalid token payload"}

  def verify("Bearer " <> token) do
    now = :os.system_time(:millisecond) / 1000

    case Process.get(:feed_jwt) do
      {^token, exp, uid, user} when now < exp -> {:ok, uid, user}
      _other -> full(token, now)
    end
  end

  def verify(_), do: @missing

  defp full(token, now) do
    case :binary.split(token, ".", [:global]) do
      [h, p, s] -> signed(token, h, p, s, now)
      _ -> @invalid
    end
  end

  defp signed(token, h, p, s, now) do
    signing = binary_part(token, 0, byte_size(h) + 1 + byte_size(p))
    mac = :crypto.mac(:hmac, :sha256, :persistent_term.get(:feed_secret), signing)

    with {:ok, sig} <- b64(s),
         true <- byte_size(sig) == 32 and :crypto.hash_equals(sig, mac),
         {:ok, hj} <- b64(h),
         {:ok, pj} <- b64(p) do
      claims(token, Feed.Nif.claims(hj, pj), now)
    else
      _ -> @invalid
    end
  end

  defp claims(token, {:ok, exp, sub, user}, now) when is_float(exp) and now < exp do
    case {post_id(sub), user} do
      {{:ok, uid}, u} when is_binary(u) ->
        Process.put(:feed_jwt, {token, exp, uid, u})
        {:ok, uid, u}

      {_sub, _user} ->
        @payload
    end
  end

  defp claims(_token, _claims, _now), do: @invalid

  defp post_id(s) when is_binary(s), do: Feed.Router.post_id(s)
  defp post_id(_), do: :error

  # base64url without padding; rejects a length of 1 mod 4 and non-zero unused bits (as jwt.rs).
  defp b64(s) do
    case rem(byte_size(s), 4) do
      1 -> :error
      _ ->
        case Base.url_decode64(s, padding: false) do
          {:ok, bin} -> if Base.url_encode64(bin, padding: false) == s, do: {:ok, bin}, else: :error
          :error -> :error
        end
    end
  end
end
