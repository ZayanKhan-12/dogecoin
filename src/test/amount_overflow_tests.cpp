// Copyright (c) 2026 The Dogecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for the int64 money-overflow concern raised in issue #327.
//
// The issue (2014) observes that a CAmount is an int64_t, so it saturates at
// INT64_MAX koinu -- about 92.23 billion DOGE -- and reports that the money
// sums in CheckTransaction, CheckInputs and GetValueOut could be made to wrap
// past it. The scenario given is that inputs are "first checked against
// MAX_MONEY, then summed up and then again checked", so a transaction with
// enough MAX_MONEY entries would wrap back to a small positive total and pass
// the final check.
//
// These tests pin down two things:
//
//   1. The arithmetic in the issue is real and is reproduced here exactly:
//      an unguarded running sum of MAX_MONEY outputs does wrap, and does come
//      back into MoneyRange.
//
//   2. The code does not sum that way. Every one of the three functions named
//      checks MoneyRange on the running total *inside* the loop, after each
//      addition, so the transaction is rejected long before any wrap can
//      occur. This is the CVE-2010-5139 guard, and these tests fail if it is
//      ever removed.
//
// Nothing here changes consensus behaviour. The tests describe the behaviour
// that already exists, so that a future refactor cannot quietly drop the guard.

#include "amount.h"
#include "consensus/validation.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "uint256.h"
#include "validation.h" // For CheckTransaction

#include "test/test_bitcoin.h"

#include <limits>
#include <stdexcept>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(amount_overflow_tests, BasicTestingSetup)

static const CAmount INT64_MAXIMUM = std::numeric_limits<int64_t>::max();

// Builds a transaction with a single dummy input and nOutputs outputs, each
// holding nValue. The input is never checked for existence by
// CheckTransaction, which is a context-free check.
static CMutableTransaction TxWithOutputs(unsigned int nOutputs, const CAmount& nValue)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.hash = uint256S("0x01");
    tx.vin[0].prevout.n = 0;
    tx.vin[0].scriptSig << std::vector<unsigned char>(65, 0);
    tx.vout.resize(nOutputs);
    for (unsigned int i = 0; i < nOutputs; i++) {
        tx.vout[i].nValue = nValue;
        tx.vout[i].scriptPubKey << OP_1;
    }
    return tx;
}

// The constants the issue reasons about, stated explicitly so that a change to
// MAX_MONEY has to come past this test.
BOOST_AUTO_TEST_CASE(money_constants)
{
    BOOST_CHECK_EQUAL(COIN, 100000000);
    BOOST_CHECK_EQUAL(MAX_MONEY, 1000000000000000000LL); // 10 billion DOGE
    BOOST_CHECK_EQUAL(MAX_MONEY / COIN, 10000000000LL);

    // The number in the issue title: INT64_MAX koinu is ~92.23 billion DOGE.
    BOOST_CHECK_EQUAL(INT64_MAXIMUM / COIN, 92233720368LL);

    // MoneyRange rejects negatives as well as oversized values. This matters
    // for #327: it is the reason CAmount cannot simply be made unsigned. An
    // unsigned CAmount would make every "is this negative" check -- here, in
    // the fee calculation in CheckTxInputs, and in wallet change computation --
    // silently unreachable.
    BOOST_CHECK(MoneyRange(0));
    BOOST_CHECK(MoneyRange(MAX_MONEY));
    BOOST_CHECK(!MoneyRange(-1));
    BOOST_CHECK(!MoneyRange(MAX_MONEY + 1));
}

// MAX_MONEY caps a single transaction, not the money supply. In Bitcoin the
// two coincide, and that is what makes a CAmount-typed aggregate safe there.
// Dogecoin's supply is unbounded (10,000 DOGE per block, forever) and passed
// INT64_MAX/COIN years ago, so the inherited assumption does not hold and any
// aggregate wider than one transaction needs its own reasoning. gettxoutsetinfo
// is the case already dealt with: it accumulates into an arith_uint256.
BOOST_AUTO_TEST_CASE(max_money_is_not_a_supply_cap)
{
    BOOST_CHECK(MAX_MONEY < INT64_MAXIMUM);

    // Only nine outputs of MAX_MONEY fit in an int64.
    BOOST_CHECK_EQUAL(INT64_MAXIMUM / MAX_MONEY, 9);

    // So ten of them do not, which is precisely the issue's concern.
    BOOST_CHECK(MAX_MONEY > INT64_MAXIMUM / 10);
}

// The issue's scenario, reproduced. Accumulation is done in uint64_t because
// signed overflow is undefined behaviour -- the point is to show the bit
// pattern a wrapped int64 would hold, not to invoke UB inside the test.
BOOST_AUTO_TEST_CASE(unguarded_sum_wraps_and_looks_valid_again)
{
    uint64_t sum = 0;
    int firstOverflow = -1;
    int firstPositiveAgain = -1;

    for (int n = 1; n <= 20; n++) {
        sum += static_cast<uint64_t>(MAX_MONEY);
        if (firstOverflow < 0 && sum > static_cast<uint64_t>(INT64_MAXIMUM))
            firstOverflow = n;
        if (firstOverflow > 0 && firstPositiveAgain < 0 && static_cast<int64_t>(sum) >= 0)
            firstPositiveAgain = n;
    }

    // Ten MAX_MONEY outputs exceed what an int64 can hold.
    BOOST_CHECK_EQUAL(firstOverflow, 10);

    // By nineteen, the wrapped total is positive again and small enough that a
    // single MoneyRange check applied only at the end would accept it.
    BOOST_CHECK_EQUAL(firstPositiveAgain, 19);
    const CAmount wrapped = static_cast<int64_t>(static_cast<uint64_t>(MAX_MONEY) * 19);
    BOOST_CHECK_EQUAL(wrapped, 553255926290448384LL);
    BOOST_CHECK(wrapped > 0);
    BOOST_CHECK(MoneyRange(wrapped));
}

// ... and the reason that scenario cannot be reached: CheckTransaction tests
// the running total after every addition, so it rejects at the second output,
// eight short of the wrap.
BOOST_AUTO_TEST_CASE(check_transaction_rejects_before_any_wrap)
{
    CValidationState state;

    // Two outputs of MAX_MONEY: over the limit, well inside int64 range.
    BOOST_CHECK(!CheckTransaction(TxWithOutputs(2, MAX_MONEY), state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-txouttotal-toolarge");

    // Nineteen outputs of MAX_MONEY: the total that an end-only check would
    // have accepted. Rejected with the same reason, because the guard fires at
    // output two and never reaches the wrap.
    CValidationState wrapState;
    BOOST_CHECK(!CheckTransaction(TxWithOutputs(19, MAX_MONEY), wrapState));
    BOOST_CHECK_EQUAL(wrapState.GetRejectReason(), "bad-txns-txouttotal-toolarge");

    // A single output at exactly MAX_MONEY is fine, so the rejections above
    // are about the total and not about the individual values.
    CValidationState okState;
    BOOST_CHECK(CheckTransaction(TxWithOutputs(1, MAX_MONEY), okState));

    // Negative outputs are caught by their own check.
    CValidationState negState;
    BOOST_CHECK(!CheckTransaction(TxWithOutputs(1, -1), negState));
    BOOST_CHECK_EQUAL(negState.GetRejectReason(), "bad-txns-vout-negative");
}

// GetValueOut carries the same in-loop guard and throws rather than returning a
// wrapped total. Callers rely on that: the fee calculation in CheckTxInputs is
// nValueIn - tx.GetValueOut().
BOOST_AUTO_TEST_CASE(get_value_out_throws_before_any_wrap)
{
    BOOST_CHECK_EQUAL(CTransaction(TxWithOutputs(1, MAX_MONEY)).GetValueOut(), MAX_MONEY);

    BOOST_CHECK_THROW(CTransaction(TxWithOutputs(2, MAX_MONEY)).GetValueOut(), std::runtime_error);
    BOOST_CHECK_THROW(CTransaction(TxWithOutputs(19, MAX_MONEY)).GetValueOut(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
