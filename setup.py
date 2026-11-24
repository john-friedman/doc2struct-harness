from setuptools import find_packages, setup

setup(
    name="doc2struct-harness",
    author="John Friedman",
    version="0.1.0",
    description="LLM document parsing at a fraction of the cost",
    packages=find_packages(include=["harness_d2s", "harness_d2s.*"]),
    python_requires=">=3.10",
    install_requires=["doc2struct>=0.1.7", "aiohttp>=3.10", "markdown-it-py>=4,<5", "pypdfium2>=4", "Pillow>=10"],
    package_data={"harness_d2s": ["presets/*.json", "presets/*.txt", "models/*.json"]},
    include_package_data=True,
)
