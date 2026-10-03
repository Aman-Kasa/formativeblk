#ifndef CONFIG_H
#define CONFIG_H

/* All Formative 2 tunables in one place, so the report and README can point
 * at a single file for every number the program uses. Coin amounts are whole
 * integers in the member ledgers; mining payouts (which can be fractional
 * after pool shares) are reported as doubles and never enter a member ledger. */

/* ---- Lending rules ------------------------------------------------------ */
#define LOAN_PERIOD_DAYS     14   /* return within this many days = on time   */
#define REWARD_ON_TIME       10   /* token reward for an on-time return       */
#define REWARD_LATE           5   /* token reward for a late return           */
#define REWARD_NONE           0   /* borrow / overdue: no reward transaction  */

/* ---- Transaction models -------------------------------------------------- */
#define TX_FEE                1   /* fixed fee deducted from every transaction */

/* ---- Proof of work -------------------------------------------------------- */
#define DEFAULT_DIFFICULTY    2   /* leading '0' hex characters required     */
#define MIN_DIFFICULTY        1
#define MAX_DIFFICULTY        4

/* ---- Mining economics ----------------------------------------------------- */
#define BLOCK_REWARD         50.0 /* coins paid to the miner(s) per block     */
#define POOL_FEE_PERCENT      2.0 /* operator fee taken before pool payout    */
#define POOL_DEFAULT_MINERS   4
#define POOL_MIN_MINERS       2
#define POOL_MAX_MINERS       8
#define POOL_MIN_HASHRATE    50   /* attempts per round, chosen at random     */
#define POOL_MAX_HASHRATE   400

#define CLOUD_MIN_ROUNDS      1
#define CLOUD_MAX_ROUNDS      5
#define CLOUD_RENTAL_FEE     20.0 /* fixed fee per rented round               */
#define CLOUD_MAINTENANCE_PERCENT 10.0 /* taken from each reward earned       */
#define CLOUD_HASHRATE     2000   /* attempts the rented rig makes per round  */

#endif
