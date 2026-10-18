# SC2KOS planning model

`planning_model.ksc` is a generated 12-month business plan containing 644
populated cells. It is intended both as a useful worked example and as a
realistic performance workload.

The model includes:

- monthly unit sales, prices, product sales and service revenue;
- product and service costs, gross profit and gross margin;
- payroll, rent, marketing, utilities and other operating expenses;
- operating profit, taxes, net income and capital spending;
- opening and closing cash with monthly debt service;
- a loan schedule using `PMT`, interest, principal and closing balance;
- revenue per unit, variable cost per unit and break-even volume;
- revenue and profit growth, debt coverage and cash-positive flags;
- cumulative income, cumulative operating cash flow and discounted cash flow;
- annual totals, ending balances and monthly averages.

Copy the file to the KonaOS filesystem and open it with:

```text
sc2kos /path/planning_model.ksc
```

Regenerate and validate it on the host with `make demo`.
