#! /usr/bin/env python
# -*- coding: utf-8 -*-

import os

from lab.environments import LocalEnvironment, BaselSlurmEnvironment

import common_setup
from common_setup import IssueConfig, IssueExperiment
from relativescatter import RelativeScatterPlotReport

DIR = os.path.dirname(os.path.abspath(__file__))
BENCHMARKS_DIR = os.environ["DOWNWARD_BENCHMARKS"]
REVISIONS = ["issue786-v2-base", "issue786-v2"]
DRIVER_OPTIONS = ["--overall-time-limit", "5m"]
CONFIGS = [
    IssueConfig(
        "diverse-potentials",
        ["--search", "astar(diverse_potentials())"],
        driver_options=DRIVER_OPTIONS),
    IssueConfig(
        "sample-potentials",
        ["--search", "astar(sample_based_potentials())"],
        driver_options=DRIVER_OPTIONS),
    IssueConfig(
        "mutex_based_potential",
        ["--search", "astar(mutex_based_potential())"],
        driver_options=DRIVER_OPTIONS),
    IssueConfig(
        "mutex_based_ensemble_potential",
        ["--search", "astar(mutex_based_ensemble_potential())"],
        driver_options=DRIVER_OPTIONS),
    IssueConfig(
        "ipdb",
        ["--search", "astar(ipdb())"],
        driver_options=DRIVER_OPTIONS)
]
SUITE = common_setup.DEFAULT_OPTIMAL_SUITE
ENVIRONMENT = BaselSlurmEnvironment(
    partition="infai_1",
    email="jendrik.seipp@unibas.ch",
    export=["PATH", "DOWNWARD_BENCHMARKS"])

if common_setup.is_test_run():
    SUITE = IssueExperiment.DEFAULT_TEST_SUITE
    ENVIRONMENT = LocalEnvironment(processes=1)

exp = IssueExperiment(
    revisions=REVISIONS,
    configs=CONFIGS,
    environment=ENVIRONMENT,
)
exp.add_suite(BENCHMARKS_DIR, SUITE)

exp.add_parser(exp.EXITCODE_PARSER)
exp.add_parser(exp.TRANSLATOR_PARSER)
exp.add_parser(exp.SINGLE_SEARCH_PARSER)
exp.add_parser(exp.PLANNER_PARSER)

exp.add_step('build', exp.build)
exp.add_step('start', exp.start_runs)
exp.add_fetcher(name='fetch')

#exp.add_absolute_report_step()
exp.add_comparison_table_step()

for attribute in ["memory", "total_time"]:
    for config in CONFIGS:
        exp.add_report(
            RelativeScatterPlotReport(
                attributes=[attribute],
                filter_algorithm=["{}-{}".format(rev, config.nick) for rev in REVISIONS],
                get_category=lambda run1, run2: run1.get("domain")),
            outfile="{}-{}-{}-{}-{}.png".format(exp.name, attribute, config.nick, *REVISIONS))

exp.run_steps()
