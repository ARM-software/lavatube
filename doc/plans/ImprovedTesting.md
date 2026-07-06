# Improve testing

## Image comparisons

We have some image out tests already but currently only test image out for
basic file validity rather than check its actual visual contents. Once we
have more useful checking, we could add a lot more image out tests.

### Automatic

Storing golden images for pixel-perfect comparisons will not work well since
we run tests on different GPUs. Simple fuzzing is not enough to solve this.
We could add a perceptual diffing algorithm, though, but there is nothing
availble out-of-the-box for this as far as I know.

### Manual

We can generate a custom static HTML from the ctest results for the user
to quickly manually inspect - comparing golden known good image against the
produced image from the test run.

ctest already supports machine-readable outputs you can build on, including
--show-only=json-v1 for test metadata and --output-junit for result XML, so
a small script can map tests to produced PNGs and emit a visual report.

What we can do:

  1. Label a subset of tests as visual.
  2. Give each one a deterministic screenshot prefix.
  3. After ctest, run a script that copies screenshots into build/visual-report/
   and generates an index page with pass/fail status and thumbnails.
  4. Keep --output-junit for CI summary, and publish the HTML folder as an artifact.

## Test splitting

We should label tests so we don't have to run all of them every run. We can
start by labeling all GPU model tests and only run those if a particular
environment variable is set.
