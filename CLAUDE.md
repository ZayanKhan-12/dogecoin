# CLAUDE.md

Guidance for Claude Code and other AI assistants working in this repository.

## What this is

Dogecoin Core 1.14.99 (development toward 1.15), a fork of Bitcoin Core around the 0.13/0.14
line. Most files are Bitcoin's, and the productive way to read a change is to ask **"is this
Bitcoin's code or Dogecoin's divergence?"** — the two are maintained very differently.

The Dogecoin-specific code is small and deliberately concentrated:

| Path | Divergence |
| :--- | :--- |
| `src/dogecoin.cpp`, `src/dogecoin.h` | Difficulty adjustment, block subsidy, the `AllowDigishieldMinDifficulty` rules. |
| `src/dogecoin-fees.cpp`, `src/dogecoin-fees.h` | `GetDogecoinMinRelayFee`, `GetDogecoinDustFee`. Dogecoin's fee scale is not Bitcoin's. |
| `src/auxpow.cpp`, `src/auxpow.h` | Merged mining with Litecoin. Bitcoin has no equivalent. |
| `src/amount.h` | `MAX_MONEY` is 10,000,000,000 × `COIN`, not 21,000,000 × `COIN`. See below — this one is load-bearing. |
| `src/chainparams.cpp` | Consensus parameters, including the coinbase-maturity switch at height 145,000. |

Anything else is usually best understood by reading the corresponding Bitcoin Core code and its
history first.

## This is live consensus code

The most important fact about this repository is the cost of being wrong in it. Code under
`src/consensus/`, `src/validation.cpp`, `src/primitives/` and `src/script/` decides which blocks
a node accepts. A node that disagrees with the rest of the network about one transaction forks
off the chain. That is not a bug that gets noticed in review and fixed in the next release; it is
real money and a split network.

Consequences for how to work here:

- **A change that "looks more correct" is not thereby safe.** Any change to validation behaviour
  needs a deployment story — a height, a version bit, a soft or hard fork plan — before it needs
  an implementation.
- **Prefer analysis and tests to patches.** A test that demonstrates the current behaviour is
  almost always a welcome contribution and carries no consensus risk. A patch to the rule itself
  usually is not, and issue #327's history shows why: see below.
- **Do not widen or change the sign of a money type.** `CAmount` is `int64_t` and a great deal of
  code depends on it being *signed*; see the worked example below.

## Building and testing

The build is autotools (`./autogen.sh && ./configure && make`), and the unit tests are Boost:

```sh
make check                                # everything
src/test/test_dogecoin --run_test=<suite> # one suite
```

New unit test files must be listed in `src/Makefile.test.include`, or they are silently not
compiled. `src/test/README.md` documents the harness; `BasicTestingSetup` is the usual fixture.

CI is `.github/workflows/ci.yml`, a large matrix (i686/armhf/aarch64/x86_64 Linux, Windows,
macOS) built through `depends/` and running `make check`.

A note for anyone trying to build this on a recent machine: 1.14 predates the current toolchains
and needs OpenSSL 1.1 and Berkeley DB 4.8. OpenSSL 1.1 is end-of-life and no longer installable
from Homebrew, so a local macOS build of this branch is generally not achievable without
`depends/` or a container. Prefer `depends/`, a container, or CI. Do not "fix" a build error by
relaxing a dependency pin.

## Money arithmetic and issue #327

`CAmount` is `int64_t`, so it saturates at `INT64_MAX` koinu. Divided by `COIN`, that is
**92,233,720,368 DOGE** — the "92 billion" in the title of issue #327, open since 2014.

Two facts settle most questions about it, and they pull in opposite directions.

### The three consensus functions named in the issue are already guarded

The issue reports that inputs and outputs are "first checked against MAX_MONEY, then summed up
and then again checked", so that enough `MAX_MONEY` entries would wrap the total back into range.
That is not how any of the three named functions works today. Each checks `MoneyRange` on the
running total **inside** the loop, after every addition:

- `CTransaction::GetValueOut` — `src/primitives/transaction.cpp`
- `CheckTransaction` — `src/validation.cpp`
- `Consensus::CheckTxInputs` — `src/validation.cpp`

This is the CVE-2010-5139 fix, inherited from Bitcoin. A transaction of `MAX_MONEY` outputs is
rejected at the *second* output, eight short of the tenth, where an int64 would first overflow.
`src/test/amount_overflow_tests.cpp` pins this down, including the specific wrapped total that an
end-of-loop-only check would have accepted.

### But MAX_MONEY is not a supply cap, and that assumption is inherited

In Bitcoin, `MAX_MONEY` is both the per-transaction ceiling and the total that will ever exist, so
"every value is within `MoneyRange`" also bounds any aggregate of real money. **In Dogecoin that
is false.** `MAX_MONEY` caps one transaction at 10 billion DOGE, while the supply is unbounded —
10,000 DOGE per block, forever — and passed 92.23 billion years ago.

So any sum that ranges wider than a single transaction needs its own reasoning rather than the
inherited assumption. The case that matters most was already handled: `gettxoutsetinfo`
accumulates the whole UTXO set into an `arith_uint256`, not a `CAmount`
(`src/rpc/blockchain.cpp`). If you add another whole-chain or whole-wallet aggregate, do the same
— and note that `CWallet::GetBalance` and friends still accumulate into a `CAmount` without a
range check on the running total, which is sound only while no single wallet holds more than
92.23 billion DOGE.

### Why `CAmount` must not become unsigned

The one attempted fix, PR #3743, changed `CAmount` from `int64_t` to `uint64_t` and raised
`MAX_MONEY`. A maintainer closed it with the decisive question: *"How does it still allow negative
values when it's uint64_t?"*

Negative `CAmount` values are load-bearing throughout:

- `MoneyRange` is `(nValue >= 0 && nValue <= MAX_MONEY)` — the first half becomes vacuously true.
- `CheckTransaction` rejects `txout.nValue < 0` — unreachable.
- `CheckTxInputs` computes `nTxFee = nValueIn - tx.GetValueOut()` and rejects `nTxFee < 0` —
  unreachable, and the subtraction now wraps to an enormous positive fee.
- `CFeeRate` is deliberately exercised with negative amounts in `src/test/amount_tests.cpp`.

Making the type unsigned does not remove the overflow; it removes the *detection* of it, in
consensus code, silently. If #327 is ever addressed by a type change, the type has to get wider
while staying signed.
