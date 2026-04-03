"""
CrewAI Example: Research and verify a factual claim.

This is the standard CrewAI way to coordinate agents.
Requires: pip install crewai (+ OpenAI API key for real execution)
"""
from crewai import Agent, Task, Crew, Process

# Define agents (each will call an LLM)
researcher = Agent(
    role="Senior Researcher",
    goal="Find accurate factual information",
    backstory="You are an expert researcher who always verifies sources.",
    verbose=True,
)

fact_checker = Agent(
    role="Fact Checker",
    goal="Verify claims against known facts",
    backstory="You are meticulous about accuracy and never accept unverified claims.",
    verbose=True,
)

reporter = Agent(
    role="Reporter",
    goal="Compile verified facts into a clear report",
    backstory="You write concise factual reports.",
    verbose=True,
)

# Define tasks
research_task = Task(
    description="Research: Where and when was Doctor Who created? Who created it?",
    expected_output="Factual answer with source references",
    agent=researcher,
)

verify_task = Task(
    description="Verify the research findings. Check each claim against known facts.",
    expected_output="Verified or disputed findings with evidence",
    agent=fact_checker,
    context=[research_task],
)

report_task = Task(
    description="Write a one-paragraph factual summary of the verified findings.",
    expected_output="A concise factual paragraph",
    agent=reporter,
    context=[verify_task],
)

# Assemble crew
crew = Crew(
    agents=[researcher, fact_checker, reporter],
    tasks=[research_task, verify_task, report_task],
    process=Process.sequential,
    verbose=True,
)

# Run (requires OpenAI API key)
# result = crew.kickoff()
# print(result)

# What this actually does:
# 1. Calls GPT-4 with researcher prompt -> gets text back (no verification)
# 2. Calls GPT-4 with fact_checker prompt + research output (LLM checks LLM)
# 3. Calls GPT-4 with reporter prompt + verified output (LLM summarizes LLM)
# 4. Returns the final text. No cryptographic proof. No ontology grounding.
#    "Verification" = another LLM agreeing with the first LLM.
#
# Lines of code: 55
# Dependencies: crewai, openai, pydantic, + 30 transitive
# API calls: 3 (at ~$0.01 each = $0.03 per run)
# Verification: None (LLM self-review)
# Provenance: None
