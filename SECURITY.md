# Security policy

PolySparQ++ can be configured to handle credentials and submit real orders. Treat the code as experimental and review it before connecting any funded account.

## Reporting a vulnerability

Please email [jacobstephane@outlook.com](mailto:jacobstephane@outlook.com) with a concise description, affected files or versions, impact, and a safe reproduction if available. Avoid sending real private keys, API secrets, personal trading data, or production credentials. Do not publish exploit details before coordinating a fix.

## Operator guidance

- Keep the engine in paper mode while evaluating it.
- Store wallet keys and API credentials outside the repository with restrictive file permissions.
- Do not expose the cockpit's development HTTP server directly to the internet.
- Use a dedicated wallet with only the funds you can afford to lose if you independently decide to test live execution.
- Rotate any credential that may have been committed, logged, or shared.

Security reports are handled by the project maintainer; no response-time or patch-time guarantee is currently offered.

## Contact

Stephane Jacob · [jacobstephane@outlook.com](mailto:jacobstephane@outlook.com)
