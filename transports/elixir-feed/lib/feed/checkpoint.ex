defmodule Feed.Checkpoint do
  @moduledoc """
  The WAL checkpoint as a state machine. No timer, no polling.

      idle --ck_due--> bulk --(ck_bulk returned)--> tail --(ck_tail returned)--> idle

  A write NIF sends `:ck_due` once the log passes the threshold. In `bulk` the writes go on; in `tail`
  the NIF holds them (they answer `:busy`), copies the rest and clears the hold itself. Writers that got
  `:busy` queue here with `{:wait, ref, pid}` and are released the moment the machine is idle again,
  or at once if it already is. Nothing waits on a clock.
  """
  @behaviour :gen_statem

  def child_spec(_arg) do
    %{id: __MODULE__, start: {__MODULE__, :start_link, []}, restart: :permanent, type: :worker}
  end

  def start_link, do: :gen_statem.start_link({:local, __MODULE__}, __MODULE__, [], [])

  @doc """
  Wait until the checkpoint in progress ends, then return. Never raises and never waits on a clock:
  the answer is a message from the machine, or the monitor's DOWN if it is gone. Returns :ok either way,
  and the caller retries its write a bounded number of times.
  """
  def wait do
    case Process.whereis(__MODULE__) do
      nil ->
        :ok

      pid ->
        mon = Process.monitor(pid)
        ref = make_ref()
        send(pid, {:wait, ref, self()})

        receive do
          {:released, ^ref} -> Process.demonitor(mon, [:flush])
          {:DOWN, ^mon, :process, _pid, _reason} -> :ok
        end

        :ok
    end
  end

  @impl true
  def callback_mode, do: :state_functions

  @impl true
  def init([]) do
    case Feed.Nif.ck_register() do
      :ok -> {:ok, :idle, []}
      :error -> {:stop, :ck_register_failed}
    end
  end

  def idle(:info, :ck_due, waiters), do: {:next_state, :bulk, waiters, [{:next_event, :internal, :run}]}
  def idle(:info, {:wait, ref, pid}, waiters), do: release(pid, ref) |> then(fn :ok -> {:keep_state, waiters} end)
  def idle(_type, _msg, _waiters), do: :keep_state_and_data

  def bulk(:internal, :run, waiters) do
    _ = Feed.Nif.ck_bulk()
    {:next_state, :tail, waiters, [{:next_event, :internal, :run}]}
  end

  def bulk(:info, {:wait, ref, pid}, waiters), do: {:keep_state, [{ref, pid} | waiters]}
  def bulk(_type, _msg, _waiters), do: :keep_state_and_data

  def tail(:internal, :run, waiters) do
    _ = Feed.Nif.ck_tail()
    Enum.each(waiters, fn {ref, pid} -> release(pid, ref) end)
    {:next_state, :idle, []}
  end

  def tail(:info, {:wait, ref, pid}, waiters), do: {:keep_state, [{ref, pid} | waiters]}
  def tail(_type, _msg, _waiters), do: :keep_state_and_data

  defp release(pid, ref) do
    send(pid, {:released, ref})
    :ok
  end
end
