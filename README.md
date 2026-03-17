# A&F Developers

Repository for the **Exodus Systems Inc. – Distributed Systems Engineering Program**.

This repository is managed under the **Exodus Engineering Workflow** used across all development teams participating in the course.

---

# Project Context

This repository is part of the **Exodus Systems Inc. Training Program** for the course:

**Sistemas Distribuidos – Ingeniería en Computación**

Students work as software engineers within a simulated company environment where they collaborate through modern development workflows, CI/CD pipelines and structured repository governance.

---

# Team Members

| Name              | GitHub               |
| ----------------- | -------------------- |
| Pavet Garcia, Aldana Micaela | @AldanaPavet |
| Soria, Federico Isaia | @BlastNeos |
|  | @ |

> The third member may be empty depending on team composition.

---

# Repository Structure

```

.
├── docs/               # Technical documentation
├── src/                # Source code
├── tests/              # Test suite
├── .github/            # CI/CD workflows and templates
├── README.md
└── LICENSE

```

Each team is free to extend the project structure depending on the needs of the implementation.

---

# Development Workflow

This repository follows a **trunk-based collaboration model with protected branches**.

```

feature/*  →  development  →  main

```

### Branch Roles

| Branch        | Purpose                             |
| ------------- | ----------------------------------- |
| `main`        | Stable release branch               |
| `development` | Integration branch for new features |
| `feature/*`   | Individual work branches            |

---

# Contribution Rules

1. **Never commit directly to `main`**
2. All work must happen in a **feature branch**
3. All merges must go through **Pull Requests**
4. CI checks must pass before merging

Example workflow:

```

git checkout development
git checkout -b feature/my-feature

# work

git commit
git push

# open PR → development

```

---

# Pull Requests

Every Pull Request must:

- Explain the purpose of the change
- Reference the related issue
- Pass CI checks
- Receive at least **one approval**

The repository enforces these rules automatically via **GitHub Rulesets**.

---

# Continuous Integration

All Pull Requests trigger automated checks:

- build
- test

The PR cannot be merged unless these checks succeed.

---

# Issue Management

Work is organized through **GitHub Issues**.

Typical issue types include:

- `type: feature`
- `type: bug`
- `type: docs`
- `type: chore`

Priority labels:

- `priority: high`
- `priority: medium`
- `priority: low`

---

# Engineering Principles

Teams are expected to follow standard engineering practices:

- small incremental commits
- readable commit messages
- testable code
- code reviews before merging
- documentation of architectural decisions

---

# Development Environment

Each team is responsible for defining its own local development environment depending on the technologies chosen for the implementation.

However, all repositories must maintain:

- reproducible builds
- automated tests
- CI compatibility

---

# License

This repository is part of an **academic engineering program** and is intended exclusively for educational purposes.

---

# Exodus Systems Inc.

> _Engineering the distributed systems of tomorrow._
