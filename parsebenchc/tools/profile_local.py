"""Profile the installed scorer on a stratified sample of saved native results."""
import argparse
import cProfile
import pstats
from pathlib import Path

from parse_bench.evaluation.metrics.parse.next_metrics import compute_next_metrics
from parse_bench.schemas.pipeline_io import InferenceResult
from parse_bench.test_cases.loader import load_test_case


def samples(root, per_subset):
    for folder in sorted(Path(root).iterdir()):
        if not folder.is_dir() or folder.name == "references":
            continue
        paths = sorted(folder.glob("*.result.json"))
        for path in paths[:per_subset]:
            result = InferenceResult.model_validate_json(path.read_bytes())
            source = Path(result.request.source_file_path)
            local = Path(root) / "references" / folder.name / source.name
            case = load_test_case(local if local.exists() else source, product_type_hint="parse")
            if case is None:
                raise ValueError(f"Missing reference for {path}")
            yield result, case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_dir")
    parser.add_argument("--per-subset", type=int, default=3)
    parser.add_argument("--output", default="parsebenchc/build/profile.pstats")
    args = parser.parse_args()
    pairs = list(samples(args.results_dir, args.per_subset))
    profiler = cProfile.Profile()
    with profiler:
        for result, case in pairs:
            compute_next_metrics(result, case)
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    profiler.dump_stats(args.output)
    print(f"Documents: {len(pairs)}")
    pstats.Stats(profiler).strip_dirs().sort_stats("tottime").print_stats(35)


if __name__ == "__main__":
    main()
